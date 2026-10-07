// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_async.c - asynchronous daemon submit worker (see nr_async.h).
 *
 *   present thread                    worker thread              m11d
 *   --------------                    -------------            ----
 *   submit(px copy) --> [pend ring] --> pop --> exchange() ----> frame
 *   take_done <------ [done slot] <-- publish reply <--------- reply
 *
 * Every crossing is one critical section; the frame copy on submit is
 * the only present-thread cost (~1 ms at 800x600).
 * ==========================================================================*/
#include "nr_async.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "nr_util.h"
#include "daemon_client.h"   /* default transport: exchange() */

/* Pool sizing: one buffer in flight in the worker (TWO for a PROC request
 * - the reply lands in a second slot so the submitted source survives to
 * be published as done.src), up to NR_ASYNC_PEND queued, two in the done
 * slot (reply+source), one held by the caller mid-swap, one fgi slot -
 * plus slack so a submit never waits on a buffer. ~2 MB each at 800x600,
 * lazily allocated on first use. */
#define NR_ASYNC_BUFS 12
/* Pending queue depth: PROC submissions are the common case; FG requests
 * ride along behind their anchor without letting midpoints starve the
 * stream. Bounded - overflow evicts, never grows. */
#define NR_ASYNC_PEND 3

typedef struct NrAsyncReq {
    int kind;
    int buf;                    /* pool index holding the payload copy */
    uint64_t chain;
    uint32_t hdr[4];            /* verbatim wire header (magic,w,h,fmt) */
    size_t n;                   /* payload bytes submitted (send size) */
    unsigned long seq;
} NrAsyncReq;

struct NrAsync {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    HANDLE thread;
    volatile LONG quit;
    nr_async_exchange_fn xchg;
    void *xctx;
    /* Buffer pool: free list as an index stack; a buffer outside the
     * free list is exclusively owned by whoever took it (submit copy,
     * in-flight worker, done slot, caller between take/give). */
    unsigned char *buf[NR_ASYNC_BUFS];
    size_t bufsz[NR_ASYNC_BUFS];
    int freeb[NR_ASYNC_BUFS];
    int nfree;
    /* Pending requests, FIFO ring of pool-slot indexes into req[]. */
    NrAsyncReq req[NR_ASYNC_PEND];
    int head, count;
    /* Latest-completed slot (done_set==0 when empty). */
    int done_set;
    NrAsyncDone done;
    int done_buf;               /* pool index owned by done.buf, -1 none */
    int done_src;               /* pool index owned by done.src, -1 none */
    int inflight;               /* worker is inside an exchange (pacing) */
    unsigned long seq_next;
    unsigned long dropped;
    unsigned long submitted;
};

static int real_xchg(void *ctx, const void *hdr, size_t hdr_sz,
                     const void *payload, size_t payload_sz,
                     void *reply, size_t reply_sz)
{
    (void)ctx;
    return exchange(hdr, hdr_sz, payload, payload_sz, reply, reply_sz);
}

static int pop_free_locked(NrAsync *a)
{
    if (!a->nfree) return -1;
    return a->freeb[--a->nfree];
}

static void push_free_locked(NrAsync *a, int idx)
{
    if (idx < 0 || idx >= NR_ASYNC_BUFS) return;
    a->freeb[a->nfree++] = idx;
}

/* Evict one pending request to make room. A pending FG request is the
 * most expendable (a lost midpoint just costs a held-anchor present);
 * otherwise the oldest pending entry goes. Returns the freed pool index,
 * or -1 when the queue was not full. cs held. */
static int evict_locked(NrAsync *a)
{
    int slot = -1, i;
    if (a->count < NR_ASYNC_PEND) return -1;
    for (i = 0; i < a->count; i++) {
        int at = (a->head + i) % NR_ASYNC_PEND;
        if (a->req[at].kind != NR_ASYNC_PROC) { slot = at; break; }
    }
    if (slot < 0) slot = a->head;
    {
        NrAsyncReq *r = &a->req[slot];
        int freed = r->buf;
        int last = (a->head + a->count - 1) % NR_ASYNC_PEND;
        nr_log("[nr_async] evicted req kind=%d seq=%lu (queue full)",
               r->kind, r->seq);
        /* Compact the ring: entries after slot step one toward head. */
        for (i = slot; i != last; i = (i + 1) % NR_ASYNC_PEND)
            a->req[i] = a->req[(i + 1) % NR_ASYNC_PEND];
        a->count--;
        a->dropped++;
        return freed;
    }
}

static DWORD WINAPI nr_async_worker(LPVOID param)
{
    NrAsync *a = (NrAsync *)param;
    for (;;) {
        NrAsyncReq r;
        int erc, rbuf = -1;
        EnterCriticalSection(&a->cs);
        /* <= 0, not == 0: belt-and-braces so even a corrupt count can
         * never produce a phantom pop (spurious wakes re-evaluate the
         * predicate anyway). */
        while (!a->quit && a->count <= 0)
            SleepConditionVariableCS(&a->cv, &a->cs, INFINITE);
        if (a->quit) { LeaveCriticalSection(&a->cs); break; }
        r = a->req[a->head];
        a->head = (a->head + 1) % NR_ASYNC_PEND;
        a->count--;
        a->inflight = 1;
        /* A PROC request additionally claims a second pool slot for the
         * reply: the payload buffer then survives the round-trip intact
         * and is published as done.src (the residual module needs the
         * exact frame this reply answers to form proc - src). FG replies
         * are display content only - no source kept. On pool exhaustion
         * the exchange falls back to the aliased in-place form and
         * done.src is NULL. */
        if (r.kind == NR_ASYNC_PROC)
            rbuf = pop_free_locked(a);
        LeaveCriticalSection(&a->cs);

        /* The payload is fully sent before the first reply byte arrives,
         * so the reply may land in the same buffer when no second slot is
         * available (the aliasing the sync path already relies on for
         * data->mapped). Sizes come from the wire header, not the pool
         * buffer's capacity - a slot grown for a bigger earlier extent
         * must not over-read the reply. */
        {
            const size_t rx = (size_t)r.hdr[1] * r.hdr[2] * 4;
            if (rbuf >= 0 && a->bufsz[rbuf] < rx) {
                /* Grow while exclusively owned; another thread can pop a
                 * DIFFERENT slot concurrently without touching this one. */
                unsigned char *grown =
                    (unsigned char *)realloc(a->buf[rbuf], rx ? rx : 1);
                if (grown) {
                    a->buf[rbuf] = grown;
                    a->bufsz[rbuf] = rx;
                } else {
                    EnterCriticalSection(&a->cs);
                    push_free_locked(a, rbuf);
                    LeaveCriticalSection(&a->cs);
                    rbuf = -1;
                }
            }
            if (rbuf >= 0)
                erc = a->xchg(a->xctx, r.hdr, sizeof r.hdr,
                              a->buf[r.buf], r.n, a->buf[rbuf], rx);
            else
                erc = a->xchg(a->xctx, r.hdr, sizeof r.hdr,
                              a->buf[r.buf], r.n, a->buf[r.buf], rx);
        }

        EnterCriticalSection(&a->cs);
        /* Latest-wins: an unconsumed record is superseded - its buffers
         * (reply AND source) recycle with it. */
        if (a->done_set) {
            if (a->done_buf >= 0) push_free_locked(a, a->done_buf);
            if (a->done_src >= 0) push_free_locked(a, a->done_src);
        }
        a->done.kind = r.kind;
        a->done.erc = erc;
        a->done.seq = r.seq;
        a->done.chain = r.chain;
        a->done.w = r.hdr[1];
        a->done.h = r.hdr[2];
        a->done.fmt = r.hdr[3];
        a->done.done_ms = nr_now_ms();
        a->done.src = NULL;
        a->done.src_bytes = 0;
        a->done_src = -1;
        if (erc == 0) {
            const size_t rx = (size_t)r.hdr[1] * r.hdr[2] * 4;
            if (rbuf >= 0) {
                a->done.buf = a->buf[rbuf];
                a->done.buf_bytes = rx;
                a->done_buf = rbuf;
                a->done.src = a->buf[r.buf];
                a->done.src_bytes = r.n;
                a->done_src = r.buf;
            } else {
                a->done.buf = a->buf[r.buf];
                a->done.buf_bytes = rx;
                a->done_buf = r.buf;
            }
        } else {
            a->done.buf = NULL;
            a->done.buf_bytes = 0;
            a->done_buf = -1;
            push_free_locked(a, r.buf);
            if (rbuf >= 0)
                push_free_locked(a, rbuf);
        }
        a->done_set = 1;
        a->inflight = 0;
        LeaveCriticalSection(&a->cs);
    }
    return 0;
}

NrAsync *nr_async_create_ex(nr_async_exchange_fn fn, void *ctx)
{
    NrAsync *a;
    int i;
    if (!fn) return NULL;
    a = (NrAsync *)calloc(1, sizeof *a);
    if (!a) return NULL;
    InitializeCriticalSection(&a->cs);
    InitializeConditionVariable(&a->cv);
    a->xchg = fn;
    a->xctx = ctx;
    a->done_buf = -1;
    a->done_src = -1;
    for (i = 0; i < NR_ASYNC_BUFS; i++)
        push_free_locked(a, i);          /* lazily sized on first use */
    a->thread = CreateThread(NULL, 0, nr_async_worker, a, 0, NULL);
    if (!a->thread) {
        DeleteCriticalSection(&a->cs);
        free(a);
        return NULL;
    }
    return a;
}

NrAsync *nr_async_create(void)
{
    return nr_async_create_ex(real_xchg, NULL);
}

void nr_async_destroy(NrAsync *a)
{
    int i;
    if (!a) return;
    EnterCriticalSection(&a->cs);
    a->quit = 1;
    WakeAllConditionVariable(&a->cv);
    LeaveCriticalSection(&a->cs);
    /* The worker only leaves its sleep to run one bounded exchange
     * (1.5 s socket timeouts), so the join is bounded in practice. A
     * wedged worker is abandoned - do NOT free the struct in that case:
     * it would still touch the pool. One tiny leak at teardown beats a
     * use-after-free inside a game process. */
    if (a->thread) {
        if (WaitForSingleObject(a->thread, 4000) != WAIT_OBJECT_0) {
            nr_log("[nr_async] worker did not exit in time; abandoning");
            return;
        }
        CloseHandle(a->thread);
    }
    for (i = 0; i < NR_ASYNC_BUFS; i++)
        free(a->buf[i]);
    DeleteCriticalSection(&a->cs);
    free(a);
}

int nr_async_submit(NrAsync *a, int kind, uint64_t chain,
                    const uint32_t hdr[4], const unsigned char *px, size_t n,
                    unsigned long *seq_out)
{
    int bi = -1, queued = 1;
    NrAsyncReq *r;
    if (!a || !px || !n || !hdr) return -1;
    EnterCriticalSection(&a->cs);
    if (a->quit) { LeaveCriticalSection(&a->cs); return -1; }
    /* FG requests never evict a PROC (they must stay behind their anchor
     * frame on the wire): if the queue is full and holds no FG entry,
     * the new FG request is the thing that drops. */
    if (a->count >= NR_ASYNC_PEND && kind != NR_ASYNC_PROC) {
        int has_fg = 0, i;
        for (i = 0; i < a->count; i++)
            if (a->req[(a->head + i) % NR_ASYNC_PEND].kind != NR_ASYNC_PROC)
                { has_fg = 1; break; }
        if (!has_fg) queued = 0;
    }
    if (queued && a->count >= NR_ASYNC_PEND) {
        /* The RING is what bounds the queue - not the buffer pool (the
         * pool is deliberately larger). Evicting here frees the slot AND
         * hands back the evicted request's buffer for the new payload;
         * skipping this on a non-empty pool would wrap the write index
         * onto the head and corrupt FIFO order. */
        bi = evict_locked(a);
        if (bi < 0) queued = 0;
    }
    if (queued && bi < 0) {
        bi = pop_free_locked(a);
        /* Pool exhausted while ring space remains: every buffer is
         * checked out (in-flight/done/caller-held) - legitimate drop. */
        if (bi < 0) queued = 0;
    }
    if (!queued) {
        a->dropped++;
        LeaveCriticalSection(&a->cs);
        return -1;
    }
    {
        /* The reply writes w*h*4 bytes back into this buffer regardless
         * of the outbound payload size - size for the larger of the two. */
        const size_t rx = (size_t)hdr[1] * hdr[2] * 4;
        const size_t need = n > rx ? n : rx;
        if (a->bufsz[bi] < need) {
            unsigned char *grown = (unsigned char *)realloc(a->buf[bi], need);
            if (!grown) {
                push_free_locked(a, bi);
                LeaveCriticalSection(&a->cs);
                return -1;
            }
            a->buf[bi] = grown;
            a->bufsz[bi] = need;
        }
    }
    memcpy(a->buf[bi], px, n);
    r = &a->req[(a->head + a->count) % NR_ASYNC_PEND];
    r->kind = kind;
    r->buf = bi;
    r->chain = chain;
    memcpy(r->hdr, hdr, sizeof r->hdr);
    r->n = n;
    r->seq = ++a->seq_next;
    a->count++;
    a->submitted++;
    if (seq_out) *seq_out = r->seq;
    WakeConditionVariable(&a->cv);
    LeaveCriticalSection(&a->cs);
    return 0;
}

int nr_async_take_done(NrAsync *a, NrAsyncDone *out)
{
    int got = 0;
    if (!a || !out) return 0;
    EnterCriticalSection(&a->cs);
    if (a->done_set) {
        *out = a->done;
        a->done_set = 0;
        a->done_buf = -1;      /* ownership moved to *out.buf / *out.src */
        a->done_src = -1;
        got = 1;
    }
    LeaveCriticalSection(&a->cs);
    return got;
}

void nr_async_give_buf(NrAsync *a, unsigned char *buf)
{
    int i;
    if (!a || !buf) return;
    EnterCriticalSection(&a->cs);
    for (i = 0; i < NR_ASYNC_BUFS; i++)
        if (a->buf[i] == buf) { push_free_locked(a, i); break; }
    if (i == NR_ASYNC_BUFS) {
        /* Not a pool pointer - should never happen; freeing it would be
         * safer than leaking it into the caller's ownership forever. */
        nr_log("[nr_async] give_buf of non-pool pointer %p - freed",
               (void *)buf);
        free(buf);
    }
    LeaveCriticalSection(&a->cs);
}

int nr_async_claim(NrAsync *a, unsigned char *buf)
{
    int i, claimed = 0;
    if (!a || !buf) return 0;
    EnterCriticalSection(&a->cs);
    for (i = 0; i < NR_ASYNC_BUFS; i++) {
        if (a->buf[i] == buf) {
            /* Only re-list slots actually checked out - the done slot's
             * buffer is pool-owned already and must not double-list. */
            int j, listed = 0;
            for (j = 0; j < a->nfree; j++)
                if (a->freeb[j] == i) { listed = 1; break; }
            if (!listed) push_free_locked(a, i);
            claimed = 1;
            break;
        }
    }
    LeaveCriticalSection(&a->cs);
    return claimed;
}

unsigned long nr_async_dropped(const NrAsync *a)
{
    return a ? a->dropped : 0;
}

unsigned long nr_async_submitted(const NrAsync *a)
{
    return a ? a->submitted : 0;
}

int nr_async_pending(NrAsync *a)
{
    int n;
    if (!a) return 0;
    EnterCriticalSection(&a->cs);
    n = a->count;
    LeaveCriticalSection(&a->cs);
    return n;
}

int nr_async_busy(NrAsync *a)
{
    int n;
    if (!a) return 0;
    EnterCriticalSection(&a->cs);
    n = a->count + a->inflight;
    LeaveCriticalSection(&a->cs);
    return n;
}
