// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_async.h - asynchronous daemon submit for the TCP layer path.
 *
 * Problem: the m11d exchange is a full TCP round-trip (~57-65 ms at
 * 800x600) executed inside vkQueuePresentKHR - the game's own present
 * blocks on daemon throughput, which is what caps GTA IV at ~15 fps and
 * trips Windows' "Not Responding" watch during loading screens.
 *
 * This module moves the round-trip to a worker thread. The present path
 * only enqueues a copy of the captured frame (bounded mailbox, drop-
 * oldest) and displays the newest completed reply it already holds -
 * the game runs at its own speed; the processed stream lags behind it by
 * roughly one daemon-frame.
 *
 * Ordering: the worker serializes requests on a single socket exchange
 * at a time and the daemon is itself serial, so replies always complete
 * in submission order; the latest-wins done slot therefore can never
 * publish an older result over a newer one.
 *
 * Wire protocol is untouched: requests are the caller's verbatim 16-byte
 * header + w*h*4 BGRA payload, replies are w*h*4 bytes.
 *
 * Concurrency model: one producer (the presenting thread), one consumer
 * (the worker). All shared state lives behind an embedded critical
 * section; buffers travel through a fixed pool so ownership is explicit
 * (submit takes a free buffer, a completed reply hands it to the caller,
 * the caller returns it via nr_async_give_buf). No file-scope globals.
 * ==========================================================================*/
#ifndef NR_ASYNC_H
#define NR_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Request kinds: which wire payload this is and where the reply belongs.
 * The caller builds the 16-byte header (incl. magic) - kind only steers
 * queue-eviction policy and the done record. */
enum {
    NR_ASYNC_PROC = 0,   /* NRN0/NRN1 process frame -> the held result */
    NR_ASYNC_FGT  = 1,   /* NRN2 residual-transfer -> one-shot display */
    NR_ASYNC_FGI  = 2    /* NRN3 midpoint interpolate -> one-shot display */
};

typedef struct NrAsync NrAsync;

/* Newest completed request (latest-wins mailbox). On erc==0 buf holds the
 * reply pixels and ownership moves to the caller - return it via
 * nr_async_give_buf when done with it. On erc!=0 buf is NULL and the
 * record is a pure transport report (same rc values as exchange()).
 * For PROC replies that were received into a second pool buffer, src also
 * holds the exact submitted payload (the raw frame this reply answers) -
 * caller-owned the same way; NULL for FG replies, failures, and when the
 * pool had no spare buffer (the reply then landed in the payload buffer
 * and the source is gone). nr_residual consumes (src, buf) pairs. */
typedef struct NrAsyncDone {
    int kind;              /* NR_ASYNC_* of the originating request */
    int erc;               /* 0 ok, -1 transport, -2 connect refused */
    unsigned long seq;     /* submission sequence (replies arrive in order) */
    uint64_t chain;        /* the swapchain the request was submitted for */
    uint32_t w, h, fmt;    /* extent + VkFormat stamped from the header */
    unsigned char *buf;    /* reply pixels (caller-owned until give_buf) */
    size_t buf_bytes;
    unsigned char *src;    /* submitted source frame (caller-owned, may be NULL) */
    size_t src_bytes;
    uint64_t done_ms;      /* nr_now_ms-style tick when the reply landed */
} NrAsyncDone;

/* Pluggable transport for tests: same contract as daemon_client's
 * exchange() - send header+payload, read exactly reply_sz bytes. */
typedef int (*nr_async_exchange_fn)(void *ctx, const void *hdr,
                                    size_t hdr_sz, const void *payload,
                                    size_t payload_sz, void *reply,
                                    size_t reply_sz);

NrAsync *nr_async_create(void);                       /* real exchange() */
NrAsync *nr_async_create_ex(nr_async_exchange_fn fn, void *ctx);
/* Signals the worker, joins it (bounded wait) and frees the pool. A
 * worker wedged inside a socket op is abandoned rather than joined
 * forever - the struct is leaked in that case only (safe-by-design:
 * it is one small block per device, at teardown). */
void nr_async_destroy(NrAsync *a);

/* Queue a request from the present path. Copies px into a pooled buffer
 * and returns - the daemon is never touched here. Bounded: at most
 * NR_ASYNC_PEND entries wait; on overflow a pending FG request is evicted
 * first (midpoints are expendable), then the oldest pending entry, so a
 * PROC submit only ever fails on pool exhaustion. A FGT/FGI submit that
 * would evict a PROC drops itself instead (it must stay behind its
 * anchor frame). Returns 0 queued, -1 not queued (drop; caller shows
 * the held/native fallback). *seq_out gets the submission sequence. */
int nr_async_submit(NrAsync *a, int kind, uint64_t chain,
                    const uint32_t hdr[4], const unsigned char *px, size_t n,
                    unsigned long *seq_out);

/* Move the newest completed record out. 1 = *out filled (caller owns the
 * fields above), 0 = nothing completed since the last take. */
int nr_async_take_done(NrAsync *a, NrAsyncDone *out);

/* Return a buffer obtained via take_done (or NULL) to the pool. */
void nr_async_give_buf(NrAsync *a, unsigned char *buf);

/* Ownership probe for teardown: 1 = buf is a pool buffer and has been
 * reclaimed into the free list (the caller must NOT free() it), 0 = it
 * never belonged to this pool (caller's own allocation - free it
 * normally). Lets a zero-copy-published result/fgi slot pass through the
 * generic free() path without double-freeing. */
int nr_async_claim(NrAsync *a, unsigned char *buf);

unsigned long nr_async_dropped(const NrAsync *a);     /* evicted requests */
unsigned long nr_async_submitted(const NrAsync *a);   /* queued requests */

#ifdef __cplusplus
}
#endif

#endif /* NR_ASYNC_H */
