/*
 * nr_layer_win - Windows port of nr_layer.c (dlss-nr-on-intel, src/layer).
 * SPDX-License-Identifier: Apache-2.0
 * Derived from dlss-nr-on-intel (Apache-2.0); upstream notice retained.
 *
 * A Vulkan implicit layer that sits at vkQueuePresentKHR: forces
 * TRANSFER_SRC|DST onto swapchain images at vkCreateSwapchainKHR, copies the
 * presented frame out, hands it to a local daemon over TCP, and copies the
 * processed frame back before the present. No desktop capture, no overlay,
 * no feedback loop: the game's own pixels are what changes.
 *
 * Port notes (Windows): pthread -> CRITICAL_SECTION, Unix socket -> TCP
 * 127.0.0.1:NR_LAYER_PORT (default 48990), access() -> _access(), stderr ->
 * %TEMP%\nr_layer_win.log. Logic mirrors the Linux original 1:1; the default
 * sync model stays "idle" (vkQueueWaitIdle), NR_LAYER_SYNC=semaphore enables
 * the present-semaphore ring.
 *
 * Env: ENABLE_NR_LAYER=1 arms the layer (manifest enable_environment).
 *   NR_LAYER_PORT     daemon TCP port (default 48990)
 *   NR_LAYER_TRIGGER  while this file exists: process + hold (photo mode)
 *   NR_LAYER_LIVE N   every Nth present processed; between frames re-blit the
 *                     last result (slideshow mode). N=1 = every present.
 *                     Arms CTRL+ALT+X (pause/resume passthrough) and
 *                     CTRL+ALT+Q (layer off) hotkeys.
 *   NR_LAYER_NOPATCH  1 = do not touch swapchains (pure passthrough; bisect)
 *   NR_LAYER_FREEZE   while this file exists (live mode only): the last raw
 *                     frame is HELD - every present re-blits the processed
 *                     result so the picture freezes while the game runs on
 *   NR_LAYER_REPROC   mtime change of this file while frozen: re-send the
 *                     held RAW frame to the daemon (it reprocesses with the
 *                     current gain) and hold the fresh result. This is the
 *                     in-game "photo mode with live knobs" (m13 overlay).
 *   NR_LAYER_CAPTURE  write the next captured frame (header+pixels) here
 *   NR_LAYER_EVERY N  capture every Nth present even without a trigger
 *   NR_LAYER_SYNC     "semaphore" = present-semaphore ring; default = idle
 *   NR_LAYER_UI_MASK  0 = disable exact-repeat reuse of the last processed
 *                     result (default on; legacy name)
 *   NR_LAYER_NEAR_REUSE 1 = also reuse on near-identical input (sampled eps,
 *                     capped replays). OPT-IN, default OFF: stale-result
 *                     replay judders on slow pans / animated grain.
 *   NR_LAYER_ASYNC    0 = synchronous TCP round-trip inside the present
 *                     (the old behaviour, kept for bisecting). Default ON
 *                     for the daemon path: the present submits the frame
 *                     to a worker mailbox (bounded, drop-oldest) and shows
 *                     the newest completed reply it already holds - the
 *                     ~60 ms m11d round-trip runs in parallel with the
 *                     game instead of capping it (GTA IV: ~15 fps ->
 *                     engine speed, no loading-screen "Not Responding").
 *                     The processed stream simply lags the raw stream by
 *                     roughly one daemon frame; on static content the
 *                     displayed result is identical to the sync path.
 *   NR_LAYER_ASYNC_STALE_MS  staleness bound for serve-best on NEW content
 *                     (default 500): while replies keep landing within
 *                     this window every present shows the freshest
 *                     processed frame; past it (daemon stalled) the
 *                     current raw frame wins - a consistent processed
 *                     stream beats a processed/raw strobe, but only while
 *                     it is alive. 0 = serve the held result at any age.
 *                     Identical input never reaches the bound (the reuse
 *                     gate replays the processed result for statics).
 *   NR_LAYER_SERVE_STALE  0 = legacy: a held result older than the async
 *                     stale bound emits the current raw frame (the -2
 *                     policy raw). Default ON (1): a processed result that
 *                     exists for this extent+format+chain is ALWAYS
 *                     preferred over raw, however old - a coherent stale
 *                     processed frame beats a single-frame raw splice
 *                     (the measured raw-pop flashes). Freshness then only
 *                     picks the diag tag: aserv when the result's source
 *                     is among the last NR_LAYER_STALE_MATCH captured
 *                     uploads (or it just landed), stale when older.
 *                     Raw remains possible only at cold start (no result
 *                     ever) or after an extent/format/chain change.
 *   NR_LAYER_STALE_MATCH  N (default 4, clamp 1..8): the captured-upload
 *                     CRC window a held result's source must hit to count
 *                     as content-fresh. Content match, not the clock, is
 *                     the freshness test - a reply that lands "late" on a
 *                     static scene still answers what the user sees.
 *   NR_LAYER_SUBMIT_IDLE  0 = submit every process-eligible present (the
 *                     legacy flood: bounded ring, drop-oldest, most frames
 *                     die in queue before the daemon sees them). Default
 *                     1: submit only while the worker's pending ring is
 *                     empty - the daemon always works on the freshest
 *                     captured frame and replies stay <= ~2 submits old.
 *                     2 = stricter: also wait for the in-flight exchange
 *                     to finish (fully serial; freshest requests, a bit
 *                     less daemon throughput).
 *   NR_LAYER_SUBMIT_EVERY N (default 1 = every eligible present): an
 *                     additional hard decimation - only every Nth
 *                     eligible present may submit. For rapid-change
 *                     content (loading screens) N=2..3 halves the daemon
 *                     load; combines with SUBMIT_IDLE (both must allow).
 *   NR_LAYER_RESIDUAL 1 = enable serve-residual (default OFF on the async
 *                     TCP path). Disabled by the owner field report: on
 *                     sustained camera motion the masked compose still
 *                     mixes enhanced/raw per pixel (coverage saws
 *                     99->20->99%), and at tuning intensity that spatial
 *                     mix itself reads as colour-play + boundary shimmer -
 *                     the same artifact class the mask was meant to kill.
 *                     A coherent stale processed frame reads better than
 *                     a 60 fps speckle. When enabled, the shipped residual
 *                     path is the GPU warped compose (NR_LAYER_GPUWARP
 *                     below): the residual lands at block-matched
 *                     displaced positions, which keeps coverage ~77-90%
 *                     through camera pans - the CPU masked path that
 *                     preceded it dithered at 40-55% (field report
 *                     2026-10-06). When a present has
 *                     no fresh
 *                     reply for its own content, the layer displays the
 *                     CURRENT raw frame + the newest reply's enhancement
 *                     delta (proc - its source, stored by nr_residual.c)
 *                     instead of re-showing the stale processed frame -
 *                     the display tracks the game's presents on animated
 *                     content rather than the daemon's reply rate.
 *   NR_LAYER_RESIDUAL_MS    age bound for the residual pair (default 400);
 *                     older than this the pair is stale, 0 = no age bound.
 *   NR_LAYER_RESIDUAL_PX    per-pixel mask eps in LSB (default 12): a
 *                     pixel takes the residual only while every colour
 *                     channel of the CURRENT raw stays within +-eps of the
 *                     pair source. Moved content can never receive a
 *                     displaced delta - this is what kills the ghosts.
 *   NR_LAYER_RESIDUAL_DIFF  scene-CUT short-circuit: sampled mean
 *                     |cur - pair src| per channel in hundredths
 *                     (default 3000 = 30.00 - only a real cut reaches it;
 *                     in-motion frames go to the per-pixel mask instead).
 *   NR_LAYER_RESIDUAL_BIG   second half of the cut probe: allowed share
 *                     of sampled pixels moved >48/255, per-mille
 *                     (default 800). A scene cut trips both bounds.
 *   NR_LAYER_RESIDUAL_FB    what a REJECTED residual serve falls back to
 *                     when mapped holds the current raw frame:
 *                     1 (default) = the current raw frame itself - the
 *                     freshest applicable enhancement for content that
 *                     moved is none, and the stated goal is CURRENT
 *                     content over stale pixels;
 *                     0 = the legacy serve-best order (stale processed
 *                     frame first) - A/B bisect knob, NOT the goal.
 *   NR_LAYER_WARP     0 = disable the displaced-transfer probe (default
 *                     ON when the residual itself is on). The mask still
 *                     runs first; pixels it rejects are probed at
 *                     displaced source positions - the first position
 *                     whose content matches cur within the same eps
 *                     supplies its residual. Fixes the sawtooth coverage
 *                     the flat mask shows on sustained motion (field
 *                     report: 55%->40% dither, rejected share flashes
 *                     raw). Bounded by probe cap + frame budget.
 *   NR_LAYER_WARP_MAXOFF    largest probe displacement in px (def 32).
 *   NR_LAYER_WARP_PROBES    per-pixel probe cap (def 48).
 *   NR_LAYER_WARP_BUDGET    per-call probe budget, per-mille of the
 *                     pixel count (def 4000 = ~4 probes/px average).
 *   NR_LAYER_GPUWARP  0 = keep the residual compose on the CPU (default
 *                     ON whenever NR_LAYER_RESIDUAL itself is enabled;
 *                     async TCP path only, framegen mode 2 excluded).
 *                     Three
 *                     embedded compute passes (nr_gpuwarp.c): quarter-res
 *                     box downsample -> 3x3-SAD block match -> per-pixel
 *                     masked compose that re-tries rejects at the
 *                     field-proposed displaced position. The composite
 *                     is written straight into the swapchain image; any
 *                     Vulkan error poisons the module for the session
 *                     and the CPU masked path takes over seamlessly.
 *   NR_LAYER_GPUWARP_RADIUS block-match radius in quarter-res px
 *                     (def 16 = +-64 full-res px).
 *   NR_LAYER_GPUWARP_DBG    1 = also record the applied offset per pixel
 *                     into a host-visible buffer (offline verifier).
 *
 * Live knobs: %TEMP%\m13_phase30_knobs.json is rewritten by the m13 manager
 * on every knob change. A running game's env is frozen at launch, so the
 * layer polls that file (mtime, ~250 ms) and pushes scale/gain/blend/
 * sharpness/frame_gen into the in-process engine - the env vars above stay
 * the launch-time baseline. The chain itself polls the same file for the 7
 * vendor phase30 params.
 *
 * Daemon flap control (TCP path, nr_degrade.h): a transport-level exchange
 * failure (connected, but the daemon did not answer) re-blits the last
 * processed frame of the same extent - a raw frame is never inserted into a
 * processed stream while the outage is fresh (NR_DEGRADE_STALE_MS).
 * connect()-refused means the daemon is absent and passes native. During a
 * flap/outage only every 8th/32nd present probes the socket; the rest are
 * served without a blocking timeout. Swapchains below 64x64 on either axis
 * are degenerate (menu/letterbox sub-elements) and always pass through
 * native, never touching the daemon.
 *
 * Build: see CMakeLists.txt (x64; x86 build needed for 32-bit games).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <vulkan/vk_layer.h>
#include "chain_c_api.h"
#include "nr_framegen.h"
#include "nr_degrade.h"
#include "nr_util.h"
#include "nr_async.h"
#include "nr_residual.h"
#include "nr_warp.h"
#include "nr_gpuwarp.h"
#include "daemon_client.h"
#include "gdi_guard.h"
#include "nr_ui.h"

#define MAX_SWAPCHAINS 8
#define MAX_IMAGES 8
#define SYNC_SLOTS 4
#define NR_MAX_WAITS 16
#define NR_CAP_RING 8   /* capture-CRC ring depth (STALE_MATCH clamp) */

static int sync_semaphores;

/* -------------------------------------------------------------------------
 * GDI repaint guard.
 *
 * A Windows repaint message (WM_PAINT / WM_ERASEBKGND - fired by input
 * events, focus changes, InvalidateRect, DWM housekeeping) can make the
 * render window flash a stale non-Vulkan surface. Observed live on GTA IV:
 * every click or posted WM_PAINT splices exactly ONE raw native frame into
 * the processed stream. In-game input means continuous repaints, which is
 * the reported "everything flickers when moving the mouse". That traffic
 * never crosses vkQueuePresentKHR, so the only interception point is the
 * window proc of the surface's HWND: drain the message (BeginPaint/
 * EndPaint validates the region) and let the composited swapchain frame
 * stay authoritative. Kill switch: NR_LAYER_GDI_GUARD=0.
 * ---------------------------------------------------------------------- */
typedef struct device_data {
    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkDeviceMemory staging_memory;
    VkBuffer staging;
    VkDeviceSize staging_size;
    void *mapped;
    unsigned char *result;
    VkDeviceSize result_size;
    int result_valid;          /* result holds a re-blittable frame */
    uint32_t result_w, result_h; /* extent that frame was processed at */
    int holding;
    VkSwapchainKHR holding_chain; /* the swapchain the held result belongs to */
    unsigned char *earlier;
    VkDeviceSize earlier_size;
    int earlier_pending;         /* earlier was overwritten by an input whose
                                  * process never completed - do not serve
                                  * `result` as its match until success */
    VkCommandBuffer ring_commands[SYNC_SLOTS];
    VkFence ring_fence[SYNC_SLOTS];
    VkSemaphore ring_done[SYNC_SLOTS];
    int ring_used[SYNC_SLOTS];
    unsigned ring_next;
    int ring_ready;
    const VkSemaphore *present_wait;
    uint32_t present_wait_count;
    VkSemaphore present_signal;
    int present_plain;
    int have_earlier;
    unsigned reuse_run;          /* consecutive near-identical replays; capped
                                  * so a moving-but-small element (cursor,
                                  * head edge) cannot ride a stale result */
    unsigned char *raw;          /* freeze mode: the held UNPROCESSED frame */
    VkDeviceSize raw_size;
    int raw_held;
    VkSwapchainKHR raw_chain;    /* SINGLE-LATCH: the swapchain the held raw
                                * frame was captured from. While frozen, ONLY
                                * this chain may touch raw/result; all other
                                * presented chains pass through native. */
    uint32_t raw_w, raw_h;       /* extent the held frame was captured at */
    FILETIME reproc_stamp;       /* last seen NR_LAYER_REPROC mtime */
    int reproc_seen;
    unsigned char *interp;
    VkDeviceSize interp_size;
    unsigned char *mid;          /* fg mode 2: the REAL middle frame captured
                                  * at the non-anchor present; sent to the
                                  * engine (in-process) or daemon (NRN3) as
                                  * the occlusion/cut fallback anchor */
    VkDeviceSize mid_size;
    int mid_valid;               /* mid holds a frame of the current extent */
    uint32_t mid_w, mid_h;       /* extent mid was captured at */
    /* Async daemon submit (nr_async.h, TCP path only): the present path
     * enqueues the captured frame and serves the newest completed reply
     * it already holds - the ~60 ms daemon round-trip moves off the
     * game's present. result_ms/result_seq track when the newest PROC
     * reply landed (staleness bound for the serve-best policy). */
    struct NrAsync *async;
    int async_failed;            /* create failed once - do not retry+spam */
    uint64_t result_ms;          /* tick the current result landed at */
    unsigned long result_seq;    /* submission seq the result answers */
    /* Content-freshness bookkeeping for the serve policy: the held
     * result's source identity + a small ring of the most recent
     * captured-upload CRCs. A reply whose source still matches a recent
     * upload answers content the user just saw - it is fresh regardless
     * of wall-clock age (the slow daemon round-trip is the normal case,
     * so age alone cannot tell "stale" from "still current"). */
    uint32_t result_src_crc;     /* crc of the raw frame result answers;
                                  * 0 when the reply carried no source */
    uint32_t last_in_crc;        /* crc of the raw input captured THIS
                                  * present (pre-serve); 0 before first
                                  * capture. Lets diag attribute a served
                                  * flash to its true source frame. */
    uint32_t result_fmt;         /* wire format of the held result */
    unsigned long result_age;    /* submits issued since the answer frame */
    uint32_t cap_crc[NR_CAP_RING]; /* last captured-upload CRCs */
    unsigned cap_fill;             /* ring fill count, <= NR_CAP_RING */
    unsigned cap_next;             /* next ring write slot */
    unsigned long submit_seq;      /* newest issued submission seq */
    unsigned long submit_tick;     /* per-device submit-eligibility clock */
    unsigned long submit_skipped;  /* pacing-gate skips (stats only) */
    /* Serve-residual compositor (nr_residual.h): keeps the newest
     * (source_raw, proc_reply) pair so a present without a fresh reply can
     * display CURRENT raw + the freshest enhancement delta instead of the
     * stale processed frame. Async TCP path only. */
    struct NrResidual *resid;
    /* Displaced-transfer probe (nr_warp.h): mask-rejected pixels retry at
     * displaced source positions. Owned separately, attached to resid. */
    struct NrWarp *warp;
    /* GPU residual compose (nr_gpuwarp.h): same mask semantics, but the
     * displaced retry is driven by a real block-match field computed on
     * this device's own queue. NULL = CPU path only. */
    struct NrGpuWarp *gpuwarp;
    /* Latest daemon-FG reply (NRN2/NRN3) awaiting its one-shot display:
     * generated midpoints are display frames, never anchors - they must
     * NOT be written into result (it feeds every held/reuse path). */
    unsigned char *fgi;
    VkDeviceSize fgi_size;
    int fgi_pending;             /* fgi holds a reply not yet displayed */
    unsigned long fgi_seq;       /* request seq that produced it */
    uint64_t fgi_ms;             /* tick it landed (staleness check) */
    uint64_t fgi_chain;
    uint32_t fgi_w, fgi_h;
    PFN_vkGetDeviceProcAddr get_device_proc;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetDeviceQueue2 get_device_queue2;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkDestroyDevice destroy_device;
} device_data;

typedef struct swapchain_data {
    VkSwapchainKHR swapchain;
    VkDevice device;
    VkImage images[MAX_IMAGES];
    uint32_t image_count;
    VkFormat format;
    VkExtent2D extent;
} swapchain_data;

typedef struct queue_data {
    VkQueue queue;
    VkDevice device;
    uint32_t family;
    int capture_ok;
    int native_logged;
} queue_data;

#define MAX_QUEUES 16

static device_data devices[8];
static swapchain_data swapchains[MAX_SWAPCHAINS];
static queue_data queues[MAX_QUEUES];

static PFN_vkGetInstanceProcAddr next_instance_proc;
static VkInstance layer_instance;
static unsigned long frame_counter;
static const char *capture_path;
static const char *capture_out_path;
static long capture_every;
static long live_every;
static NrFrameGen g_fg;
static NrDegrade g_dg;
static const char *trigger_path;
static const char *freeze_path;
static const char *reproc_path;
static char default_freeze_path[MAX_PATH];
static char default_reproc_path[MAX_PATH];
static int static_reuse;
static int no_patch;            /* NR_LAYER_NOPATCH=1: leave swapchains alone */
/* Async daemon submit (TCP path): present enqueues, the worker waits for
 * the reply. ON by default on the TCP path; NR_LAYER_ASYNC=0 restores the
 * synchronous exchange for bisecting. */
static int g_async = -1;        /* -1 uninited (CreateInstance reads env) */
/* Staleness bound for the serve-best policy: while a reply has landed
 * within this window the processed stream is "live" and even NEW content
 * is shown the freshest completed result (a consistent processed stream
 * beats a processed/raw strobe). Older than this on changed input the
 * daemon is effectively stalled -> present the current raw frame.
 * Identical input never reaches this check (the reuse path replays the
 * processed result for static content regardless of age). 0 disables
 * the bound entirely. */
static long g_async_stale_ms = 500;
/* Serve-policy knobs (NR_LAYER_SERVE_STALE / _STALE_MATCH /
 * _SUBMIT_IDLE / _SUBMIT_EVERY - see the header block): prefer a held
 * processed result over raw at any age, and pace async submits so the
 * daemon's pending ring stays empty and every request it runs is the
 * freshest captured frame. */
static int g_serve_stale = 1;
static long g_stale_match = 4;
static int g_submit_idle = 1;
static long g_submit_every = 1;
/* Serve-residual (NR_LAYER_RESIDUAL, async TCP path only): while the
 * newest reply pair is fresh AND the current frame still resembles the
 * pair's source, a present displays cur_raw + residual instead of the
 * stale processed frame. -1 = env not parsed yet. */
static int g_residual = -1;
static long g_resid_ms = 400;      /* pair age bound (NR_LAYER_RESIDUAL_MS) */
static long g_resid_px = 12;       /* per-pixel mask eps, LSB
                                      (NR_LAYER_RESIDUAL_PX) */
static long g_resid_diff = 3000;   /* scene-cut probe: sampled mean |d|
                                      bound, x100 per ch (cut-only now
                                      that the pixel mask gates) */
static long g_resid_big = 800;     /* scene-cut probe: moved-pixel share
                                      bound, per-mille */
static int g_resid_fb_raw = 1;     /* reject fallback: current raw (1) vs
                                      legacy stale-processed order (0) -
                                      NR_LAYER_RESIDUAL_FB */
/* Displaced residual transfer (nr_warp.h, NR_LAYER_WARP): pixels the mask
 * rejects get probed at displaced source positions - smallest matching
 * displacement supplies the residual. */
static int g_warp = -1;
static long g_warp_maxoff = 32;
static long g_warp_cap = 48;
static long g_warp_budget = 4000;    /* probe budget, per-mille of npix */
/* GPU residual compose (nr_gpuwarp.h, NR_LAYER_GPUWARP): default ON; the
 * same
 * mask semantics as the CPU path but the displaced retry is driven by a
 * real quarter-res block-match field on the game's own queue. */
static int g_gpuwarp = -1;
static long g_gpuwarp_radius = 16;   /* DS px (NR_LAYER_GPUWARP_RADIUS) */
static int g_gpuwarp_dbg = 0;        /* per-pixel offset record
                                        (NR_LAYER_GPUWARP_DBG) */
static long g_gpuwarp_cut = 500;     /* unmatched-block cut gate, pm
                                        (NR_LAYER_GPUWARP_CUT) */
static int g_gpuwarp_nh = 2;         /* neighbour votes at same offset
                                        (NR_LAYER_GPUWARP_NH) */
static volatile LONG g_paused;  /* CTRL+ALT+X: passthrough, full fps */
static volatile LONG g_off;     /* CTRL+ALT+Q: layer off for good */
static volatile LONG g_hotkeys_started;
/* A pushed knob value changes what the SAME input produces: the cached
 * earlier->result mapping is stale the moment a knob lands. Bumped wherever
 * update_live_knobs/poll_knob_file pushes a changed value; consumed once per
 * present in nr_QueuePresentKHR by dropping the earlier compare snapshot, so
 * a byte-identical frame reprocesses at the new strength instead of
 * replaying the old one (the "dead knob on a static screen" bug). */
static unsigned long g_knobs_changed;

static void init_freeze_paths(void)
{
    if (default_freeze_path[0] == '\0') {
        char temp_dir[MAX_PATH];
        DWORD len = GetTempPathA(sizeof(temp_dir), temp_dir);
        if (len > 0 && len < sizeof(temp_dir)) {
            if (temp_dir[len - 1] != '\\' && temp_dir[len - 1] != '/') {
                snprintf(default_freeze_path, sizeof(default_freeze_path), "%s\\m13_freeze.flag", temp_dir);
                snprintf(default_reproc_path, sizeof(default_reproc_path), "%s\\m13_reproc.flag", temp_dir);
            } else {
                snprintf(default_freeze_path, sizeof(default_freeze_path), "%sm13_freeze.flag", temp_dir);
                snprintf(default_reproc_path, sizeof(default_reproc_path), "%sm13_reproc.flag", temp_dir);
            }
        } else {
            const char *t = getenv("TEMP");
            snprintf(default_freeze_path, sizeof(default_freeze_path), "%s\\m13_freeze.flag", t ? t : ".");
            snprintf(default_reproc_path, sizeof(default_reproc_path), "%s\\m13_reproc.flag", t ? t : ".");
        }
    }
    if (!freeze_path || !freeze_path[0]) {
        const char *env_f = getenv("NR_LAYER_FREEZE");
        freeze_path = (env_f && env_f[0]) ? env_f : default_freeze_path;
    }
    if (!reproc_path || !reproc_path[0]) {
        const char *env_r = getenv("NR_LAYER_REPROC");
        reproc_path = (env_r && env_r[0]) ? env_r : default_reproc_path;
    }
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* In-process DLSS 5 engine integration (Stage 3: TCP elimination).    */
/* ------------------------------------------------------------------ */
static HMODULE g_chain_dll = NULL;
static void *g_engine = NULL;
static PFN_dlss5_create pfn_dlss5_create = NULL;
static PFN_dlss5_destroy pfn_dlss5_destroy = NULL;
static PFN_dlss5_process_frame pfn_dlss5_process_frame = NULL;
static PFN_dlss5_set_scale pfn_dlss5_set_scale = NULL;
static PFN_dlss5_set_gain pfn_dlss5_set_gain = NULL;
static PFN_dlss5_set_blend pfn_dlss5_set_blend = NULL;
static PFN_dlss5_set_sharpness pfn_dlss5_set_sharpness = NULL;
static PFN_dlss5_framegen_transfer pfn_dlss5_framegen_transfer = NULL;
static PFN_dlss5_framegen_interpolate pfn_dlss5_framegen_interpolate = NULL;
static PFN_dlss5_set_framegen pfn_dlss5_set_framegen = NULL;
static PFN_dlss5_get_framegen pfn_dlss5_get_framegen = NULL;
static PFN_dlss5_set_framegen_mode pfn_dlss5_set_framegen_mode = NULL;
static PFN_dlss5_get_framegen_mode pfn_dlss5_get_framegen_mode = NULL;
static PFN_dlss5_set_phase30 pfn_dlss5_set_phase30 = NULL;
static PFN_dlss5_set_intensity pfn_dlss5_set_intensity = NULL;
static PFN_dlss5_set_local_tone pfn_dlss5_set_local_tone = NULL;
static PFN_dlss5_set_local_structure pfn_dlss5_set_local_structure = NULL;
static PFN_dlss5_set_skin_structure pfn_dlss5_set_skin_structure = NULL;
static PFN_dlss5_set_auto_mask pfn_dlss5_set_auto_mask = NULL;
static PFN_dlss5_set_style_index pfn_dlss5_set_style_index = NULL;
static PFN_dlss5_set_color_strength pfn_dlss5_set_color_strength = NULL;
static PFN_dlss5_reset_history pfn_dlss5_reset_history = NULL;
static int g_inprocess_attempted = 0;
static int g_inprocess_active = 0;
static float g_cur_scale = -1.0f;
static float g_cur_gain = -1.0f;
static float g_cur_blend = -1.0f;
static float g_cur_sharpness = -1.0f;
static float g_cur_intensity = -1.0f;
static float g_cur_local_tone = -1.0f;
static float g_cur_local_structure = -1.0f;
static float g_cur_skin_structure = -999.0f;
static int g_cur_auto_mask = -1;
static int g_cur_style_index = -1;
static float g_cur_color_strength = -1.0f;
static int g_cur_framegen = -1;

/* Live knob file (%TEMP%\m13_phase30_knobs.json): the manager rewrites it
 * on every knob change and a running child's env is frozen at launch, so
 * the file is the live override channel for the in-process engine while
 * the env reads in update_live_knobs stay the launch baseline. Once the
 * file supplied a value it owns the knob - g_knob_seen gates the matching
 * env blocks so the static env cannot push back over a file-driven value. */
static char g_knob_path[MAX_PATH];
static FILETIME g_knob_stamp;
static int g_knob_seen;
static ULONGLONG g_knob_next_poll;

static int is_framegen_enabled(void)
{
    /* The knob file is the live override once it supplied a value. */
    if (g_cur_framegen >= 0) return g_cur_framegen;
    const char *fg = getenv("NR_LAYER_FRAMEGEN");
    if (!fg) fg = getenv("DLSS5_FRAMEGEN");
    if (fg) {
        if (fg[0] == '1' || !strcmp(fg, "true") || !strcmp(fg, "on")) return 1;
        if (fg[0] == '0' || !strcmp(fg, "false") || !strcmp(fg, "off")) return 0;
    }
    if (g_engine && pfn_dlss5_get_framegen) {
        return pfn_dlss5_get_framegen(g_engine) == 1;
    }
    return 0;
}

/* FG algorithm: 1 = residual transfer (+0 latency), 2 = anchor-to-anchor
 * interpolation (+1 frame). Env override NR_LAYER_FG_MODE/DLSS5_FG_MODE
 * wins; else the in-process engine's effective mode; default 1 keeps the
 * old extrapolation cadence for deployments without fginterp.spv. */
static int framegen_mode(void)
{
    const char *fm = getenv("NR_LAYER_FG_MODE");
    if (!fm) fm = getenv("DLSS5_FG_MODE");
    if (fm && fm[0]) return (atoi(fm) == 2) ? 2 : 1;
    if (g_engine && pfn_dlss5_get_framegen_mode) {
        int m = pfn_dlss5_get_framegen_mode(g_engine);
        return (m == 2) ? 2 : 1;
    }
    return 1;
}

static const char *knob_file_path(void)
{
    if (!g_knob_path[0]) {
        DWORD len = GetTempPathA(sizeof(g_knob_path), g_knob_path);
        if (len > 0 && len < sizeof(g_knob_path)) {
            size_t l = strlen(g_knob_path);
            if (g_knob_path[l - 1] != '\\' && g_knob_path[l - 1] != '/')
                strncat(g_knob_path, "\\", sizeof(g_knob_path) - l - 1);
            strncat(g_knob_path, "m13_phase30_knobs.json",
                    sizeof(g_knob_path) - strlen(g_knob_path) - 1);
        } else {
            const char *t = getenv("TEMP");
            _snprintf(g_knob_path, sizeof(g_knob_path),
                      "%s\\m13_phase30_knobs.json", t ? t : ".");
            g_knob_path[sizeof(g_knob_path) - 1] = 0;
        }
    }
    return g_knob_path;
}

/* Minimal JSON scalar scan: find "key", then ':', then the token up to a
 * ',', '}' or line end - same idea as extractJsonField in the chain's
 * phase30.cpp (the knob file is ~300 bytes; a real parser is overkill).
 * Returns 1 and fills `out` when the key is present. */
static int knob_json_value(const char *json, const char *key,
                           char *out, size_t out_sz)
{
    char needle[64];
    const char *p;
    size_t n = 0;
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
    needle[sizeof(needle) - 1] = 0;
    p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '"') p++;
    while (p[n] && p[n] != ',' && p[n] != '}' && p[n] != '"' &&
           p[n] != '\r' && p[n] != '\n' && n + 1 < out_sz)
        n++;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (!n) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

/* Re-read the knob file when its mtime changed; the attribute fetch is
 * throttled to once per ~250 ms (force=1 bypasses it - the freeze/reprocess
 * path wants the new value on THIS frame). Pushes changed values into the
 * in-process engine; a successful read marks the file as the authority
 * (g_knob_seen), which gates the env baseline blocks below. */
static void poll_knob_file(int force)
{
    static char buf[4096];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILE *f;
    size_t n;
    ULONGLONG now;
    char tok[64];
    /* No g_engine gate on the WATCH half: on the x86 daemon path the
     * resident engine inside m11d polls this file itself, while the
     * layer's earlier->result cache must still see the rewrite as a knob
     * change - else a static screen replays the old-strength frame. */
    now = GetTickCount64();
    if (!force && now < g_knob_next_poll) return;
    g_knob_next_poll = now + 250;
    if (!GetFileAttributesExA(knob_file_path(), GetFileExInfoStandard, &fa))
        return;
    if (g_knob_seen
        && fa.ftLastWriteTime.dwLowDateTime == g_knob_stamp.dwLowDateTime
        && fa.ftLastWriteTime.dwHighDateTime == g_knob_stamp.dwHighDateTime)
        return;
    f = fopen(knob_file_path(), "rb");
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    /* A torn write (manager rewrites non-atomically) lacks the closing brace;
     * bail without consuming the stamp so the next poll retries it rather
     * than applying a partial parameter set to one frame. */
    if (!strchr(buf, '}')) return;
    g_knob_stamp = fa.ftLastWriteTime;
    if (!g_knob_seen)
        nr_log("[nr_layer] live knob file: %s", knob_file_path());
    g_knob_seen = 1;
    /* A rewrite of the knob file IS a knob change - the manager rewrites it
     * only when a knob moved, and the file also carries the 7 vendor
     * phase30 params the chain polls itself (the layer never sees those
     * pushes). Invalidate the earlier->result mapping either way. */
    g_knobs_changed++;
    /* frame_gen is the exception to the engine gate below: on the x86
     * daemon path g_engine stays NULL for the whole session but
     * g_cur_framegen is still the layer-side FG authority
     * (is_framegen_enabled drives the cadence and the NRN2/NRN3 choice),
     * so a mid-session knob write must reach it engine-less or a global
     * toggle can never flip this path's FG flag. */
    if (knob_json_value(buf, "frame_gen", tok, sizeof(tok))) {
        int val = (tok[0] == '1' || !strcmp(tok, "true")) ? 1 : 0;
        if (val != g_cur_framegen) {
            g_cur_framegen = val;
            if (g_engine && pfn_dlss5_set_framegen) {
                pfn_dlss5_set_framegen(g_engine, val);
                nr_log("[nr_layer] live framegen updated: %d (knob file)",
                       val);
            }
        }
    }
    /* The value pushes below need the in-process engine; on the daemon
     * path the watch above alone carries the knob-change signal. */
    if (!g_engine) return;

    if (knob_json_value(buf, "scale", tok, sizeof(tok))) {
        float val = (float)atof(tok);
        float diff = val - g_cur_scale;
        if (diff < 0.0f) diff = -diff;
        if (val > 0.0f && diff > 1e-4f) {
            g_cur_scale = val;
            if (pfn_dlss5_set_scale) {
                pfn_dlss5_set_scale(g_engine, val);
                nr_log("[nr_layer] live scale updated: %.2f (knob file)", val);
            }
        }
    }
    if (knob_json_value(buf, "gain", tok, sizeof(tok))) {
        float val = (float)atof(tok);
        float diff = val - g_cur_gain;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && diff > 1e-4f) {
            g_cur_gain = val;
            if (pfn_dlss5_set_gain) {
                pfn_dlss5_set_gain(g_engine, val);
                nr_log("[nr_layer] live gain updated: %.2f (knob file)", val);
            }
        }
    }
    if (knob_json_value(buf, "blend", tok, sizeof(tok))) {
        float val = (float)atof(tok);
        float diff = val - g_cur_blend;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && val <= 1.0f && diff > 1e-4f) {
            g_cur_blend = val;
            if (pfn_dlss5_set_blend) {
                pfn_dlss5_set_blend(g_engine, val);
                nr_log("[nr_layer] live blend updated: %.2f (knob file)", val);
            }
        }
    }
    if (knob_json_value(buf, "sharpness", tok, sizeof(tok))) {
        float val = (float)atof(tok);
        float diff = val - g_cur_sharpness;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && diff > 1e-4f) {
            g_cur_sharpness = val;
            if (pfn_dlss5_set_sharpness) {
                pfn_dlss5_set_sharpness(g_engine, val);
                nr_log("[nr_layer] live sharpness updated: %.2f (knob file)",
                       val);
            }
        }
    }
}

static void update_live_knobs(int force_file)
{
    /* Live channel first: once the knob file has been read it owns the
     * engine knobs; the env reads below stay the launch baseline. The
     * poll itself runs engine-less - on the daemon path the file rewrite
     * is still the knob-change signal that invalidates the reuse cache. */
    poll_knob_file(force_file);

    if (!g_engine) return;

    if (!g_knob_seen) {
        const char *scale_s = getenv("NR_LAYER_SCALE");
        if (!scale_s) scale_s = getenv("DLSS5_SCALE");
        if (scale_s) {
            float val = (float)atof(scale_s);
            float diff = val - g_cur_scale;
            if (diff < 0.0f) diff = -diff;
            if (val > 0.0f && diff > 1e-4f) {
                g_cur_scale = val;
                g_knobs_changed++;
                if (pfn_dlss5_set_scale) {
                    pfn_dlss5_set_scale(g_engine, val);
                    nr_log("[nr_layer] live scale updated: %.2f", val);
                }
            }
        }

        const char *gain_s = getenv("NR_LAYER_GAIN");
        if (!gain_s) gain_s = getenv("DLSS5_GAIN");
        if (gain_s) {
            float val = (float)atof(gain_s);
            float diff = val - g_cur_gain;
            if (diff < 0.0f) diff = -diff;
            if (val >= 0.0f && diff > 1e-4f) {
                g_cur_gain = val;
                g_knobs_changed++;
                if (pfn_dlss5_set_gain) {
                    pfn_dlss5_set_gain(g_engine, val);
                    nr_log("[nr_layer] live gain updated: %.2f", val);
                }
            }
        }

        const char *blend_s = getenv("NR_LAYER_BLEND");
        if (!blend_s) blend_s = getenv("DLSS5_BLEND");
        if (blend_s) {
            float val = (float)atof(blend_s);
            float diff = val - g_cur_blend;
            if (diff < 0.0f) diff = -diff;
            if (val >= 0.0f && val <= 1.0f && diff > 1e-4f) {
                g_cur_blend = val;
                g_knobs_changed++;
                if (pfn_dlss5_set_blend) {
                    pfn_dlss5_set_blend(g_engine, val);
                    nr_log("[nr_layer] live blend updated: %.2f", val);
                }
            }
        }

        const char *sharp_s = getenv("NR_LAYER_SHARPNESS");
        if (!sharp_s) sharp_s = getenv("DLSS5_SHARPNESS");
        if (sharp_s) {
            float val = (float)atof(sharp_s);
            float diff = val - g_cur_sharpness;
            if (diff < 0.0f) diff = -diff;
            if (val >= 0.0f && diff > 1e-4f) {
                g_cur_sharpness = val;
                g_knobs_changed++;
                if (pfn_dlss5_set_sharpness) {
                    pfn_dlss5_set_sharpness(g_engine, val);
                    nr_log("[nr_layer] live sharpness updated: %.2f", val);
                }
            }
        }

        const char *fg_s = getenv("NR_LAYER_FRAMEGEN");
        if (!fg_s) fg_s = getenv("DLSS5_FRAMEGEN");
        if (fg_s && pfn_dlss5_set_framegen) {
            int enable = (fg_s[0] == '1' || !strcmp(fg_s, "true") || !strcmp(fg_s, "on")) ? 1 : 0;
            pfn_dlss5_set_framegen(g_engine, enable);
        }
    }

    const char *intensity_s = getenv("NR_LAYER_INTENSITY");
    if (!intensity_s) intensity_s = getenv("DLSS5_INTENSITY");
    if (intensity_s && pfn_dlss5_set_intensity) {
        float val = (float)atof(intensity_s);
        float diff = val - g_cur_intensity;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_intensity = val;
            g_knobs_changed++;
            pfn_dlss5_set_intensity(g_engine, val);
            nr_log("[nr_layer] live intensity updated: %.2f", val);
        }
    }

    const char *lt_s = getenv("NR_LAYER_LOCAL_TONE");
    if (!lt_s) lt_s = getenv("DLSS5_LOCAL_TONE");
    if (lt_s && pfn_dlss5_set_local_tone) {
        float val = (float)atof(lt_s);
        float diff = val - g_cur_local_tone;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_local_tone = val;
            g_knobs_changed++;
            pfn_dlss5_set_local_tone(g_engine, val);
            nr_log("[nr_layer] live local_tone updated: %.2f", val);
        }
    }

    const char *ls_s = getenv("NR_LAYER_LOCAL_STRUCTURE");
    if (!ls_s) ls_s = getenv("DLSS5_LOCAL_STRUCTURE");
    if (ls_s && pfn_dlss5_set_local_structure) {
        float val = (float)atof(ls_s);
        float diff = val - g_cur_local_structure;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_local_structure = val;
            g_knobs_changed++;
            pfn_dlss5_set_local_structure(g_engine, val);
            nr_log("[nr_layer] live local_structure updated: %.2f", val);
        }
    }

    const char *ss_s = getenv("NR_LAYER_SKIN_STRUCTURE");
    if (!ss_s) ss_s = getenv("DLSS5_SKIN_STRUCTURE");
    if (ss_s && pfn_dlss5_set_skin_structure) {
        float val = (float)atof(ss_s);
        float diff = val - g_cur_skin_structure;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_skin_structure = val;
            g_knobs_changed++;
            pfn_dlss5_set_skin_structure(g_engine, val);
            nr_log("[nr_layer] live skin_structure updated: %.2f", val);
        }
    }

    const char *am_s = getenv("NR_LAYER_AUTO_MASK");
    if (!am_s) am_s = getenv("DLSS5_AUTO_MASK");
    if (am_s && pfn_dlss5_set_auto_mask) {
        int val = (am_s[0] == '1' || !strcmp(am_s, "true") || !strcmp(am_s, "on")) ? 1 : 0;
        if (val != g_cur_auto_mask) {
            g_cur_auto_mask = val;
            g_knobs_changed++;
            pfn_dlss5_set_auto_mask(g_engine, val);
            nr_log("[nr_layer] live auto_mask updated: %d", val);
        }
    }

    const char *si_s = getenv("NR_LAYER_STYLE_INDEX");
    if (!si_s) si_s = getenv("DLSS5_STYLE_INDEX");
    if (si_s && pfn_dlss5_set_style_index) {
        int val = atoi(si_s);
        if (val != g_cur_style_index) {
            g_cur_style_index = val;
            g_knobs_changed++;
            pfn_dlss5_set_style_index(g_engine, val);
            nr_log("[nr_layer] live style_index updated: %d", val);
        }
    }

    const char *cs_s = getenv("NR_LAYER_COLOR_STRENGTH");
    if (!cs_s) cs_s = getenv("DLSS5_COLOR_STRENGTH");
    if (cs_s && pfn_dlss5_set_color_strength) {
        float val = (float)atof(cs_s);
        float diff = val - g_cur_color_strength;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_color_strength = val;
            g_knobs_changed++;
            pfn_dlss5_set_color_strength(g_engine, val);
            nr_log("[nr_layer] live color_strength updated: %.2f", val);
        }
    }
}

static void ensure_inprocess(void)
{
    if (g_inprocess_attempted) return;
    g_inprocess_attempted = 1;

    const char *inproc_env = getenv("NR_LAYER_INPROCESS");
    if (inproc_env && !strcmp(inproc_env, "0")) {
        nr_log("in-process mode disabled via NR_LAYER_INPROCESS=0");
        return;
    }

    HMODULE hDll = NULL;
    const char *custom_dll = getenv("NR_LAYER_CHAIN_DLL");
    if (custom_dll && custom_dll[0]) {
        hDll = LoadLibraryA(custom_dll);
    }
    if (!hDll) {
        /* Check directory of this layer DLL */
        char layer_path[MAX_PATH] = {0};
        HMODULE hThis = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ensure_inprocess, &hThis) && hThis) {
            if (GetModuleFileNameA(hThis, layer_path, sizeof(layer_path))) {
                char *slash = strrchr(layer_path, '\\');
                if (!slash) slash = strrchr(layer_path, '/');
                if (slash) {
                    *(slash + 1) = '\0';
                    char candidate[MAX_PATH];
                    _snprintf(candidate, sizeof(candidate), "%sd5c_chain.dll", layer_path);
                    hDll = LoadLibraryA(candidate);
                }
            }
        }
    }
    if (!hDll) {
        /* Check build output directory relative to repo if running in tree */
        const char *candidates[] = {
            "dlss5\\chain\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11d\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11-layer\\build\\Release\\d5c_chain.dll",
            "..\\..\\chain\\build\\Release\\d5c_chain.dll",
            "..\\..\\m11d\\build\\Release\\d5c_chain.dll"
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (_access(candidates[i], 0) == 0) {
                hDll = LoadLibraryA(candidates[i]);
                if (hDll) break;
            }
        }
    }
    if (!hDll) {
        hDll = LoadLibraryA("d5c_chain.dll");
    }

    if (!hDll) {
        nr_log("d5c_chain.dll not found; falling back to TCP socket");
        return;
    }

    pfn_dlss5_create = (PFN_dlss5_create)GetProcAddress(hDll, "dlss5_create");
    pfn_dlss5_destroy = (PFN_dlss5_destroy)GetProcAddress(hDll, "dlss5_destroy");
    pfn_dlss5_process_frame = (PFN_dlss5_process_frame)GetProcAddress(hDll, "dlss5_process_frame");
    pfn_dlss5_set_scale = (PFN_dlss5_set_scale)GetProcAddress(hDll, "dlss5_set_scale");
    pfn_dlss5_set_gain = (PFN_dlss5_set_gain)GetProcAddress(hDll, "dlss5_set_gain");
    pfn_dlss5_set_blend = (PFN_dlss5_set_blend)GetProcAddress(hDll, "dlss5_set_blend");
    pfn_dlss5_set_sharpness = (PFN_dlss5_set_sharpness)GetProcAddress(hDll, "dlss5_set_sharpness");
    pfn_dlss5_framegen_transfer = (PFN_dlss5_framegen_transfer)GetProcAddress(hDll, "dlss5_framegen_transfer");
    pfn_dlss5_framegen_interpolate = (PFN_dlss5_framegen_interpolate)GetProcAddress(hDll, "dlss5_framegen_interpolate");
    pfn_dlss5_set_framegen = (PFN_dlss5_set_framegen)GetProcAddress(hDll, "dlss5_set_framegen");
    pfn_dlss5_get_framegen = (PFN_dlss5_get_framegen)GetProcAddress(hDll, "dlss5_get_framegen");
    pfn_dlss5_set_framegen_mode = (PFN_dlss5_set_framegen_mode)GetProcAddress(hDll, "dlss5_set_framegen_mode");
    pfn_dlss5_get_framegen_mode = (PFN_dlss5_get_framegen_mode)GetProcAddress(hDll, "dlss5_get_framegen_mode");
    pfn_dlss5_set_phase30 = (PFN_dlss5_set_phase30)GetProcAddress(hDll, "dlss5_set_phase30");
    pfn_dlss5_set_intensity = (PFN_dlss5_set_intensity)GetProcAddress(hDll, "dlss5_set_intensity");
    pfn_dlss5_set_local_tone = (PFN_dlss5_set_local_tone)GetProcAddress(hDll, "dlss5_set_local_tone");
    pfn_dlss5_set_local_structure = (PFN_dlss5_set_local_structure)GetProcAddress(hDll, "dlss5_set_local_structure");
    pfn_dlss5_set_skin_structure = (PFN_dlss5_set_skin_structure)GetProcAddress(hDll, "dlss5_set_skin_structure");
    pfn_dlss5_set_auto_mask = (PFN_dlss5_set_auto_mask)GetProcAddress(hDll, "dlss5_set_auto_mask");
    pfn_dlss5_set_style_index = (PFN_dlss5_set_style_index)GetProcAddress(hDll, "dlss5_set_style_index");
    pfn_dlss5_set_color_strength = (PFN_dlss5_set_color_strength)GetProcAddress(hDll, "dlss5_set_color_strength");
    pfn_dlss5_reset_history = (PFN_dlss5_reset_history)GetProcAddress(hDll, "dlss5_reset_history");

    if (!pfn_dlss5_create || !pfn_dlss5_destroy || !pfn_dlss5_process_frame) {
        nr_log("d5c_chain.dll missing exports; falling back to TCP");
        FreeLibrary(hDll);
        return;
    }

    g_chain_dll = hDll;

    const char *weights = getenv("NR_LAYER_WEIGHTS");
    if (!weights) weights = getenv("DLSS5_WEIGHTS_PATH");
    const char *shaders = getenv("NR_LAYER_SHADERS");
    if (!shaders) shaders = getenv("DLSS5_SHADER_DIR");
    const char *scale_s = getenv("NR_LAYER_SCALE");
    if (!scale_s) scale_s = getenv("DLSS5_SCALE");
    const char *gain_s = getenv("NR_LAYER_GAIN");
    if (!gain_s) gain_s = getenv("DLSS5_GAIN");
    const char *blend_s = getenv("NR_LAYER_BLEND");
    if (!blend_s) blend_s = getenv("DLSS5_BLEND");
    const char *sharp_s = getenv("NR_LAYER_SHARPNESS");
    if (!sharp_s) sharp_s = getenv("DLSS5_SHARPNESS");

    float scale = scale_s ? (float)atof(scale_s) : 1.0f;
    float gain = gain_s ? (float)atof(gain_s) : 1.0f;
    float blend = blend_s ? (float)atof(blend_s) : 1.0f;
    float sharpness = sharp_s ? (float)atof(sharp_s) : 0.8f;

    g_engine = pfn_dlss5_create(weights, shaders, scale, gain, blend);
    if (!g_engine) {
        nr_log("dlss5_create failed; falling back to TCP");
        FreeLibrary(g_chain_dll);
        g_chain_dll = NULL;
        return;
    }

    if (pfn_dlss5_set_sharpness) {
        pfn_dlss5_set_sharpness(g_engine, sharpness);
    }

    const char *fg_env = getenv("NR_LAYER_FRAMEGEN");
    if (!fg_env) fg_env = getenv("DLSS5_FRAMEGEN");
    if (fg_env && pfn_dlss5_set_framegen) {
        int enable = (fg_env[0] == '1' || !strcmp(fg_env, "true") || !strcmp(fg_env, "on")) ? 1 : 0;
        pfn_dlss5_set_framegen(g_engine, enable);
    }
    /* Push the resolved FG algorithm so the engine's per-frame anchor
     * rotation runs in the mode the present cadence expects (mode 2 keeps a
     * two-anchor pair; the engine clamps to 1 when fginterp.spv is absent). */
    if (pfn_dlss5_set_framegen_mode) {
        pfn_dlss5_set_framegen_mode(g_engine, framegen_mode());
    }

    g_cur_scale = scale;
    g_cur_gain = gain;
    g_cur_blend = blend;
    g_cur_sharpness = sharpness;
    g_inprocess_active = 1;
    nr_log("in-process DLSS 5 active (TCP bypassed, scale=%.2f gain=%.2f blend=%.2f sharpness=%.2f)",
           scale, gain, blend, sharpness);
}

static device_data *find_device(VkDevice device)
{
    for (int i = 0; i < 8; i++)
        if (devices[i].device == device) return &devices[i];
    return NULL;
}

static queue_data *find_queue(VkQueue queue)
{
    for (int i = 0; i < MAX_QUEUES; i++)
        if (queues[i].queue == queue) return &queues[i];
    return NULL;
}

/* Whether a copy can be recorded for this family at all (present-only and
 * protected queues cannot). Same guard as the Linux original. */
static int family_can_capture(device_data *data, uint32_t family)
{
    if (!data || !next_instance_proc || !layer_instance) return 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        next_instance_proc(layer_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!properties) return 0;
    uint32_t count = 0;
    properties(data->physical, &count, NULL);
    if (!count || family >= count) return 0;
    VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)calloc(count, sizeof *families);
    if (!families) return 0;
    properties(data->physical, &count, families);
    int ok = (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
                                             | VK_QUEUE_TRANSFER_BIT)) != 0;
    free(families);
    return ok;
}


/* CTRL+ALT+X pauses processing (native frames, full fps), CTRL+ALT+Q turns
 * the layer off for good. Polled via GetAsyncKeyState (like
 * m12-dxgi/m12_hotkeys.cpp): RegisterHotKey is global and EXCLUSIVE -
 * m8blive.exe registers the same combos and whoever wins the race owns
 * them; the loser got ERROR_HOTKEY_ALREADY_REGISTERED and a dead pause
 * key. Polling cannot collide and works in a game that never pumps
 * messages for us. */
static DWORD WINAPI hotkey_thread(LPVOID param)
{
    (void)param;
    int x_down = 0, q_down = 0;
    nr_log("[nr_layer] hotkeys armed: CTRL+ALT+X pause/resume, CTRL+ALT+Q layer off");
    for (;;) {
        int mod = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                  (GetAsyncKeyState(VK_MENU) & 0x8000);
        int x = mod && (GetAsyncKeyState('X') & 0x8000);
        int q = mod && (GetAsyncKeyState('Q') & 0x8000);
        if (x && !x_down) {
            InterlockedExchange(&g_paused, g_paused ? 0 : 1);
            nr_log("[nr_layer] %s by hotkey", g_paused ? "paused" : "resumed");
        }
        if (q && !q_down) {
            InterlockedExchange(&g_off, 1);
            nr_log("[nr_layer] layer off by hotkey");
        }
        x_down = x;
        q_down = q;
        Sleep(50);
    }
    return 0;
}

static void hotkeys_ensure(void)
{
    if (InterlockedCompareExchange(&g_hotkeys_started, 1, 0)) return;
    HANDLE t = CreateThread(NULL, 0, hotkey_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else {
        g_hotkeys_started = 0;
        nr_log("[nr_layer] hotkey thread failed (%lu)", GetLastError());
    }
}

static void remember_queue(VkDevice device, uint32_t family, VkQueue queue, int capture_ok)
{
    if (!queue) return;
    lock();
    if (!find_queue(queue))
        for (int i = 0; i < MAX_QUEUES; i++)
            if (!queues[i].queue) {
                queues[i].queue = queue;
                queues[i].device = device;
                queues[i].family = family;
                queues[i].capture_ok = capture_ok;
                break;
            }
    unlock();
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue(VkDevice device, uint32_t family,
                                             uint32_t index, VkQueue *queue)
{
    device_data *data = find_device(device);
    if (!data || !data->get_device_queue) return;
    data->get_device_queue(device, family, index, queue);
    remember_queue(device, family, *queue, family_can_capture(data, family));
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue2(VkDevice device,
                                              const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    device_data *data = find_device(device);
    if (!data || !data->get_device_queue2) return;
    data->get_device_queue2(device, info, queue);
    remember_queue(device, info->queueFamilyIndex, *queue,
                   (info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)
                   ? 0 : family_can_capture(data, info->queueFamilyIndex));
}

/* Four bytes a pixel only, exactly like the original. */
static int format_is_four_bytes(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return 1;
    default:
        return 0;
    }
}

/* Channel byte order of the staging pixels for the UI compositor. The
 * layer never swizzles - data->mapped is a bit-exact copy of the swapchain
 * image - so the memory order IS the format's component order. Only the
 * 8-bit RGBA-class formats are blendable; the packed A2* formats are
 * skipped entirely. Returns 0 for B,G,R,A order, 1 for R,G,B,A, -1 when
 * not blendable. */
static int frame_byte_order(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return 0;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return 1;
    default:
        return -1;
    }
}

static swapchain_data *find_swapchain(VkSwapchainKHR swapchain)
{
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (swapchains[i].swapchain == swapchain) return &swapchains[i];
    return NULL;
}

static VkLayerInstanceCreateInfo *instance_chain(const VkInstanceCreateInfo *info)
{
    VkLayerInstanceCreateInfo *item = (VkLayerInstanceCreateInfo *)info->pNext;
    while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                     && item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerInstanceCreateInfo *)item->pNext;
    return item;
}

static VkLayerDeviceCreateInfo *device_chain(const VkDeviceCreateInfo *info)
{
    VkLayerDeviceCreateInfo *item = (VkLayerDeviceCreateInfo *)info->pNext;
    while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                     && item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerDeviceCreateInfo *)item->pNext;
    return item;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateInstance(const VkInstanceCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkInstance *instance)
{
    VkLayerInstanceCreateInfo *link = instance_chain(info);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    next_instance_proc = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)next_instance_proc(NULL, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    if (info && info->pApplicationInfo && info->pApplicationInfo->pApplicationName) {
        const char *app = info->pApplicationInfo->pApplicationName;
        if (!strcmp(app, "dlss5_inprocess") || !strcmp(app, "m11d") || !strcmp(app, "d5c")) {
            nr_log("[nr_layer] bypass interception for internal process: %s", app);
            return create(info, allocator, instance);
        }
    }

    VkResult r = create(info, allocator, instance);
    if (r == VK_SUCCESS) {
        layer_instance = *instance;
        capture_path = getenv("NR_LAYER_CAPTURE");
        capture_out_path = getenv("NR_LAYER_CAPTURE_OUT");
        trigger_path = getenv("NR_LAYER_TRIGGER");
        freeze_path = getenv("NR_LAYER_FREEZE");
        reproc_path = getenv("NR_LAYER_REPROC");
        init_freeze_paths();
        const char *mask = getenv("NR_LAYER_UI_MASK");
        static_reuse = (!mask || strcmp(mask, "0") != 0);
        const char *every = getenv("NR_LAYER_EVERY");
        capture_every = every ? strtol(every, NULL, 10) : 0;
        const char *sync = getenv("NR_LAYER_SYNC");
        sync_semaphores = sync && !strcmp(sync, "semaphore");
        const char *live = getenv("NR_LAYER_LIVE");
        live_every = live ? strtol(live, NULL, 10) : 0;
        if (live_every < 0) live_every = 0;
        nr_fg_init(&g_fg, live_every);
        nr_degrade_init(&g_dg);
        const char *port = getenv("NR_LAYER_PORT");
        nr_port = port ? strtol(port, NULL, 10) : NR_DEFAULT_PORT;
        if (nr_port <= 0) nr_port = NR_DEFAULT_PORT;
        const char *nopatch = getenv("NR_LAYER_NOPATCH");
        no_patch = nopatch && strcmp(nopatch, "0") != 0;
        if (no_patch)
            nr_log("[nr_layer] NOPATCH: swapchains left completely alone");
        {
            const char *ae = getenv("NR_LAYER_ASYNC");
            g_async = !ae || strcmp(ae, "0") != 0;
            const char *st = getenv("NR_LAYER_ASYNC_STALE_MS");
            if (st) {
                g_async_stale_ms = atol(st);
                if (g_async_stale_ms < 0) g_async_stale_ms = 0;
            }
            const char *re = getenv("NR_LAYER_RESIDUAL");
            g_residual = re && strcmp(re, "0") != 0;
            const char *rm = getenv("NR_LAYER_RESIDUAL_MS");
            if (rm) {
                g_resid_ms = atol(rm);
                if (g_resid_ms < 0) g_resid_ms = 0;
            }
            const char *rd = getenv("NR_LAYER_RESIDUAL_DIFF");
            if (rd) {
                g_resid_diff = atol(rd);
                if (g_resid_diff < 0) g_resid_diff = 0;
            }
            const char *rb = getenv("NR_LAYER_RESIDUAL_BIG");
            if (rb) {
                g_resid_big = atol(rb);
                if (g_resid_big < 0) g_resid_big = 0;
            }
            const char *px = getenv("NR_LAYER_RESIDUAL_PX");
            if (px) {
                g_resid_px = atol(px);
                if (g_resid_px < 0) g_resid_px = 0;
                if (g_resid_px > 255) g_resid_px = 255;
            }
            const char *fb = getenv("NR_LAYER_RESIDUAL_FB");
            g_resid_fb_raw = !fb || strcmp(fb, "0") != 0;
            const char *we = getenv("NR_LAYER_WARP");
            g_warp = !we || strcmp(we, "0") != 0;
            const char *wm = getenv("NR_LAYER_WARP_MAXOFF");
            if (wm) { g_warp_maxoff = atol(wm); if (g_warp_maxoff < 0) g_warp_maxoff = 0; }
            const char *wc = getenv("NR_LAYER_WARP_PROBES");
            if (wc) { g_warp_cap = atol(wc); if (g_warp_cap < 0) g_warp_cap = 0; }
            const char *wb = getenv("NR_LAYER_WARP_BUDGET");
            if (wb) { g_warp_budget = atol(wb); if (g_warp_budget < 0) g_warp_budget = 0; }
            const char *gw = getenv("NR_LAYER_GPUWARP");
            g_gpuwarp = !gw || strcmp(gw, "0") != 0;
            const char *gr = getenv("NR_LAYER_GPUWARP_RADIUS");
            if (gr) { g_gpuwarp_radius = atol(gr);
                      if (g_gpuwarp_radius < 4) g_gpuwarp_radius = 4;
                      if (g_gpuwarp_radius > 64) g_gpuwarp_radius = 64; }
            const char *gd = getenv("NR_LAYER_GPUWARP_DBG");
            g_gpuwarp_dbg = gd && strcmp(gd, "0") != 0;
            const char *gc = getenv("NR_LAYER_GPUWARP_CUT");
            if (gc) { g_gpuwarp_cut = atol(gc);
                      if (g_gpuwarp_cut < 0) g_gpuwarp_cut = 0;
                      if (g_gpuwarp_cut > 1000) g_gpuwarp_cut = 1000; }
            const char *gh = getenv("NR_LAYER_GPUWARP_NH");
            if (gh) { g_gpuwarp_nh = atoi(gh);
                      if (g_gpuwarp_nh < 0) g_gpuwarp_nh = 0;
                      if (g_gpuwarp_nh > 4) g_gpuwarp_nh = 4; }
            const char *sv = getenv("NR_LAYER_SERVE_STALE");
            g_serve_stale = !sv || strcmp(sv, "0") != 0;
            const char *sm = getenv("NR_LAYER_STALE_MATCH");
            if (sm) {
                g_stale_match = atol(sm);
                if (g_stale_match < 1) g_stale_match = 1;
                if (g_stale_match > NR_CAP_RING)
                    g_stale_match = NR_CAP_RING;
            }
            const char *si = getenv("NR_LAYER_SUBMIT_IDLE");
            if (si) g_submit_idle = atoi(si);
            const char *se = getenv("NR_LAYER_SUBMIT_EVERY");
            if (se) {
                g_submit_every = atol(se);
                if (g_submit_every < 1) g_submit_every = 1;
            }
        }
        hotkeys_ensure();
        if (live_every > 0)
            nr_log("[nr_layer] live: every %ld present processed", live_every);
        if (sync_semaphores)
            nr_log("[nr_layer] sync: the present's own semaphores, not a queue idle");
        nr_log("[nr_layer] active; port=%ld trigger=%s capture=%s every=%ld",
             nr_port, trigger_path ? trigger_path : "(none)",
             capture_path ? capture_path : "(off)", capture_every);
        if (nr_port)
            nr_log("[nr_layer] async daemon submit: %s (stale bound %ld ms)",
                   g_async ? "on" : "OFF (sync exchange)", g_async_stale_ms);
        if (nr_port && g_async)
            nr_log("[nr_layer] serve policy: serve-stale %s (match window "
                   "%ld), submit pacing idle=%d every=%ld",
                   g_serve_stale ? "on" : "OFF (legacy raw)",
                   g_stale_match, g_submit_idle, g_submit_every);
        if (nr_port && g_async)
            nr_log("[nr_layer] serve-residual: %s (age bound %ld ms, "
                   "pixel eps %ld, cut bound %ld.%02ld, moved share %ld pm, "
                   "reject fb %s, warp %s maxoff %ld cap %ld budget %ld pm)",
                   g_residual ? "on" : "OFF", g_resid_ms, g_resid_px,
                   g_resid_diff / 100, g_resid_diff % 100, g_resid_big,
                   g_resid_fb_raw ? "raw" : "stale",
                   g_warp ? "on" : "OFF", g_warp_maxoff, g_warp_cap,
                   g_warp_budget);
            if (g_residual)
                nr_log("[nr_layer] GPU residual compose: %s "
                       "(radius %ld ds px, dbg %d)",
                       g_gpuwarp ? "on" : "OFF", g_gpuwarp_radius,
                       g_gpuwarp_dbg);
        if (static_reuse)
            nr_log("[nr_layer] static reuse on: exact-repeat frames reuse "
                 "the last processed result");
        ensure_inprocess();
        {
            int fg_on = is_framegen_enabled();
            int fg_avail = (g_engine && pfn_dlss5_framegen_transfer)
                           || (nr_port != 0);
            nr_log("[nr_layer] framegen: %s, %s, cadence every %ld",
                   fg_on ? "enabled" : "disabled",
                   (g_engine && pfn_dlss5_framegen_transfer)
                       ? "in-process x64 available"
                       : (nr_port ? "daemon path (NRN2 transfer over TCP)"
                                  : "unavailable (no in-process engine, no daemon port)"),
                   nr_fg_effective_every(&g_fg, fg_on, fg_avail));
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateWin32SurfaceKHR(VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)
        next_instance_proc(instance, "vkCreateWin32SurfaceKHR");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(instance, info, allocator, surface);
    if (r == VK_SUCCESS && info && info->hwnd)
        nr_guard_window(info->hwnd);
    return r;
}

VKAPI_ATTR void VKAPI_CALL nr_DestroyInstance(VkInstance instance,
                                             const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyInstance destroy = NULL;
    if (next_instance_proc)
        destroy = (PFN_vkDestroyInstance)next_instance_proc(instance, "vkDestroyInstance");
    if (instance == layer_instance) {
        layer_instance = VK_NULL_HANDLE;
        /* The loader may drop this DLL after the instance dies; queued
         * window messages must never reach an unmapped nr_wnd_proc. */
        nr_unguard_all();
    }
    if (destroy) destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateDevice(VkPhysicalDevice physical,
                                               const VkDeviceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkDevice *device)
{
    VkLayerDeviceCreateInfo *link = device_chain(info);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_device = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create = NULL;
    if (next_instance && layer_instance)
        create = (PFN_vkCreateDevice)next_instance(layer_instance, "vkCreateDevice");
    if (!create && next_instance)
        create = (PFN_vkCreateDevice)next_instance(NULL, "vkCreateDevice");
    if (!create && next_instance_proc && layer_instance)
        create = (PFN_vkCreateDevice)next_instance_proc(layer_instance, "vkCreateDevice");
    if (!create && next_instance_proc)
        create = (PFN_vkCreateDevice)next_instance_proc(NULL, "vkCreateDevice");
    if (!create) {
        nr_log("[nr_layer] FATAL: vkCreateDevice proc address not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult r = create(physical, info, allocator, device);
    if (r != VK_SUCCESS || !device || !*device) return r;

    lock();
    device_data *data = NULL;
    for (int i = 0; i < 8; i++) if (!devices[i].device) { data = &devices[i]; break; }
    if (data) {
        memset(data, 0, sizeof *data);
        data->device = *device;
        data->physical = physical;
        data->get_device_proc = next_device;
        data->present = (PFN_vkQueuePresentKHR)next_device(*device, "vkQueuePresentKHR");
        data->get_device_queue =
            (PFN_vkGetDeviceQueue)next_device(*device, "vkGetDeviceQueue");
        data->get_device_queue2 =
            (PFN_vkGetDeviceQueue2)next_device(*device, "vkGetDeviceQueue2");
        data->destroy_swapchain =
            (PFN_vkDestroySwapchainKHR)next_device(*device, "vkDestroySwapchainKHR");
        data->create_swapchain =
            (PFN_vkCreateSwapchainKHR)next_device(*device, "vkCreateSwapchainKHR");
        data->get_swapchain_images =
            (PFN_vkGetSwapchainImagesKHR)next_device(*device, "vkGetSwapchainImagesKHR");
        data->destroy_device = (PFN_vkDestroyDevice)next_device(*device, "vkDestroyDevice");
        data->queue_family = info->queueCreateInfoCount
            ? info->pQueueCreateInfos[0].queueFamilyIndex : 0;
    }
    unlock();
    return r;
}

/* Force TRANSFER_SRC|DST onto the swapchain images; without it the copy is
 * invalid and vkCreateSwapchainKHR is the only place it can be added. */
VKAPI_ATTR VkResult VKAPI_CALL nr_CreateSwapchainKHR(VkDevice device,
                                                     const VkSwapchainCreateInfoKHR *info,
                                                     const VkAllocationCallbacks *allocator,
                                                     VkSwapchainKHR *swapchain)
{
    device_data *data = find_device(device);
    if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    if (no_patch)
        return data->create_swapchain(device, info, allocator, swapchain);
    const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainCreateInfoKHR patched = *info;
    patched.imageUsage |= copies;
    int copyable = 1;
    VkResult r = data->create_swapchain(device, &patched, allocator, swapchain);
    if (r != VK_SUCCESS) {
        /* Surface refused transfer usage: create it as asked and do not track,
         * rather than issue an illegal copy every frame. */
        copyable = (info->imageUsage & copies) == copies;
        nr_log("[nr_layer] patched usage refused (0x%x); creating as asked, copyable=%d",
             (unsigned)r, copyable);
        r = data->create_swapchain(device, info, allocator, swapchain);
    }
    if (r != VK_SUCCESS) return r;
    if (!copyable) {
        nr_log("[nr_layer] swapchain refused transfer usage; capture off");
        return r;
    }
    if (info->imageArrayLayers != 1
        || (info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) {
        nr_log("[nr_layer] swapchain is multi-layer or protected; capture off");
        return r;
    }
    if (!format_is_four_bytes(info->imageFormat)) {
        nr_log("[nr_layer] swapchain format %d is not four bytes a pixel; "
             "leaving it alone", info->imageFormat);
        return r;
    }
    lock();
    swapchain_data *entry = NULL;
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (!swapchains[i].swapchain) { entry = &swapchains[i]; break; }
    if (!entry)
        nr_log("[nr_layer] all %d swapchain slots are in use; not tracked", MAX_SWAPCHAINS);
    if (entry) {
        memset(entry, 0, sizeof *entry);
        entry->swapchain = *swapchain;
        entry->device = device;
        entry->format = info->imageFormat;
        entry->extent = info->imageExtent;
        entry->image_count = MAX_IMAGES;
        VkResult got = data->get_swapchain_images(device, *swapchain,
                                                  &entry->image_count, entry->images);
        if (got != VK_SUCCESS || !entry->image_count) {
            memset(entry, 0, sizeof *entry);
            unlock();
            nr_log("[nr_layer] swapchain has more than %d images or none; capture off",
                 MAX_IMAGES);
            return r;
        }
        nr_log("[nr_layer] swapchain %ux%u format %d, %u images",
             entry->extent.width, entry->extent.height, entry->format,
             entry->image_count);
    }
    unlock();
    return r;
}

static void release_device(device_data *data)
{
    if (data->mapped) {
        PFN_vkUnmapMemory unmap =
            (PFN_vkUnmapMemory)data->get_device_proc(data->device, "vkUnmapMemory");
        if (unmap) unmap(data->device, data->staging_memory);
    }
    if (data->staging) {
        PFN_vkDestroyBuffer destroy =
            (PFN_vkDestroyBuffer)data->get_device_proc(data->device, "vkDestroyBuffer");
        if (destroy) destroy(data->device, data->staging, NULL);
    }
    if (data->staging_memory) {
        PFN_vkFreeMemory release =
            (PFN_vkFreeMemory)data->get_device_proc(data->device, "vkFreeMemory");
        if (release) release(data->device, data->staging_memory, NULL);
    }
    if (data->pool) {
        PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)
            data->get_device_proc(data->device, "vkDestroyCommandPool");
        if (destroy) destroy(data->device, data->pool, NULL);
    }
    if (data->async) {
        /* result/fgi may be POOL buffers (zero-copy publish in
         * async_drain) - reclaim them first or free() below double-frees
         * what nr_async_destroy is about to release itself. */
        if (nr_async_claim(data->async, data->result))
            data->result = NULL;
        if (nr_async_claim(data->async, data->fgi))
            data->fgi = NULL;
        nr_async_destroy(data->async);
        data->async = NULL;
    }
    if (data->warp) {
        nr_warp_destroy(data->warp);
        data->warp = NULL;
    }
    /* The residual module owns plain mallocs (its src copy + res delta),
     * never pool buffers - safe to destroy after the pool is gone. */
    if (data->resid) {
        nr_residual_destroy(data->resid);
        data->resid = NULL;
    }
    /* GPU compose: waits its own fence first, then frees every Vulkan
     * object it created on this device. NULL-safe on partial init. */
    if (data->gpuwarp) {
        nr_gpuwarp_destroy(data->gpuwarp);
        data->gpuwarp = NULL;
    }
    free(data->result);
    free(data->earlier);
    free(data->raw);
    free(data->interp);
    free(data->mid);
    free(data->fgi);
    data->interp = NULL;
    data->interp_size = 0;
    data->mid = NULL;
    data->mid_size = 0;
    data->mid_valid = 0;
    data->fgi = NULL;
    data->fgi_size = 0;
    data->fgi_pending = 0;
}

VKAPI_ATTR void VKAPI_CALL nr_DestroyDevice(VkDevice device,
                                            const VkAllocationCallbacks *allocator)
{
    device_data *data = find_device(device);
    PFN_vkDestroyDevice next = data ? data->destroy_device : NULL;
    if (data) {
        lock();
        for (int i = 0; i < MAX_SWAPCHAINS; i++)
            if (swapchains[i].device == device)
                memset(&swapchains[i], 0, sizeof swapchains[i]);
        for (int i = 0; i < MAX_QUEUES; i++)
            if (queues[i].device == device)
                memset(&queues[i], 0, sizeof queues[i]);
        unlock();
        release_device(data);
        memset(data, 0, sizeof *data);
    }
    if (next) next(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL nr_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks *allocator)
{
    device_data *data = find_device(device);
    lock();
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (swapchains[i].swapchain == swapchain)
            memset(&swapchains[i], 0, sizeof swapchains[i]);
    /* A held result belongs to one swapchain; never re-blit it into another
     * (device resets recreate swapchains, sometimes at the same size). */
    for (int i = 0; i < 8; i++)
        if (devices[i].holding && devices[i].holding_chain == swapchain) {
            devices[i].holding = 0;
            devices[i].holding_chain = VK_NULL_HANDLE;
        }
    /* Same for the freeze latch: the held raw frame belongs to exactly one
     * swapchain. If that chain dies while frozen, drop the held state so the
     * next present re-captures the new primary instead of re-blitting a dead
     * chain's frame (or worse, latching onto a menu strip). */
    for (int i = 0; i < 8; i++)
        if (devices[i].raw_held && devices[i].raw_chain == swapchain) {
            devices[i].raw_held = 0;
            devices[i].raw_chain = VK_NULL_HANDLE;
            devices[i].result_valid = 0;
            nr_log("[nr_layer] frozen primary swapchain destroyed; "
                   "held frame dropped");
        }
    unlock();
    if (data && data->destroy_swapchain)
        data->destroy_swapchain(device, swapchain, allocator);
}

static uint32_t memory_type(device_data *data, uint32_t bits,
                            VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties properties;
    PFN_vkGetPhysicalDeviceMemoryProperties get =
        (PFN_vkGetPhysicalDeviceMemoryProperties)next_instance_proc(
            layer_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!get) return UINT32_MAX;
    get(data->physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (properties.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int ensure_resources(device_data *data, VkDeviceSize needed)
{
    if (!data->pool) {
        VkCommandPoolCreateInfo info;
        memset(&info, 0, sizeof info);
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = data->queue_family;
        PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)
            data->get_device_proc(data->device, "vkCreateCommandPool");
        if (create(data->device, &info, NULL, &data->pool) != VK_SUCCESS) return -1;
    }
    if (data->staging_size >= needed) return 0;

    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)
        data->get_device_proc(data->device, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements requirements = (PFN_vkGetBufferMemoryRequirements)
        data->get_device_proc(data->device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)
        data->get_device_proc(data->device, "vkAllocateMemory");
    PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)
        data->get_device_proc(data->device, "vkBindBufferMemory");
    PFN_vkMapMemory map = (PFN_vkMapMemory)
        data->get_device_proc(data->device, "vkMapMemory");
    VkBufferCreateInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = needed;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)
        data->get_device_proc(data->device, "vkDestroyBuffer");
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)
        data->get_device_proc(data->device, "vkFreeMemory");
    PFN_vkUnmapMemory unmap_memory = (PFN_vkUnmapMemory)
        data->get_device_proc(data->device, "vkUnmapMemory");

    /* Stage into locals and swap in only after every step succeeds: a failed
     * bind/map must not leave mapped==NULL under a non-zero staging_size, and
     * the superseded buffer is released instead of leaked per grow. */
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (create_buffer(data->device, &info, NULL, &staging) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    requirements(data->device, staging, &mr);
    uint32_t type = memory_type(data, mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = memory_type(data, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type != UINT32_MAX) {
        VkMemoryAllocateInfo allocation;
        memset(&allocation, 0, sizeof allocation);
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = mr.size;
        allocation.memoryTypeIndex = type;
        if (allocate(data->device, &allocation, NULL, &staging_memory) == VK_SUCCESS
            && bind(data->device, staging, staging_memory, 0) == VK_SUCCESS
            && map(data->device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS
            && mapped) {
            VkBuffer old = data->staging;
            VkDeviceMemory old_mem = data->staging_memory;
            int old_mapped = (data->mapped != NULL);
            data->staging = staging;
            data->staging_memory = staging_memory;
            data->mapped = mapped;
            data->staging_size = needed;
            data->have_earlier = 0; /* old-extent bytes must not feed memcmp */
            /* A live async result is a POOL buffer (zero-copy publish) -
             * claim() returns it to the pool; freeing it directly would
             * leave a dangling a->buf[] slot and double-free at destroy. */
            if (data->async && nr_async_claim(data->async, data->result))
                data->result = NULL;
            free(data->result);
            data->result = (unsigned char *)malloc((size_t)needed);
            data->result_size = needed;
            data->result_valid = 0; /* the old last-processed frame is gone */
            if (old && destroy_buffer) destroy_buffer(data->device, old, NULL);
            if (old_mem) {
                if (old_mapped && unmap_memory)
                    unmap_memory(data->device, old_mem);
                if (free_memory) free_memory(data->device, old_mem, NULL);
            }
            return data->result ? 0 : -1;
        }
    }
    if (mapped && unmap_memory) unmap_memory(data->device, staging_memory);
    if (staging && destroy_buffer) destroy_buffer(data->device, staging, NULL);
    if (staging_memory && free_memory) free_memory(data->device, staging_memory, NULL);
    nr_log("[nr_layer] staging alloc failed (%llu bytes)", (unsigned long long)needed);
    return -1;
}

static int ensure_ring(device_data *data)
{
    if (data->ring_ready) return 0;
    if (!data->pool) return -1;
    PFN_vkAllocateCommandBuffers allocate_commands = (PFN_vkAllocateCommandBuffers)
        data->get_device_proc(data->device, "vkAllocateCommandBuffers");
    PFN_vkCreateFence create_fence = (PFN_vkCreateFence)
        data->get_device_proc(data->device, "vkCreateFence");
    PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)
        data->get_device_proc(data->device, "vkCreateSemaphore");
    if (!allocate_commands || !create_fence || !create_semaphore) return -1;
    VkCommandBufferAllocateInfo alloc;
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = data->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = SYNC_SLOTS;
    if (allocate_commands(data->device, &alloc, data->ring_commands) != VK_SUCCESS)
        return -1;
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < SYNC_SLOTS; i++) {
        if (create_fence(data->device, &fi, NULL, &data->ring_fence[i]) != VK_SUCCESS
            || create_semaphore(data->device, &si, NULL, &data->ring_done[i]) != VK_SUCCESS)
            return -1;
        data->ring_used[i] = 0;
    }
    data->ring_ready = 1;
    return 0;
}

static int diag_active(void);   /* fwd: per-present diagnostics gate */

/* Diagnostic marker (NR_LAYER_DIAG): stamp magic + frame counter into the
 * first pixels of every uploaded buffer. The staging image is host-visible
 * so the GPU copy picks this up at execution. A screen-captured "insert"
 * frame carrying this mark proves it left through our upload path; without
 * it, the insert arrived through a non-Vulkan surface. */
static void stamp_diag_marker(device_data *data)
{
    if (diag_active() && data->mapped && data->staging_size >= 8) {
        unsigned char *mk = (unsigned char *)data->mapped;
        mk[0] = 'N'; mk[1] = 'R'; mk[2] = 'M'; mk[3] = 'K';
        mk[4] = (unsigned char)(frame_counter & 0xFF);
        mk[5] = (unsigned char)((frame_counter >> 8) & 0xFF);
        mk[6] = 0; mk[7] = 0;
    }
}

/* In-game UI overlay (nr_ui.c): blend the shared-memory panel into the
 * frame while it still sits in the host-visible staging buffer, before
 * the copy command is recorded. Every write-back path out - processed,
 * held, generated, frozen, GPU-composited - funnels through this one
 * point. Absent mapping / bad magic / hidden UI = zero behaviour. */
static void composite_ui_into_mapped(device_data *data,
                                     swapchain_data *chain)
{
    int order = frame_byte_order(chain->format);
    if (order >= 0 && data->mapped
        && data->staging_size >= (VkDeviceSize)chain->extent.width
                                 * chain->extent.height * 4) {
        NrUiFrame ui_frame;
        ui_frame.mapped = (unsigned char *)data->mapped;
        ui_frame.width = chain->extent.width;
        ui_frame.height = chain->extent.height;
        ui_frame.rgba_order = order;
        nr_ui_composite(&ui_frame);
    }
}

/* Move the presented frame between the swapchain image and the staging
 * buffer. idle sync = vkQueueWaitIdle around the transfer (default, proven);
 * semaphore sync = take over the present's wait semaphores (NR_LAYER_SYNC). */
static int transfer(device_data *data, swapchain_data *chain, VkQueue queue,
                    uint32_t index, int to_image)
{
    if (to_image) {
        stamp_diag_marker(data);
        composite_ui_into_mapped(data, chain);
        /* Keep the window's redirection bitmap a mirror of what we are
         * about to blit into the swapchain image: DWM splices that bitmap
         * over the live surface on repaint events, and a stale bitmap is
         * the menu-flash artifact seen on GTA IV. Throttled internally. */
        nr_guard_feed_surface(data->mapped, chain->extent.width,
                              chain->extent.height,
                              frame_byte_order(chain->format) == 1);
    }
    PFN_vkAllocateCommandBuffers allocate_commands = (PFN_vkAllocateCommandBuffers)
        data->get_device_proc(data->device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)
        data->get_device_proc(data->device, "vkBeginCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)
        data->get_device_proc(data->device, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer copy_out = (PFN_vkCmdCopyImageToBuffer)
        data->get_device_proc(data->device, "vkCmdCopyImageToBuffer");
    PFN_vkCmdCopyBufferToImage copy_in = (PFN_vkCmdCopyBufferToImage)
        data->get_device_proc(data->device, "vkCmdCopyBufferToImage");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)
        data->get_device_proc(data->device, "vkEndCommandBuffer");
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)
        data->get_device_proc(data->device, "vkQueueSubmit");
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)
        data->get_device_proc(data->device, "vkQueueWaitIdle");
    PFN_vkFreeCommandBuffers free_commands = (PFN_vkFreeCommandBuffers)
        data->get_device_proc(data->device, "vkFreeCommandBuffers");

    int ringed = sync_semaphores && !data->present_plain && ensure_ring(data) == 0;
    unsigned slot = data->ring_next % SYNC_SLOTS;
    VkCommandBuffer commands;
    if (ringed) {
        if (data->ring_used[slot]) {
            PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)
                data->get_device_proc(data->device, "vkWaitForFences");
            PFN_vkResetFences reset_fences = (PFN_vkResetFences)
                data->get_device_proc(data->device, "vkResetFences");
            wait_fences(data->device, 1, &data->ring_fence[slot], VK_TRUE,
                        10ull * 1000000000ull);
            reset_fences(data->device, 1, &data->ring_fence[slot]);
        }
        commands = data->ring_commands[slot];
        data->ring_next++;
    } else {
        VkCommandBufferAllocateInfo alloc;
        memset(&alloc, 0, sizeof alloc);
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = data->pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (allocate_commands(data->device, &alloc, &commands) != VK_SUCCESS) return -1;
    }
    VkCommandBufferBeginInfo beginning;
    memset(&beginning, 0, sizeof beginning);
    beginning.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginning.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin(commands, &beginning);

    VkImageLayout working = to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier into;
    memset(&into, 0, sizeof into);
    into.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    into.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    into.dstAccessMask = to_image ? VK_ACCESS_TRANSFER_WRITE_BIT
                                  : VK_ACCESS_TRANSFER_READ_BIT;
    into.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    into.newLayout = working;
    into.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.image = chain->images[index];
    into.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    into.subresourceRange.baseMipLevel = 0;
    into.subresourceRange.levelCount = 1;
    into.subresourceRange.baseArrayLayer = 0;
    into.subresourceRange.layerCount = 1;
    barrier(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &into);

    VkBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = chain->extent.width;
    region.imageExtent.height = chain->extent.height;
    region.imageExtent.depth = 1;
    if (to_image)
        copy_in(commands, data->staging, chain->images[index], working, 1, &region);
    else
        copy_out(commands, chain->images[index], working, data->staging, 1, &region);

    VkImageMemoryBarrier back = into;
    back.srcAccessMask = into.dstAccessMask;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back.oldLayout = working;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    end(commands);

    if (!ringed) {
        wait(queue);
        VkSubmitInfo submission;
        memset(&submission, 0, sizeof submission);
        submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &commands;
        submit(queue, 1, &submission, VK_NULL_HANDLE);
        wait(queue);
        free_commands(data->device, data->pool, 1, &commands);
        return 0;
    }

    /* Semaphore mode: wait on the present's own waits; only the write-back
     * signals (a binary semaphore may not be signalled twice; the readback is
     * fenced by the host instead). */
    VkSemaphore signal = to_image ? data->ring_done[slot] : VK_NULL_HANDLE;
    VkPipelineStageFlags stages[NR_MAX_WAITS];
    uint32_t waits = data->present_wait_count;
    for (uint32_t i = 0; i < waits; i++) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submission;
    memset(&submission, 0, sizeof submission);
    submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submission.waitSemaphoreCount = waits;
    submission.pWaitSemaphores = waits ? data->present_wait : NULL;
    submission.pWaitDstStageMask = waits ? stages : NULL;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    submission.signalSemaphoreCount = signal ? 1 : 0;
    submission.pSignalSemaphores = signal ? &signal : NULL;
    if (submit(queue, 1, &submission, data->ring_fence[slot]) != VK_SUCCESS) {
        data->ring_used[slot] = 0;
        return -1;
    }
    data->ring_used[slot] = 1;
    data->present_wait_count = 0;
    if (signal) data->present_signal = signal;
    if (!to_image) {
        PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)
            data->get_device_proc(data->device, "vkWaitForFences");
        if (wait_fences(data->device, 1, &data->ring_fence[slot], VK_TRUE,
                        10ull * 1000000000ull) != VK_SUCCESS)
            return -1;
    }
    return 0;
}

/* NR_LAYER_CAPTURE_OUT: dump header+processed result for A/B stills. */
/* Near-identity reuse thresholds. "Static" game content is rarely bit-static:
 * film grain, dither and cursor sprites shift a few percent of pixels by a few
 * lsb every present, so the exact memcmp gate never fires, every present runs
 * a full reprocess, and the slightly-different results land ~70 ms apart -
 * the output visibly "swims" on content the eye reads as static. Treating
 * sub-perceptual deltas as repeats freezes the processed frame until real
 * drift accumulates. */
static int  eps_state = -1;      /* -1 uninited, 0 off, 1 on */
static long eps_mean_x100 = 120; /* mean |delta| over sampled rgb, hundredths */
static int  eps_pix = 24;        /* per-channel delta that counts as real change */
static int  eps_big_pm = 20;     /* allowed share of big-delta pixels, per-mille */

static void eps_init(void)
{
    const char *e;
    if (eps_state >= 0) return;
    /* The sampled-eps path is OPT-IN: it replays a STALE processed result
     * for content that merely looks similar, which on slow pans / animated
     * grain sticks the displayed image then jumps it (stick-slip judder),
     * and the forced reprocess after reuse_cap lands on drifted temporal
     * state. NR_LAYER_NEAR_REUSE=1 re-enables it; absent or any other
     * value keeps it OFF - bit-exact memcmp repeats still reuse.
     * NR_LAYER_EPS=0 remains a hard kill switch. */
    e = getenv("NR_LAYER_NEAR_REUSE");
    eps_state = (e && !strcmp(e, "1")) ? 1 : 0;
    e = getenv("NR_LAYER_EPS");
    if (e && !strcmp(e, "0")) eps_state = 0;
    e = getenv("NR_LAYER_EPS_MEAN"); if (e) eps_mean_x100 = atol(e);
    e = getenv("NR_LAYER_EPS_PIX");  if (e) eps_pix = atoi(e);
    e = getenv("NR_LAYER_EPS_BIG");  if (e) eps_big_pm = atoi(e);
}

/* Max consecutive near-identical replays before a forced reprocess. Bounds
 * the staleness any missed-motion pattern can accumulate; 0 disables the
 * near-identical path entirely (bit-identical repeats still reuse free).
 * Moot unless NR_LAYER_NEAR_REUSE=1 armed eps_state in eps_init. */
static unsigned reuse_cap(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("NR_LAYER_REUSE_CAP");
        cap = e ? atoi(e) : 8;
        if (cap < 0) cap = 0;
    }
    return (unsigned)cap;
}

/* Sampled near-equality of two BGRA frames: every 4th pixel, rgb channels
 * (alpha is often garbage). ~0.3 ms on 800x600. w = frame width in pixels;
 * used to bound-box the big-delta pixels for the concentration test below. */
static int nearly_identical(const unsigned char *a, const unsigned char *b,
                            size_t n, uint32_t w)
{
    uint64_t sum = 0, big = 0, samples = 0;
    size_t npix = n >> 2, p;
    uint32_t xmin = 0xFFFFFFFFu, xmax = 0, ymin = 0xFFFFFFFFu, ymax = 0;
    eps_init();
    if (!eps_state) return 0;
    if (!w) return 0;
    for (p = 0; p < npix; p += 4) {
        size_t o = p << 2;
        int d0 = (int)a[o]   - (int)b[o];
        int d1 = (int)a[o+1] - (int)b[o+1];
        int d2 = (int)a[o+2] - (int)b[o+2];
        int m;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (d2 < 0) d2 = -d2;
        m = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
        sum += (uint64_t)(d0 + d1 + d2);
        if (m > eps_pix) {
            uint32_t x = (uint32_t)(p % w), y = (uint32_t)(p / w);
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
            big++;
        }
        samples++;
    }
    if (!samples) return 0;
    /* Concentrated change = a real moving element (cursor sprite, blinking
     * HUD bit, the edge of a nodding head) riding under the global share
     * limit. Film grain is diffuse: its big pixels scatter over the whole
     * frame so the density inside their bounding box stays tiny, while a
     * moving sprite packs them densely. Reusing a stale result over a
     * concentrated move reads as ghost trails and judder on screen. */
    if (big >= 48 && xmax > xmin && ymax > ymin) {
        uint64_t area = (uint64_t)(xmax - xmin + 1) * (ymax - ymin + 1);
        if (area && (big * 100) / area >= 4)
            return 0;
    }
    return (sum * 100) / (samples * 3) <= (uint64_t)eps_mean_x100 &&
           (big * 1000) / samples <= (uint64_t)eps_big_pm;
}

static void dump_capture(const char *path, const uint32_t *header,
                         const unsigned char *pixels, VkDeviceSize size)
{
    if (!path) return;
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(header, sizeof(uint32_t), 4, file);
    fwrite(pixels, 1, (size_t)size, file);
    fclose(file);
}

/* Per-present diagnostics (NR_LAYER_DIAG=1): one line per live-mode present
 * naming the path that produced it plus a cheap CRC of the buffer that was
 * uploaded to the swapchain image. Lets a captured "insert" frame be traced
 * back to the exact code path that produced (or failed to overwrite) it. */
static int diag_enabled = -1;
static const char *diag_path = "?";
static int diag_active(void)
{
    if (diag_enabled < 0) {
        const char *e = getenv("NR_LAYER_DIAG");
        diag_enabled = (e && *e && *e != '0');
    }
    return diag_enabled;
}
static uint32_t diag_crc(const void *p, VkDeviceSize n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    if (!b || !n) return 0;
    for (VkDeviceSize i = 0; i < n; i += 4093)
        h = (h ^ b[i]) * 16777619u;
    h = (h ^ b[n - 1]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ */
/* Async daemon submit (NR_LAYER_ASYNC, TCP path).                      */
/* ------------------------------------------------------------------ */
/* The present path submits the captured frame to a worker mailbox
 * (bounded, drop-oldest; nr_async.c) and displays the newest reply it
 * already holds. The ~60 ms m11d round-trip therefore runs in parallel
 * with the game instead of inside its present. All state transitions
 * that feed the hold/reuse paths (result, fgi slot, nr_degrade streaks)
 * are applied HERE, on the present thread, when the done mailbox is
 * drained - the worker itself touches only its own buffers + sockets. */

static void async_lazy_start(device_data *data)
{
    if (!g_async || !nr_port || data->async || data->async_failed) return;
    data->async = nr_async_create();
    if (data->async) {
        nr_log("[nr_layer] async daemon submit started (bounded queue, "
               "drop-oldest, stale bound %ld ms)", g_async_stale_ms);
        if (g_residual && !data->resid) {
            data->resid = nr_residual_create();
            nr_residual_configure(data->resid, g_resid_ms,
                                  g_resid_diff, g_resid_big, g_resid_px);
            nr_log("[nr_layer] serve-residual compositor armed (age bound "
                   "%ld ms, pixel eps %ld, cut bound %ld.%02ld, "
                   "moved share %ld pm)",
                   g_resid_ms, g_resid_px, g_resid_diff / 100,
                   g_resid_diff % 100, g_resid_big);
            if (g_warp && !data->warp) {
                data->warp = nr_warp_create();
                if (data->warp) {
                    nr_warp_configure(data->warp, g_warp_maxoff,
                                      g_warp_cap, g_warp_budget);
                    nr_residual_set_warp(data->resid, data->warp);
                    nr_log("[nr_layer] residual warp probe armed "
                           "(maxoff %ld px, cap %ld, budget %ld pm)",
                           g_warp_maxoff, g_warp_cap, g_warp_budget);
                }
            }
            /* GPU compose (opt-in): pipelines on the game's own device +
             * present-queue family. Any failure leaves gpuwarp NULL and
             * the CPU masked path keeps serving. */
            if (g_gpuwarp && !data->gpuwarp) {
                data->gpuwarp = nr_gpuwarp_create(
                    data->physical, data->device, data->queue_family,
                    data->get_device_proc,
                    (nr_gw_mem_type_fn)memory_type, data,
                    (nr_gw_fam_ok_fn)family_can_capture, data,
                    g_gpuwarp_radius, g_gpuwarp_dbg);
                if (data->gpuwarp)
                    nr_gpuwarp_configure(data->gpuwarp,
                                         g_gpuwarp_cut, g_gpuwarp_nh);
                nr_log("[nr_layer] GPU residual compose %s",
                       data->gpuwarp ? "armed" : "unavailable");
            }
        }
    } else {
        data->async_failed = 1;
        nr_log("[nr_layer] async worker failed to start; staying sync");
    }
}

/* Publish every completed daemon reply (latest-wins mailbox). PROC
 * replies swap straight into data->result (zero-copy: the reply buffer
 * becomes the held result, the old result buffer returns to the pool);
 * FGT/FGI replies land in the single-shot fgi slot - generated frames
 * are display content, never anchors, so they stay out of result.
 * Transport reports feed nr_degrade exactly as the inline exchange did;
 * a failed PROC submission also clears have_earlier so even identical
 * content resubmits next present (the sync earlier_pending semantics). */
static int async_drain(device_data *data)
{
    NrAsyncDone d;
    int landed = 0;
    if (!data->async) return 0;
    while (nr_async_take_done(data->async, &d)) {
        if (d.erc) {
            int hold = 0;
            int lg = nr_degrade_note_fail(&g_dg,
                d.erc == -2 ? NR_DEGRADE_FAIL_CONNECT
                            : NR_DEGRADE_FAIL_TRANSPORT,
                nr_now_ms(), &hold);
            if (d.kind != NR_ASYNC_PROC)
                nr_degrade_fg_note_fail(&g_dg);
            data->have_earlier = 0;
            if (lg == NR_DEGRADE_LOG_HICCUP)
                nr_log("[nr_layer] daemon hiccup; holding last processed "
                       "frame (fail %u)", g_dg.consecutive_fail);
            else if (lg == NR_DEGRADE_LOG_DOWN)
                nr_log("[nr_layer] daemon considered down; native "
                       "passthrough");
            continue;
        }
        if (nr_degrade_note_success(&g_dg, nr_now_ms())
                == NR_DEGRADE_LOG_RECOVERY) {
            nr_log("[nr_layer] daemon answered again; processed output "
                   "resumed");
            if (g_dg.fg_fail) {
                nr_degrade_fg_note_success(&g_dg);
                nr_log("[nr_layer] daemon recovered; framegen re-probed "
                       "on next generated present");
            }
        }
        if (d.kind != NR_ASYNC_PROC)
            nr_degrade_fg_note_success(&g_dg);
        if (d.kind == NR_ASYNC_PROC) {
            unsigned char *old = data->result;
            /* Serve-residual pair update BEFORE the zero-copy publish:
             * d.src is the exact submitted frame this reply answers and
             * d.buf is its full-res processed output - res = proc - src
             * is computed once inside the module (channels_ok fails on
             * packed 10-bit formats). A NULL src means the pool was dry:
             * the previous pair simply rides on. */
            if (data->resid && d.src) {
                uint64_t stamp = nr_now_ms();
                int upd = nr_residual_update(data->resid, d.src, d.buf,
                                             d.w, d.h, d.chain, d.seq,
                                             frame_byte_order(
                                                 (VkFormat)d.fmt) >= 0,
                                             stamp);
                /* GPU mirror: stage the SAME (src, res) bytes the CPU
                 * mask composes with. The device upload itself rides the
                 * next eligible present's submit - here it is only a
                 * CPU-side shadow copy + dirty flag. */
                if (upd && data->gpuwarp) {
                    const unsigned char *psrc;
                    const signed char *pres;
                    uint32_t pw, ph;
                    uint64_t pchain, pstamp;
                    if (nr_residual_pair(data->resid, &psrc, &pres,
                                         &pw, &ph, &pchain, &pstamp))
                        nr_gpuwarp_set_pair(data->gpuwarp, psrc, pres,
                                            pw, ph, pchain, pstamp);
                }
            }
            else if (data->resid && !d.src) {
                static unsigned long src_misses;
                if ((++src_misses & 63) == 1)
                    nr_log("[nr_layer] async reply without source copy "
                           "(pool pressure); residual pair kept (x%lu)",
                           src_misses);
            }
            data->result = d.buf;          /* zero-copy publish */
            data->result_size = d.buf_bytes;
            d.buf = old;
            data->result_w = d.w;
            data->result_h = d.h;
            data->result_valid = 1;
            data->holding_chain = (VkSwapchainKHR)d.chain;
            data->result_ms = nr_now_ms();
            data->result_seq = d.seq;
            /* Content identity of the frame this result answers: the
             * serve freshness test matches it against the capture-CRC
             * ring (fix B). src==NULL (pool dry) degrades the test to
             * the submit-age / wall-clock fallback. */
            data->result_src_crc = d.src ? diag_crc(d.src, d.src_bytes) : 0;
            data->result_fmt = d.fmt;
            data->result_age = (data->submit_seq > d.seq)
                               ? data->submit_seq - d.seq : 0;
            landed = 1;
            /* A newer anchor supersedes an undisplayed midpoint: showing
             * a midpoint of an interval that already closed backwards
             * would step the stream in time. */
            if (data->fgi_pending && d.seq > data->fgi_seq)
                data->fgi_pending = 0;
            {
                uint32_t oh[4] = { 0x304E524Eu, d.w, d.h, d.fmt };
                dump_capture(capture_out_path, oh, data->result,
                             d.buf_bytes);
            }
        } else {
            unsigned char *old = data->fgi;
            data->fgi = d.buf;
            data->fgi_size = d.buf_bytes;
            d.buf = old;
            data->fgi_pending = 1;
            data->fgi_seq = d.seq;
            data->fgi_ms = nr_now_ms();
            data->fgi_chain = d.chain;
            data->fgi_w = d.w;
            data->fgi_h = d.h;
            nr_log("[nr_layer] daemon framegen reply %ux%u seq=%lu "
                   "awaiting display", d.w, d.h, d.seq);
        }
        /* The swapped-out result/fgi slot is pool-owned once a zero-copy
         * publish has happened, but it starts life as the staging-path
         * malloc - claim() sorts the two (non-pool = ours to free, so the
         * pool never sees a foreign pointer). d.src is pool-owned by
         * construction (the worker only ever fills it from the pool). */
        if (d.buf && !nr_async_claim(data->async, d.buf))
            free(d.buf);
        if (d.src)
            nr_async_give_buf(data->async, d.src);
    }
    return landed;
}

/* Load the pending FG reply into mapped if it belongs to this chain +
 * extent and is still fresh. A stale midpoint is dropped, not shown. */
static int async_take_fgi(device_data *data, swapchain_data *chain,
                          VkDeviceSize want)
{
    if (!data->mapped || data->staging_size < want) return 0;
    if (!data->fgi_pending || !data->fgi || data->fgi_size < want) return 0;
    if (data->fgi_chain != (uint64_t)chain->swapchain) return 0;
    if (data->fgi_w != chain->extent.width
        || data->fgi_h != chain->extent.height) return 0;
    if (g_async_stale_ms > 0
        && nr_now_ms() - data->fgi_ms > (uint64_t)g_async_stale_ms) {
        data->fgi_pending = 0;
        return 0;
    }
    memcpy(data->mapped, data->fgi, (size_t)want);
    data->fgi_pending = 0;
    return 1;
}

/* Low-rate serve-class diag: one line per policy class TRANSITION
 * (residual-served vs stale-served vs raw vs generated), capped at ~3/s
 * with the suppressed flips counted into the next line. cov_pm carries
 * the residual coverage (per-mille) for the "ares" class, -1 elsewhere. */
static void serve_class_note(const char *cls, long cov_pm)
{
    static char last[16];
    static uint64_t last_ms;
    static unsigned long run, skipped;
    uint64_t now = nr_now_ms();
    if (!strcmp(cls, last)) { run++; return; }
    if (now - last_ms >= 300) {
        if (cov_pm >= 0)
            nr_log("[nr_layer] serve class %s -> %s (prev x%lu%s, "
                   "pres=%ld.%ld%%)",
                   last[0] ? last : "none", cls, run,
                   skipped ? ", flips suppressed" : "",
                   cov_pm / 10, cov_pm % 10);
        else
            nr_log("[nr_layer] serve class %s -> %s (prev x%lu%s)",
                   last[0] ? last : "none", cls, run,
                   skipped ? ", flips suppressed" : "");
        last_ms = now;
        skipped = 0;
    } else skipped++;
    run = 0;
    snprintf(last, sizeof last, "%s", cls);
}

/* Unified async output choice for every managed present: a fresh FG
 * reply first (its single display slot), then the serve-residual
 * composite (CURRENT raw + freshest enhancement delta - have_raw must be
 * set, i.e. mapped holds THIS present's captured frame), then the newest
 * PROC result while the reply stream is live, else -2 = "present the
 * current raw frame, do NOT re-blit the stale result" (the caller treats
 * -2 as fail-but-no-hold). The processed-stream staleness bound only
 * applies to NEW content - identical/near-identical input replays the
 * result at any age via the reuse gate (static content cannot go stale
 * visually). mapped holds the raw captured frame on entry; a 0 return
 * means mapped now holds the frame to blit; a 2 return means the GPU
 * compose already wrote the swapchain image (caller skips transfer). */
static int async_serve_best(device_data *data, swapchain_data *chain,
                            VkQueue queue, uint32_t index,
                            VkDeviceSize want, int have_raw)
{
    if (!data->mapped || data->staging_size < want) {
        diag_path = "araw";
        serve_class_note("araw", -1);
        return -2;
    }
    if (async_take_fgi(data, chain, want)) {
        diag_path = "afgi";
        serve_class_note("afgi", -1);
        return 0;
    }
    /* Serve-residual: mapped holds the CURRENT captured raw frame. The
     * composite is per-pixel masked (nr_residual.c): only pixels that
     * still match the pair's source take the delta, moved content shows
     * raw current pixels - the residual can never land displaced.
     * A residual-composited display frame is display content ONLY - it is
     * never written into result/mid/earlier/fgi, so it can never become
     * an anchor or a reuse source (the mode-2 mid frame is captured from
     * mapped BEFORE this call).
     * Reject fallback ordering (g_resid_fb_raw=1, default): the freshest
     * enhancement applicable to content that moved or outlived the pair
     * is NONE - mapped already holds current content, so it goes out as
     * raw rather than re-showing the stale processed frame (which would
     * re-freeze the display at the daemon's reply cadence on exactly the
     * animated content this exists for). Packed 10-bit formats and
     * have_raw=0 presents cannot produce current content either way and
     * keep the legacy stale-first order below. */
    if (have_raw && data->resid) {
        /* GPU compose (nr_gpuwarp, opt-in): the same pair the CPU mask
         * would serve, but the displaced retry is driven by a real
         * quarter-res block-match field computed on the game's own
         * queue. On success the composite already sits in the swapchain
         * image and the caller's staging write-back is skipped (return
         * 2). NR_GPUWARP_SKIP = the pair is ineligible this present (no
         * pair/extent/age) - identical verdict classes to the CPU path,
         * which then runs below. NR_GPUWARP_FAIL poisons the module for
         * the session - the CPU path stays the fallback, so a Vulkan
         * fault can never leave the game on a black frame.
         * Gated off while framegen mode 2 runs: its anchor present needs
         * mapped==P semantics for the mid interpolation, which a direct
         * image write cannot provide - the CPU path serves there. */
        if (data->gpuwarp
            && !(is_framegen_enabled() && framegen_mode() == 2)) {
            int wr = nr_gpuwarp_will_run(data->gpuwarp,
                                         chain->extent.width,
                                         chain->extent.height,
                                         (uint64_t)chain->swapchain,
                                         g_resid_ms, nr_now_ms());
            if (wr == NR_GPUWARP_OK) {
                /* Marker + overlay panel must ride the upload (the GPU
                 * compose reads the staging bytes as `cur`; panel pixels
                 * mismatch the pair source so the mask keeps them raw,
                 * exactly like the CPU path's post-compose blend). */
                stamp_diag_marker(data);
                composite_ui_into_mapped(data, chain);
                wr = nr_gpuwarp_run(data->gpuwarp, queue, data->staging,
                                    chain->images[index],
                                    chain->extent.width,
                                    chain->extent.height,
                                    (int)g_resid_px,
                                    sync_semaphores
                                    && !data->present_plain);
                if (wr == NR_GPUWARP_OK) {
                    long cov = nr_gpuwarp_last_cov_pm(data->gpuwarp);
                    if (sync_semaphores && !data->present_plain)
                        data->present_signal =
                            nr_gpuwarp_done_sem(data->gpuwarp);
                    /* The GPU compose bypasses transfer(): the GDI
                     * redirection bitmap must still be fed or it freezes
                     * at the last CPU-path frame and DWM splices it over
                     * the live surface on every input event (the menu
                     * cursor-ghost/flash on GTA IV). staging already holds
                     * cur + marker + UI - position-correct, always fresh. */
                    nr_guard_feed_surface(data->mapped,
                                          chain->extent.width,
                                          chain->extent.height,
                                          frame_byte_order(chain->format)
                                          == 1);
                    diag_path = "gres";
                    serve_class_note("gres", cov);
                    {
                        static uint64_t gcov_ms;
                        uint64_t now = nr_now_ms();
                        if (now - gcov_ms >= 1000) {
                            gcov_ms = now;
                            nr_log("[nr_layer] gpuwarp serve "
                                   "pres=%ld.%ld%% (warp +%ld.%ld%%, "
                                   "unmatched %ld.%ld%%)",
                                   cov / 10, cov % 10,
                                   nr_gpuwarp_last_warp_pm(
                                       data->gpuwarp) / 10,
                                   nr_gpuwarp_last_warp_pm(
                                       data->gpuwarp) % 10,
                                   nr_gpuwarp_last_unmatched_pm(
                                       data->gpuwarp) / 10,
                                   nr_gpuwarp_last_unmatched_pm(
                                       data->gpuwarp) % 10);
                        }
                    }
                    return 2;
                }
                nr_log("[nr_layer] GPU compose submit failed; CPU "
                       "masked path takes over");
            }
        }
        int v = nr_residual_apply(data->resid,
                                  (unsigned char *)data->mapped,
                                  chain->extent.width, chain->extent.height,
                                  (uint64_t)chain->swapchain, nr_now_ms());
        if (v == NR_RESIDUAL_APPLIED) {
            /* Coverage = the field signal: pres~100% = full enhancement,
             * low% = mostly raw current pixels (still correct - moved
             * content shows the CURRENT frame, just unenhanced). Logged
             * at ~1 Hz while the class stays ares. */
            long cov = nr_residual_last_cov_pm(data->resid);
            static uint64_t cov_ms;
            uint64_t now = nr_now_ms();
            diag_path = "ares";
            serve_class_note("ares", cov);
            if (now - cov_ms >= 1000) {
                cov_ms = now;
                if (data->warp)
                    nr_log("[nr_layer] residual serve pres=%ld.%ld%% "
                           "(diff %ld.%02ld, warp +%ld.%ld%% %lu px "
                           "%lu pr hint %ld,%ld)",
                           cov / 10, cov % 10,
                           nr_residual_last_diff_x100(data->resid) / 100,
                           nr_residual_last_diff_x100(data->resid) % 100,
                           nr_residual_last_recov_pm(data->resid) / 10,
                           nr_residual_last_recov_pm(data->resid) % 10,
                           nr_warp_fails(data->warp),
                           nr_warp_probed(data->warp),
                           nr_warp_hint_y(data->warp),
                           nr_warp_hint_x(data->warp));
                else
                    nr_log("[nr_layer] residual serve pres=%ld.%ld%% "
                           "(diff %ld.%02ld)",
                           cov / 10, cov % 10,
                           nr_residual_last_diff_x100(data->resid) / 100,
                           nr_residual_last_diff_x100(data->resid) % 100);
            }
            return 0;
        }
        if (g_resid_fb_raw && frame_byte_order(chain->format) >= 0) {
            diag_path = "arej";
            serve_class_note("arej",
                             nr_residual_last_cov_pm(data->resid));
            return -2;
        }
    }
    if (data->result_valid && data->result
        && data->result_size >= want
        && data->result_w == chain->extent.width
        && data->result_h == chain->extent.height
        && data->holding_chain == chain->swapchain
        && (!data->result_fmt
            || data->result_fmt == (uint32_t)chain->format)) {
        /* Freshness = CONTENT match, not the clock: the held result is
         * fresh when its source frame is still among the last
         * NR_LAYER_STALE_MATCH captured uploads (a reply that lands
         * "late" on a static or re-presented scene still answers what
         * the user sees). Replies that arrived without their source copy
         * (pool dry) fall back to the submit-age count, then to the
         * wall-clock bound. With serve-stale on, freshness only picks
         * the diag tag - a stale coherent frame is always served over
         * raw. serve_stale=0 reproduces the legacy clock-only policy. */
        int fresh;
        if (!g_serve_stale) {
            fresh = (g_async_stale_ms <= 0
                     || nr_now_ms() - data->result_ms
                        <= (uint64_t)g_async_stale_ms);
            if (fresh) {
                memcpy(data->mapped, data->result, (size_t)want);
                diag_path = "aserv";
                serve_class_note("aserv", -1);
                return 0;
            }
            diag_path = "astale";
            serve_class_note("astale", -1);
            return -2;
        }
        if (data->result_src_crc) {
            /* Two ways to be content-fresh: the reply is only a few
             * submits behind the newest one (the pacing gate keeps this
             * <= ~2), or its source still matches one of the last
             * STALE_MATCH captured uploads (static/re-presented scene,
             * however the submits were spaced). */
            unsigned lim = data->cap_fill < (unsigned)g_stale_match
                           ? data->cap_fill : (unsigned)g_stale_match;
            unsigned k;
            fresh = (data->result_age <= (unsigned long)g_stale_match);
            for (k = 0; !fresh && k < lim; k++) {
                unsigned at = (data->cap_next + NR_CAP_RING - 1 - k)
                              % NR_CAP_RING;
                if (data->cap_crc[at] == data->result_src_crc) {
                    fresh = 1;
                    break;
                }
            }
        } else {
            fresh = (data->result_age <= (unsigned long)g_stale_match)
                || (g_async_stale_ms <= 0
                    || nr_now_ms() - data->result_ms
                       <= (uint64_t)g_async_stale_ms);
        }
        memcpy(data->mapped, data->result, (size_t)want);
        diag_path = fresh ? "aserv" : "stale";
        serve_class_note(diag_path, -1);
        return 0;
    }
    diag_path = "araw";
    serve_class_note("araw", -1);
    return -2;
}

static int process_frame(device_data *data, swapchain_data *chain,
                         VkQueue queue, uint32_t index)
{
    /* Degenerate swapchain gate: tiny menu/letterbox sub-elements are shown
     * native without a transfer, engine call, or TCP exchange - processing
     * them only wastes a full daemon engine re-init per appearance. */
    int deg_log = NR_DEGRADE_LOG_NONE;
    if (nr_degrade_is_degenerate(&g_dg, chain->extent.width,
                                 chain->extent.height, &deg_log)) {
        if (deg_log == NR_DEGRADE_LOG_DEGENERATE)
            nr_log("[nr_layer] degenerate swapchain %ux%u; passing through",
                   chain->extent.width, chain->extent.height);
        diag_path = "degen";
        return -1;
    }
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
    if (ensure_resources(data, needed)) {
        nr_log("[nr_layer] ensure_resources failed (%ux%u); raw out (frame %lu)",
               chain->extent.width, chain->extent.height, frame_counter);
        diag_path = "eres";
        return -1;
    }
    if (transfer(data, chain, queue, index, 0)) {
        nr_log("[nr_layer] readback failed (%ux%u); raw out (frame %lu)",
               chain->extent.width, chain->extent.height, frame_counter);
        diag_path = "rbfail";
        return -1;
    }

    /* Capture-CRC ring: every frame that reaches the screen through this
     * path, submitted or not. The serve freshness test asks "does the
     * held result answer content the user recently saw" - repeated
     * bit-identical frames share the same CRC, so a static scene keeps
     * its replies content-fresh however the submits are paced. */
    data->last_in_crc = diag_crc(data->mapped, needed);
    data->cap_crc[data->cap_next] = data->last_in_crc;
    data->cap_next = (data->cap_next + 1) % NR_CAP_RING;
    if (data->cap_fill < NR_CAP_RING) data->cap_fill++;

    /* Exact-repeat reuse: a frame identical to the previous one reuses the
     * last processed result instead of running the chain again. data->result
     * holds the last processed frame (in-process or TCP path). Bit-identical
     * input always replays free (static content, deterministic output);
     * near-identical input replays at most reuse_cap() times in a row so a
     * slow drift or an unfelt motion pattern cannot freeze the picture.
     *
     * Ownership check (ghost-serve fix): `earlier` tracks the last SEEN
     * input, but a content transition whose first present found the async
     * queue busy never reached the daemon - its submit was paced out and
     * `earlier` still got overwritten. memcmp alone then replayed the OLD
     * frame's answer on every identical present forever (the "frozen dark
     * screen while the game shows new content" field bug). Requiring
     * result_src_crc == current-input crc makes the replay provably answer
     * THIS content; a mismatched result falls through to the submit path,
     * which retries until the frame actually reaches the engine. */
    if (static_reuse && data->have_earlier && !data->earlier_pending &&
        data->earlier_size >= needed &&
        data->result_valid && data->result && data->result_size >= needed &&
        data->result_w == chain->extent.width &&
        data->result_h == chain->extent.height &&
        data->holding_chain == chain->swapchain) {
        int bit_ident =
            memcmp(data->mapped, data->earlier, (size_t)needed) == 0
            && data->result_src_crc
            && data->result_src_crc == data->last_in_crc;
        /* The near-identical replay is only honest when the held result
         * answers `earlier` (the content it is being compared to): a reply
         * for an older frame must not ride this branch either. */
        int src_answers_earlier =
            data->result_src_crc
            && data->result_src_crc
                   == diag_crc(data->earlier, needed);
        if (bit_ident ||
            (src_answers_earlier && data->reuse_run < reuse_cap() &&
             nearly_identical((const unsigned char *)data->mapped,
                              data->earlier, (size_t)needed,
                              chain->extent.width))) {
            memcpy(data->mapped, data->result, (size_t)needed);
            if (!bit_ident) data->reuse_run++;
            diag_path = "reuse";
            return 0;
        }
    }

    /* Save native input into data->earlier before processing */
    if (data->earlier_size < needed) {
        unsigned char *grown = (unsigned char *)realloc(data->earlier, (size_t)needed);
        if (!grown) {
            nr_log("[nr_layer] earlier-buffer alloc failed; raw out (frame %lu)",
                   frame_counter);
            diag_path = "ealloc";
            return -1;
        }
        data->earlier = grown;
        data->earlier_size = needed;
    }
    memcpy(data->earlier, data->mapped, (size_t)needed);
    data->have_earlier = 1;
    data->earlier_size = needed;
    data->earlier_pending = 1;   /* no matching result yet - if the exchange
                                  * below fails, next frame must not reuse
                                  * `result` as this input's output */
    data->reuse_run = 0;         /* fresh input accepted for processing - the
                                  * staleness counter restarts here */

    uint32_t header[4] = { 0x304E524Eu, chain->extent.width,
                           chain->extent.height, (uint32_t)chain->format };
    if (capture_path) {
        FILE *file = fopen(capture_path, "wb");
        if (file) {
            fwrite(header, sizeof header, 1, file);
            fwrite(data->mapped, 1, (size_t)needed, file);
            fclose(file);
        }
    }

    /* In-process DLSS 5 engine execution: bypass TCP socket entirely */
    ensure_inprocess();
    if (g_inprocess_active && pfn_dlss5_process_frame && g_engine) {
        int r = pfn_dlss5_process_frame(g_engine, chain->extent.width, chain->extent.height,
                                        data->mapped, data->mapped, (long)frame_counter);
        if (r == 0) {
            /* The in-process engine is a processed-result producer too:
             * feeding the flap clock here keeps the hold path eligible if a
             * later in-process failure drops to the TCP fallback. */
            nr_degrade_note_success(&g_dg, nr_now_ms());
            if (data->result && data->result_size >= needed) {
                memcpy(data->result, data->mapped, (size_t)needed);
                data->result_w = chain->extent.width;
                data->result_h = chain->extent.height;
                data->result_valid = 1;
                data->holding_chain = chain->swapchain;
                data->result_fmt = (uint32_t)chain->format;
                data->result_src_crc = data->last_in_crc;
                data->earlier_pending = 0;
            }
            dump_capture(capture_out_path, header, (const unsigned char *)data->mapped, needed);
            nr_log("[nr_layer] in-process processed %ux%u (frame %lu)",
                   chain->extent.width, chain->extent.height, frame_counter);
            diag_path = "inproc";
            return 0;
        }
        nr_log("[nr_layer] in-process process_frame returned %d; trying TCP fallback", r);
    }

    /* No daemon configured: capture-only mode (frame passes through). */
    if (!nr_port) { diag_path = "noport"; return -1; }
    /* Probe pacing: while a flap/outage is in progress only every Nth
     * present touches the socket; the rest are served from the held result
     * (or natively when the held extent does not match) without paying the
     * 1.5 s timeout per frame and without piling TCP backlog on a daemon
     * that is already saturated. */
    if (!nr_degrade_probe_now(&g_dg, nr_now_ms())) {
        if (data->result_valid && data->result
            && data->result_size >= needed
            && data->result_w == chain->extent.width
            && data->result_h == chain->extent.height
            && data->holding_chain == chain->swapchain) {
            memcpy(data->mapped, data->result, (size_t)needed);
            nr_degrade_note_held(&g_dg);
            diag_path = "pheld";
            return 0;
        }
        nr_log("[nr_layer] probe paced and no held result; raw out (frame %lu)",
               frame_counter);
        diag_path = "praw";
        return -1;
    }
    /* Async submit (default, NR_LAYER_ASYNC=1): enqueue the frame copy and
     * display the newest reply already held - the ~60 ms daemon round-trip
     * runs on the worker, never inside the game's present. earlier_pending
     * is a sync-only notion: the async reuse gate treats `earlier` as the
     * last SUBMITTED input and result as the newest COMPLETED output, so a
     * byte-identical present simply serves the freshest processed frame. */
    if (g_async) {
        async_lazy_start(data);
        if (data->async) {
            uint32_t ahdr[4] = { 0x304E524Eu, chain->extent.width,
                                 chain->extent.height,
                                 (uint32_t)chain->format };
            data->earlier_pending = 0;
            /* Submit pacing (fix C): the bounded ring used to absorb an
             * every-present submit flood by evicting the oldest - most
             * frames died in queue and the daemon processed a stale
             * backlog. With the gate on, a submit lands only while the
             * pending ring is empty (SUBMIT_IDLE=1) or the worker is
             * fully idle (=2): the request the daemon runs is always the
             * freshest captured frame and its reply stays <= ~2 submits
             * old. SUBMIT_EVERY=N adds a hard decimation on top for
             * rapid-change content. Skipped presents still capture,
             * serve and present normally - only the queue stops growing. */
            {
                int may = 1;
                data->submit_tick++;
                if (g_submit_every > 1
                    && (data->submit_tick % (unsigned long)g_submit_every))
                    may = 0;
                if (may && g_submit_idle) {
                    if (nr_async_pending(data->async)) may = 0;
                    if (may && g_submit_idle > 1
                        && nr_async_busy(data->async)) may = 0;
                }
                if (!may) {
                    data->submit_skipped++;
                } else {
                    unsigned long seq = 0;
                    if (nr_async_submit(data->async, NR_ASYNC_PROC,
                                        (uint64_t)chain->swapchain, ahdr,
                                        (const unsigned char *)data->mapped,
                                        (size_t)needed, &seq) != 0) {
                        /* Mailbox would not take the frame (pool
                         * exhaustion is nearly impossible): the newest
                         * pending submission still covers fresher
                         * content than the last reply - serve. */
                        nr_log("[nr_layer] async submit dropped "
                               "(frame %lu)", frame_counter);
                    } else {
                        data->submit_seq = seq;
                    }
                }
            }
            /* have_raw=1: mapped holds THIS present's captured frame.
             * queue+index are passed through for the GPU compose path,
             * which may return 2 = "image already written". */
            return async_serve_best(data, chain, queue, index, needed, 1);
        }
        /* worker failed to start once: sync exchange fallback below */
    }
    /* The reply lands in data->mapped (the payload is fully sent before the
     * first reply byte arrives, so the alias is safe). Receiving anywhere
     * else - into data->result as the old code did - lets a timed-out recv
     * tear the last processed frame we may still want to hold. */
    int erc = exchange_keepalive(header, sizeof header, data->mapped,
                                 (size_t)needed, data->mapped,
                                 (size_t)needed);
    if (erc) {
        int hold = 0;
        int dlog = nr_degrade_note_fail(&g_dg,
            erc == -2 ? NR_DEGRADE_FAIL_CONNECT : NR_DEGRADE_FAIL_TRANSPORT,
            nr_now_ms(), &hold);
        /* Flap control: a fresh outage on a known-up daemon re-blits the
         * last processed result of THIS extent instead of strobing a raw
         * frame between processed neighbours. */
        if (hold && data->result_valid && data->result
            && data->result_size >= needed
            && data->result_w == chain->extent.width
            && data->result_h == chain->extent.height
            && data->holding_chain == chain->swapchain) {
            memcpy(data->mapped, data->result, (size_t)needed);
            nr_degrade_note_held(&g_dg);
            if (dlog == NR_DEGRADE_LOG_HICCUP)
                nr_log("[nr_layer] daemon hiccup; holding last processed "
                       "frame (fail %u)", g_dg.consecutive_fail);
            diag_path = "hold";
            return 0;
        }
        if (dlog == NR_DEGRADE_LOG_DOWN)
            nr_log("[nr_layer] daemon considered down; native passthrough");
        nr_log("[nr_layer] the daemon did not answer; frame unchanged");
        diag_path = "exfail";
        return -1;
    }
    if (nr_degrade_note_success(&g_dg, nr_now_ms())
            == NR_DEGRADE_LOG_RECOVERY) {
        nr_log("[nr_layer] daemon answered again; processed output resumed");
        /* The daemon demonstrably went down and came back (restart or a
         * drained flap): a fresh peer gets a fresh framegen probe - the
         * next generated present re-tries NRN2/NRN3 immediately instead
         * of sitting out the remaining backoff. */
        if (g_dg.fg_fail) {
            nr_degrade_fg_note_success(&g_dg);
            nr_log("[nr_layer] daemon recovered; framegen re-probed on "
                   "next generated present");
        }
    }
    memcpy(data->result, data->mapped, (size_t)needed);
    data->result_w = chain->extent.width;
    data->result_h = chain->extent.height;
    data->result_valid = 1;
    data->holding_chain = chain->swapchain;
    data->result_fmt = (uint32_t)chain->format;
    data->result_src_crc = data->last_in_crc;
    data->earlier_pending = 0;
    dump_capture(capture_out_path, header, data->result, needed);
    nr_log("[nr_layer] processed %ux%u", chain->extent.width, chain->extent.height);
    diag_path = "tcp";
    return 0;
}

/* The present, with its waits replaced by ours if a transfer consumed them. */
static VkResult present_now(device_data *data, VkQueue queue,
                            const VkPresentInfoKHR *info)
{
    if (!data || !data->present) return VK_ERROR_INITIALIZATION_FAILED;
    if (!sync_semaphores || !data->present_signal)
        return data->present(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &data->present_signal;
    return data->present(queue, &patched);
}

/* ------------------------------------------------ freeze + live reprocess --
 * m13 overlay "photo mode": while NR_LAYER_FREEZE exists the layer holds the
 * RAW frame captured at freeze time and re-blits its processed result every
 * present (the game keeps running underneath; the picture stands still).
 * When the manager pushes a new gain to the daemon it bumps NR_LAYER_REPROC's
 * mtime; the next present re-sends the SAME raw frame, so the knob turns are
 * visible on the frozen frame, processed from the untouched original. */
static int reproc_changed(device_data *data)
{
    init_freeze_paths();
    if (!reproc_path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(reproc_path, GetFileExInfoStandard, &fa)) {
        data->reproc_seen = 0;
        return 0;
    }
    if (!data->reproc_seen
        || fa.ftLastWriteTime.dwLowDateTime != data->reproc_stamp.dwLowDateTime
        || fa.ftLastWriteTime.dwHighDateTime != data->reproc_stamp.dwHighDateTime) {
        data->reproc_stamp = fa.ftLastWriteTime;
        data->reproc_seen = 1;
        return 1;
    }
    return 0;
}

/* Send the held raw frame to the daemon; reply lands in data->result via a
 * scratch buffer. The HELD extent (raw_w/raw_h/raw_size) is authoritative -
 * the caller only reaches us for the latched swapchain, but keying on the
 * captured extent instead of the presented chain's keeps a mid-freeze
 * swapchain surprise from ever resizing the wrong buffer.
 *
 * TCP integrity: the reply is received into data->mapped (the staging
 * buffer; the caller overwrites it with the result right after) or a heap
 * scratch when staging is too small, and is memcpy'd into data->result ONLY
 * on a complete exchange. A timed-out/partial recv therefore can never tear
 * the last good result the frozen loop keeps re-blitting - the same
 * discipline process_frame uses since Task 26. */
static int resend_raw(device_data *data, swapchain_data *chain)
{
    ensure_inprocess();
    /* Forced poll: a reprocess must run with the just-written knob values,
     * not the values from up to 250 ms ago. */
    update_live_knobs(1);
    VkDeviceSize needed = data->raw_size;
    if (!data->raw || !needed || !data->raw_w || !data->raw_h) return -1;
    if (!data->result || data->result_size < needed) {
        /* A pool-published result cannot be realloc'd in place - claim()
         * hands it back to the async pool first; the fresh slot below is
         * caller-owned again (realloc(NULL) == malloc). */
        if (data->async && nr_async_claim(data->async, data->result)) {
            data->result = NULL;
            data->result_size = 0;
        }
        unsigned char *grown = (unsigned char *)realloc(data->result, (size_t)needed);
        if (!grown) {
            if (!data->result) data->result_valid = 0;
            return -1;
        }
        data->result = grown;
        data->result_size = needed;
    }
    /* Knob-turned reprocess: drop the temporal anchor so the new parameter
     * values apply unblended. Without this the net's history pull converges
     * over several reprocesses instead of answering the knob turn now. */
    if (g_inprocess_active && pfn_dlss5_reset_history && g_engine)
        pfn_dlss5_reset_history(g_engine);
    else
        nr_ctrl(12, 0);

    if (g_inprocess_active && pfn_dlss5_process_frame && g_engine) {
        int r = pfn_dlss5_process_frame(g_engine, data->raw_w, data->raw_h,
                                        data->raw, data->result, (long)frame_counter);
        if (r == 0) {
            data->result_w = data->raw_w;
            data->result_h = data->raw_h;
            data->result_valid = 1;
            data->holding_chain = chain->swapchain;
            data->result_fmt = (uint32_t)chain->format;
            data->result_src_crc = 0;   /* answers data->raw, not live input */
            /* result now holds P(raw_frozen), not P(earlier): the
             * earlier->result mapping is broken, so a post-unfreeze frame
             * byte-identical to the stale earlier must not replay this. */
            data->have_earlier = 0;
            nr_log("[nr_layer] in-process reprocessed held raw frame (live knob turn)");
            return 0;
        }
    }

    uint32_t header[4] = { 0x304E524Eu, data->raw_w, data->raw_h,
                           (uint32_t)chain->format };
    unsigned char *reply;
    int reply_is_staging = data->mapped && data->staging_size >= needed;
    if (reply_is_staging)
        reply = (unsigned char *)data->mapped;
    else
        reply = (unsigned char *)malloc((size_t)needed);
    if (!reply) return -1;
    int erc = exchange(header, sizeof header, data->raw, (size_t)needed,
                       reply, (size_t)needed);
    if (erc) {
        if (!reply_is_staging) free(reply);
        nr_degrade_note_fail(&g_dg,
            erc == -2 ? NR_DEGRADE_FAIL_CONNECT : NR_DEGRADE_FAIL_TRANSPORT,
            nr_now_ms(), NULL);
        nr_log("[nr_layer] reprocess: the daemon did not answer "
               "(previous result kept)");
        return -1;
    }
    nr_degrade_note_success(&g_dg, nr_now_ms());
    memcpy(data->result, reply, (size_t)needed);
    if (!reply_is_staging) free(reply);
    data->result_w = data->raw_w;
    data->result_h = data->raw_h;
    data->result_valid = 1;
    data->holding_chain = chain->swapchain;
    data->result_fmt = (uint32_t)chain->format;
    data->result_src_crc = 0;           /* answers data->raw, not live input */
    data->have_earlier = 0;  /* result is P(raw), not P(earlier) - see above */
    nr_log("[nr_layer] reprocessed the held frame (live knob turn)");
    return 0;
}

/* Hold the frame being presented right now as the frozen raw frame, process
 * it once (so the frozen picture starts processed at the current gain). */
static int freeze_capture(device_data *data, swapchain_data *chain,
                          VkQueue queue, uint32_t index)
{
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width
                        * chain->extent.height * 4;
    if (ensure_resources(data, needed)) return -1;
    if (transfer(data, chain, queue, index, 0)) return -1;
    if (data->raw_size < needed) {
        unsigned char *grown = (unsigned char *)realloc(data->raw,
                                                        (size_t)needed);
        if (!grown) return -1;
        data->raw = grown;
        data->raw_size = needed;
    }
    memcpy(data->raw, data->mapped, (size_t)needed);
    data->raw_size = needed;
    data->raw_held = 1;
    /* SINGLE-LATCH: this swapchain owns the frozen frame until unfreeze or
     * swapchain destroy; no other presented chain may overwrite raw. */
    data->raw_chain = chain->swapchain;
    data->raw_w = chain->extent.width;
    data->raw_h = chain->extent.height;
    if (resend_raw(data, chain)) {
        /* resend_raw returning -1 may mean the result realloc itself failed:
         * only mirror raw->result when the slot is actually big enough. */
        if (data->result && data->result_size >= needed) {
            memcpy(data->result, data->raw, (size_t)needed);
            data->result_w = chain->extent.width;
            data->result_h = chain->extent.height;
            data->result_valid = 1;
            data->holding_chain = chain->swapchain;
            data->result_fmt = (uint32_t)chain->format;
            data->result_src_crc = 0;   /* result is the raw frame itself */
            data->have_earlier = 0;  /* result is raw, not P(earlier) */
        }
        nr_log("[nr_layer] daemon unavailable; freezing the original frame unprocessed");
    }
    /* data->mapped still holds the raw capture when no result exists - the
     * frozen picture degrades to unprocessed rather than reading a short
     * or NULL result buffer. */
    if (data->result_valid && data->result && data->result_size >= needed)
        memcpy(data->mapped, data->result, (size_t)needed);
    nr_log("[nr_layer] frame frozen (%ux%u); knobs reprocess the original",
           chain->extent.width, chain->extent.height);
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_QueuePresentKHR(VkQueue queue,
                                                  const VkPresentInfoKHR *info)
{
    frame_counter++;
    device_data *data = NULL;
    queue_data *owner = find_queue(queue);
    if (owner && !owner->capture_ok) {
        if (!owner->native_logged) {
            owner->native_logged = 1;
            nr_log("[nr_layer] queue %p cannot capture; presenting native",
                   (void *)queue);
        }
        device_data *host = find_device(owner->device);
        if (host && host->present) return host->present(queue, info);
    }
    if (owner) {
        data = find_device(owner->device);
        if (data && data->queue_family != owner->family) {
            data->queue_family = owner->family;
            if (data->pool) {
                PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)
                    data->get_device_proc(data->device, "vkDestroyCommandPool");
                if (destroy) destroy(data->device, data->pool, NULL);
                data->pool = VK_NULL_HANDLE;
            }
        }
    }
    if (!data) {
        /* A present on a queue the layer never saw via vkGetDeviceQueue*:
         * falling back to devices[0] would run transfers on a queue whose
         * family may not support them (present-only / protected). Present
         * natively through any live device's hook instead. */
        for (int i = 0; i < 8; i++) {
            if (devices[i].device && devices[i].present) {
                static int untracked_logged;
                if (!untracked_logged) {
                    untracked_logged = 1;
                    nr_log("[nr_layer] present on untracked queue %p; "
                           "passing through native", (void *)queue);
                }
                return devices[i].present(queue, info);
            }
        }
    }
    if (!data || !data->present) return VK_ERROR_INITIALIZATION_FAILED;

    data->present_plain = info->waitSemaphoreCount > NR_MAX_WAITS;
    data->present_wait = info->pWaitSemaphores;
    data->present_wait_count = (sync_semaphores && !data->present_plain)
        ? info->waitSemaphoreCount : 0;
    data->present_signal = VK_NULL_HANDLE;

    /* Hotkeys / status checks:
     * - g_off: layer disabled via CTRL+ALT+Q -> present native untouched.
     * - g_paused: toggled by hotkey CTRL+ALT+X -> present native untouched.
     * - no_patch: swapchains left completely alone -> present native untouched.
     */
    if (g_off)
        return data->present(queue, info);
    if (g_paused)
        return data->present(queue, info);
    if (no_patch)
        return data->present(queue, info);
    /* Manager pause: processing runs only while the trigger file exists.
     * m13's dx9_pause()/resume() toggles it; _access on an existing path is
     * a microseconds-class stat, same cadence as the freeze check below. */
    if (trigger_path && _access(trigger_path, 0) != 0)
        return data->present(queue, info);

    hotkeys_ensure();
    ensure_inprocess();
    update_live_knobs(0);
    /* Knob invalidation: a pushed value changes what the SAME input would
     * produce, so the cached earlier->result mapping is stale. Drop the
     * compare snapshot (same idiom ensure_resources uses on extent grow):
     * the next present reprocesses at the new values instead of replaying
     * the old-strength result onto a byte-identical frame - which read as
     * "the knob is dead until content moves" on static screens. The held
     * result stays valid; it is still the emergency fallback frame. */
    if (g_knobs_changed) {
        g_knobs_changed = 0;
        for (int i = 0; i < 8; i++) {
            devices[i].have_earlier = 0;
            devices[i].reuse_run = 0;
            /* A knob turn changes what the SAME input produces - the held
             * residual pair encodes the old values, drop it (the next
             * reply re-arms at the new strength in ~one round-trip). */
            if (devices[i].resid)
                nr_residual_invalidate(devices[i].resid);
            if (devices[i].gpuwarp)
                nr_gpuwarp_invalidate(devices[i].gpuwarp);
        }
    }
    init_freeze_paths();

    int frozen = freeze_path && _access(freeze_path, 0) == 0;
    static int frozen_logged;
    if (frozen && !frozen_logged) {
        frozen_logged = 1;
        nr_log("[nr_layer] freeze ENGAGED (frame %lu)", frame_counter);
    } else if (!frozen && frozen_logged) {
        frozen_logged = 0;
        nr_log("[nr_layer] freeze released (frame %lu)", frame_counter);
    }
    if (!frozen) {
        data->raw_held = 0;
        data->raw_chain = VK_NULL_HANDLE;
        data->reproc_seen = 0;
    }

    /* SINGLE-LATCH freeze: the held raw frame belongs to exactly ONE
     * primary swapchain. Freeze targets only the largest NON-DEGENERATE
     * chain among those presented; every other swapchain in the same pass
     * goes out native and can never overwrite data->raw/result. Without the
     * latch a multi-swapchain game (GTA IV presents a 160x28 menu strip next
     * to the 800x600 frame) overwrote the held frame with whatever presented
     * at a different extent first. Once latched the latch is sticky: a
     * present that does not carry the frozen chain passes fully native. */
    swapchain_data *freeze_primary = NULL;
    if (frozen) {
        if (data->raw_held && !find_swapchain(data->raw_chain)) {
            /* Latched chain vanished without us seeing the destroy: drop the
             * held frame so this present re-captures the new primary. */
            data->raw_held = 0;
            data->raw_chain = VK_NULL_HANDLE;
            data->result_valid = 0;
        }
        VkDeviceSize best = 0;
        for (uint32_t j = 0; j < info->swapchainCount; j++) {
            swapchain_data *c = find_swapchain(info->pSwapchains[j]);
            if (!c || info->pImageIndices[j] >= c->image_count) continue;
            int deg_log = NR_DEGRADE_LOG_NONE;
            if (nr_degrade_is_degenerate(&g_dg, c->extent.width,
                                         c->extent.height, &deg_log)) {
                if (deg_log == NR_DEGRADE_LOG_DEGENERATE)
                    nr_log("[nr_layer] degenerate swapchain %ux%u; passing "
                           "through", c->extent.width, c->extent.height);
                continue;
            }
            if (data->raw_held) {
                if (c->swapchain == data->raw_chain) {
                    freeze_primary = c;
                    break;
                }
            } else {
                VkDeviceSize sz = (VkDeviceSize)c->extent.width
                                * c->extent.height * 4;
                if (sz > best) { best = sz; freeze_primary = c; }
            }
        }
    }

    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
        if (!chain || info->pImageIndices[i] >= chain->image_count) {
            nr_log("[nr_layer] UNKNOWN swapchain %p idx %u skipped; raw out "
                   "(frame %lu)", (void *)info->pSwapchains[i],
                   info->pImageIndices[i], frame_counter);
            continue;
        }
        uint32_t index = info->pImageIndices[i];
        VkDeviceSize want = (VkDeviceSize)chain->extent.width
                          * chain->extent.height * 4;

        if (frozen) {
            /* Photo mode: hold the frozen frame; reprocess on demand.
             * Only the latched primary chain is served; non-primary chains
             * (degenerate strips or secondary surfaces) present native. */
            if (chain != freeze_primary) continue;
            if (!data->raw_held) {
                if (freeze_capture(data, chain, queue, index) == 0)
                    transfer(data, chain, queue, index, 1);
            } else {
                if (reproc_changed(data)) {
                    update_live_knobs(1);
                    if (resend_raw(data, chain) != 0)
                        nr_log("[nr_layer] reprocess failed; keeping the "
                               "previous result");
                }
                if (data->result_valid && data->result_size >= want
                    && data->holding_chain == chain->swapchain
                    && data->mapped && data->staging_size >= want) {
                    memcpy(data->mapped, data->result,
                           (size_t)want);
                    transfer(data, chain, queue, index, 1);
                }
            }
            continue;
        }

        /* Live mode processing:
         * When In-Process DLSS 5 is active (g_inprocess_active) or DLSS 5 is enabled,
         * EVERY presented frame is processed by default.
         * With frame generation on (1:2 cadence) every 2nd present is
         * processed; the presents in between are generated by the engine's
         * residual transfer (dlss5_framegen_transfer), which warps the last
         * processed frame's detail along the raw->raw motion. When FG is off
         * or unavailable the unprocessed presents pass through native.
         */
        {
        /* Publish any completed daemon replies BEFORE the serve decisions
         * below - result/fgi slots refresh on every present, including
         * reuse, mid and cadence-skipped ones (the worker reports through
         * the done mailbox; nr_degrade streaks apply here on the present
         * thread, keeping all flap policy single-threaded). */
        if (data->async)
            async_drain(data);
        int fg_enabled = is_framegen_enabled();
        int fg_mode = fg_enabled ? framegen_mode() : 1;
        /* Two FG producers: the in-process x64 engine answers
         * dlss5_framegen_transfer (mode 1) / dlss5_framegen_interpolate
         * (mode 2) directly; the x86 daemon answers the NRN2/NRN3
         * frame-magic requests (the resident engine owns the anchors).
         * Daemon requests only flow once the daemon has proven itself on the
         * socket (daemon_known_up) - a daemon that is down keeps FG silent
         * instead of paying a refused connect every 2nd present; a failed
         * NRN2/NRN3 backs the probe off exponentially (nr_degrade fg_skip)
         * instead of latching off - on the wire a refusal is a bare
         * connection drop, indistinguishable from a busy daemon, so nothing
         * justifies giving up for the session. */
        int fg_inprocess = g_inprocess_active && g_engine
                           && (fg_mode == 2
                               ? (pfn_dlss5_framegen_interpolate != NULL)
                               : (pfn_dlss5_framegen_transfer != NULL));
        int fg_daemon = !fg_inprocess && nr_port
                        && g_dg.daemon_known_up;
        int fg_avail = fg_inprocess || fg_daemon;
        int should_process = nr_fg_should_process(&g_fg, frame_counter,
                                                  fg_enabled, fg_avail);
        diag_path = "?";
        int diag_tr = -1;
        if (should_process) {
            int prc = process_frame(data, chain, queue, index);
            /* prc 0 = mapped holds the frame to blit; prc 2 = the GPU
             * compose already wrote the swapchain image (no write-back,
             * no mode-2 overwrite - the generated midpoint path only
             * makes sense when mapped holds the anchor). */
            int ok = (prc == 0 || prc == 2);
            if (!ok) {
                /* Splice guard: a frame that failed processing must never go
                 * out raw between processed neighbours - serve the last
                 * processed result of this extent instead (same policy as the
                 * daemon-flap hold). Only a matching-extent result qualifies;
                 * degenerate/other-extent chains still pass through native.
                 * prc==-2 is the async POLICY raw: the serve-best already
                 * weighed this exact held result and rejected it (stale on
                 * moving content) - the guard must not re-serve it. */
                if (prc != -2 && data->result_valid && data->result
                    && data->result_size >= want
                    && data->result_w == chain->extent.width
                    && data->result_h == chain->extent.height
                    && data->holding_chain == chain->swapchain
                    && data->mapped && data->staging_size >= want) {
                    memcpy(data->mapped, data->result, (size_t)want);
                    nr_log("[nr_layer] process failed; held last processed "
                           "frame (frame %lu)", frame_counter);
                    ok = 1;
                    diag_path = "sheld";
                } else {
                    if (prc != -2) {
                        nr_log("[nr_layer] process failed and no held result; "
                               "raw out (frame %lu)", frame_counter);
                        diag_path = "raw";
                    }
                    /* prc == -2: async policy chose raw; the araw/astale
                     * serve tag stays for the diag line. */
                }
            }
            if (ok) {
                nr_fg_note(&g_fg, NR_FG_PROCESSED);
                data->holding = 1;
                data->holding_chain = chain->swapchain;
                if (fg_mode == 2 && fg_avail && prc != 2) {
                    /* Mode-2 anchor present: data->mapped currently holds P
                     * (the newest processed anchor). Overwrite it with the
                     * generated midpoint of the just-closed anchor interval
                     * {Pprev, Pcur} around the REAL middle frame captured at
                     * the previous mid present - every displayed frame then
                     * sits at a uniform +1-present delay. On any failure P
                     * stays in mapped and goes out on time. */
                    int gen_ok = 0;
                    if (data->mid_valid && data->mid
                        && data->mid_size >= want
                        && data->mid_w == chain->extent.width
                        && data->mid_h == chain->extent.height) {
                        if (fg_inprocess) {
                            if (data->interp_size < want) {
                                unsigned char *grown = (unsigned char *)realloc(data->interp, (size_t)want);
                                if (grown) { data->interp = grown; data->interp_size = want; }
                            }
                            if (data->interp && data->interp_size >= want) {
                                int r = pfn_dlss5_framegen_interpolate(
                                    g_engine, chain->extent.width,
                                    chain->extent.height,
                                    data->mid, data->interp, 0.5f);
                                if (r == 0) {
                                    memcpy(data->mapped, data->interp,
                                           (size_t)want);
                                    gen_ok = 1;
                                }
                            }
                        } else if (nr_degrade_fg_probe_now(&g_dg)) {
                            /* x86 daemon NRN3: the mid frame payload -> the
                             * resident engine replies the interpolated frame
                             * (or a full process fallback - either way a
                             * presentable frame).
                             * ASYNC: queue the mid behind its anchor PROC
                             * (same FIFO, ordered); the reply lands in the
                             * fgi slot and displays one-shot at a later
                             * present - this one keeps whatever serve-best
                             * already put in mapped. SYNC: the reply lands
                             * in interp so a dead daemon cannot tear the P
                             * frame still sitting in mapped as the fallback.
                             * A probe skipped by the backoff leaves P in
                             * mapped the same way - the anchor goes out on
                             * time. */
                            uint32_t fgi_hdr[4] = { 0x334E524Eu,
                                chain->extent.width, chain->extent.height,
                                (uint32_t)chain->format };
                            if (data->async) {
                                if (nr_async_submit(data->async, NR_ASYNC_FGI,
                                        (uint64_t)chain->swapchain, fgi_hdr,
                                        data->mid, (size_t)want, NULL))
                                    nr_log("[nr_layer] async NRN3 dropped "
                                           "(frame %lu)", frame_counter);
                            } else {
                            if (data->interp_size < want) {
                                unsigned char *grown = (unsigned char *)realloc(data->interp, (size_t)want);
                                if (grown) { data->interp = grown; data->interp_size = want; }
                            }
                            if (data->interp && data->interp_size >= want) {
                                int erc = exchange(fgi_hdr, sizeof fgi_hdr,
                                                   data->mid, (size_t)want,
                                                   data->interp, (size_t)want);
                                if (erc == 0) {
                                    nr_degrade_fg_note_success(&g_dg);
                                    nr_degrade_note_success(&g_dg, nr_now_ms());
                                    memcpy(data->mapped, data->interp,
                                           (size_t)want);
                                    gen_ok = 1;
                                } else {
                                    /* Transient by definition (busy daemon,
                                     * engine rebuild, torn socket - a real
                                     * refusal is just a connection drop):
                                     * back the probe off and keep the held
                                     * anchor; the next FG slot re-tries. */
                                    nr_degrade_fg_note_fail(&g_dg);
                                    nr_log("[nr_layer] daemon NRN3 failed "
                                           "(rc %d); held anchor out, FG "
                                           "probe streak %u, next in %u "
                                           "slots", erc, g_dg.fg_fail,
                                           g_dg.fg_skip);
                                }
                            }
                            }
                        }
                        /* Consume the mid: it described THIS anchor interval
                         * only. If the next mid present fails to capture
                         * (deg extent, transfer fault), a stale mid must not
                         * be re-sent as the next interval's midpoint - the
                         * held anchor P is the safer splice. */
                        data->mid_valid = 0;
                    }
                    if (gen_ok) {
                        static unsigned long fgi_count = 0;
                        fgi_count++;
                        nr_fg_note(&g_fg, NR_FG_GENERATED);
                        if (fgi_count <= 5 || fgi_count % 60 == 0) {
                            nr_log("[nr_layer] framegen interpolated %ux%u "
                                   "(frame %lu, fgi #%lu)",
                                   chain->extent.width, chain->extent.height,
                                   frame_counter, fgi_count);
                        }
                        diag_path = "fgi";
                    }
                }
                /* prc==2: the GPU compose already wrote the image. */
                if (prc == 0) {
                    diag_tr = transfer(data, chain, queue, index, 1);
                    if (diag_tr)
                        nr_log("[nr_layer] write-back failed; raw out "
                               "(frame %lu)", frame_counter);
                }
            }
        } else if (fg_enabled && fg_avail && fg_mode == 2) {
            /* Mode-2 mid present: capture the REAL middle frame for the next
             * anchor interval and re-blit the held processed anchor - no
             * engine call, no daemon round-trip at this slot. */
            int held = 0;
            int deg_log = NR_DEGRADE_LOG_NONE;
            int deg = nr_degrade_is_degenerate(&g_dg, chain->extent.width,
                                               chain->extent.height, &deg_log);
            if (!deg
                && ensure_resources(data, want) == 0
                && transfer(data, chain, queue, index, 0) == 0) {
                if (data->mid_size < want) {
                    unsigned char *grown = (unsigned char *)realloc(data->mid, (size_t)want);
                    if (grown) { data->mid = grown; data->mid_size = want; }
                }
                if (data->mid && data->mid_size >= want) {
                    memcpy(data->mid, data->mapped, (size_t)want);
                    data->mid_valid = 1;
                    data->mid_w = chain->extent.width;
                    data->mid_h = chain->extent.height;
                }
                if (data->async) {
                    /* serve-best: a pending fgi midpoint (the previous
                     * interval's generated frame, landing right at its
                     * natural cadence slot), else the freshest processed
                     * anchor, else the raw mid just captured. have_raw=1:
                     * the mid was captured into data->mid above and
                     * mapped still holds it. 2 = the GPU compose already
                     * wrote the swapchain image - nothing to transfer. */
                    int sb = async_serve_best(data, chain, queue, index,
                                              want, 1);
                    if (sb == 0) {
                        diag_tr = transfer(data, chain, queue, index, 1);
                        held = (diag_tr == 0);
                    } else if (sb == 2) {
                        held = 1;
                    }
                } else if (data->result_valid && data->result
                    && data->result_size >= want
                    && data->result_w == chain->extent.width
                    && data->result_h == chain->extent.height
                    && data->holding_chain == chain->swapchain) {
                    memcpy(data->mapped, data->result, (size_t)want);
                    diag_tr = transfer(data, chain, queue, index, 1);
                    held = (diag_tr == 0);
                }
            }
            nr_fg_note(&g_fg, held
                       ? ((data->async && !strcmp(diag_path, "afgi"))
                          ? NR_FG_GENERATED : NR_FG_PROCESSED)
                       : NR_FG_NATIVE);
            if (!held || !data->async)
                diag_path = held ? "mheld" : "mnat";
        } else if (fg_enabled && fg_avail) {
            /* Framegen: generate this present from the engine's copy of the
             * last processed frame + its raw input (residual transfer).
             * x64: dlss5_framegen_transfer in-process. x86 daemon path: the
             * NRN2 frame magic asks the daemon's resident engine for the
             * same transfer - it falls back to a full process when it has no
             * usable prev pair, so any complete reply is a presentable
             * frame (generated or not). */
            int generated = 0;
            int fg_out = -1;   /* async: explicit NR_FG_* for shown frame */
            int deg_log = NR_DEGRADE_LOG_NONE;
            /* Same gate as process_frame: a menu/letterbox strip must not
             * bounce the daemon's (or in-process) engine through a
             * junk-extent re-init just to synthesize its frame. */
            int deg = nr_degrade_is_degenerate(&g_dg, chain->extent.width,
                                               chain->extent.height, &deg_log);
            if (deg && deg_log == NR_DEGRADE_LOG_DEGENERATE)
                nr_log("[nr_layer] degenerate swapchain %ux%u; FG "
                       "passthrough", chain->extent.width,
                       chain->extent.height);
            if (!deg
                && ensure_resources(data, want) == 0
                && transfer(data, chain, queue, index, 0) == 0) {
                if (fg_inprocess) {
                    if (data->raw_size < want) {
                        unsigned char *grown = (unsigned char *)realloc(data->raw, (size_t)want);
                        if (grown) {
                            data->raw = grown;
                            data->raw_size = want;
                        }
                    }
                    if (data->interp_size < want) {
                        unsigned char *grown = (unsigned char *)realloc(data->interp, (size_t)want);
                        if (grown) {
                            data->interp = grown;
                            data->interp_size = want;
                        }
                    }
                    if (data->raw && data->raw_size >= want && data->interp && data->interp_size >= want) {
                        memcpy(data->raw, data->mapped, (size_t)want);
                        data->raw_size = want;
                        int r = pfn_dlss5_framegen_transfer(g_engine, chain->extent.width, chain->extent.height,
                                                            data->raw, data->interp);
                        if (r == 0) {
                            static unsigned long fg_frame_count = 0;
                            fg_frame_count++;
                            if (fg_frame_count <= 5 || fg_frame_count % 60 == 0) {
                                nr_log("[nr_layer] framegen generated %ux%u (frame %lu, fg #%lu)",
                                       chain->extent.width, chain->extent.height, frame_counter, fg_frame_count);
                            }
                            memcpy(data->mapped, data->interp, (size_t)want);
                            diag_tr = transfer(data, chain, queue, index, 1);
                            generated = 1;
                        } else if (g_fg.native < 3) {
                            nr_log("[nr_layer] dlss5_framegen_transfer failed (%d); presenting native frame", r);
                        }
                    }
                } else if (nr_degrade_fg_probe_now(&g_dg)) {
                    /* Daemon path (x86): the reply lands in data->mapped -
                     * the payload is fully sent before the first reply byte
                     * arrives, so the aliasing is safe (same trick
                     * process_frame uses). data->raw/interp stay untouched:
                     * the engine's own buffers hold prev.
                     * ASYNC: the NRN2 request is queued; THIS present shows
                     * serve-best (a pending fgi reply, else the freshest
                     * processed result, else the raw mid we just captured).
                     * The generated frame displays when its reply lands. */
                    uint32_t fgheader[4] = { 0x324E524Eu, chain->extent.width,
                                             chain->extent.height,
                                             (uint32_t)chain->format };
                    if (data->async) {
                        if (nr_async_submit(data->async, NR_ASYNC_FGT,
                                (uint64_t)chain->swapchain, fgheader,
                                (const unsigned char *)data->mapped,
                                (size_t)want, NULL))
                            nr_log("[nr_layer] async NRN2 dropped "
                                   "(frame %lu)", frame_counter);
                        /* have_raw=1: mapped is the just-captured frame.
                         * 2 = GPU compose already wrote the image - count
                         * the enhanced frame as the processed bucket. */
                        int sb = async_serve_best(data, chain, queue,
                                                  index, want, 1);
                        if (sb == 0) {
                            diag_tr = transfer(data, chain, queue, index, 1);
                            if (diag_tr == 0) {
                                generated = 1;
                                /* serve tag (afgi/aserv) already in
                                 * diag_path; fg_out carries the note kind */
                                fg_out = !strcmp(diag_path, "afgi")
                                         ? NR_FG_GENERATED : NR_FG_PROCESSED;
                            }
                        } else if (sb == 2) {
                            generated = 1;
                            fg_out = NR_FG_PROCESSED;
                        } else {
                            fg_out = NR_FG_NATIVE;
                        }
                    } else {
                    int erc = exchange(fgheader, sizeof fgheader, data->mapped,
                                       (size_t)want, data->mapped, (size_t)want);
                    if (erc == 0) {
                        static unsigned long fg_tcp_count = 0;
                        nr_degrade_fg_note_success(&g_dg);
                        fg_tcp_count++;
                        if (fg_tcp_count <= 5 || fg_tcp_count % 60 == 0) {
                            nr_log("[nr_layer] daemon framegen %ux%u "
                                   "(frame %lu, fg #%lu)",
                                   chain->extent.width, chain->extent.height,
                                   frame_counter, fg_tcp_count);
                        }
                        nr_degrade_note_success(&g_dg, nr_now_ms());
                        diag_tr = transfer(data, chain, queue, index, 1);
                        if (diag_tr == 0)
                            generated = 1;
                    } else {
                        /* Transient (busy daemon, engine rebuild, torn
                         * socket - a real refusal is a bare connection
                         * drop, same on the wire): back the probe off
                         * instead of latching. The frame goes out native
                         * (staging is never written back on failure). */
                        nr_degrade_fg_note_fail(&g_dg);
                        nr_log("[nr_layer] daemon framegen failed "
                               "(rc %d); native out, FG probe streak %u, "
                               "next in %u slots", erc, g_dg.fg_fail,
                               g_dg.fg_skip);
                    }
                    }
                }
                /* Probe backoff window: this slot passes native without
                 * paying the doomed socket round-trip (same picture as a
                 * refused request). */
            }
            /* fg_out (async only): the serve tag already chose the exact
             * bucket for the displayed frame; sync keeps generated/native. */
            nr_fg_note(&g_fg, fg_out >= 0 ? fg_out
                       : (generated ? NR_FG_GENERATED : NR_FG_NATIVE));
            if (fg_out < 0)
                diag_path = generated ? "fgd" : "fgn";
        } else {
            /* Cadence-skipped present (NR_LAYER_LIVE=N>1): re-blit the held
             * processed result of this extent - the documented behaviour -
             * falling back to native only when nothing is held yet. A raw
             * splice between processed frames is a visible quality strobe. */
            int held = 0;
            if (data->async) {
                /* have_raw=0: this cadence-skipped present does NOT
                 * readback - mapped still holds the previous present's
                 * output, so serve-residual must not composite onto it
                 * (applying the delta to non-raw content double-adds). */
                int sb = async_serve_best(data, chain, queue, index,
                                          want, 0);
                if (sb == 0) {
                    diag_tr = transfer(data, chain, queue, index, 1);
                    held = (diag_tr == 0);
                } else if (sb == 2) {
                    held = 1;
                }
            } else if (data->result_valid && data->result && data->mapped
                && data->result_size >= want
                && data->result_w == chain->extent.width
                && data->result_h == chain->extent.height
                && data->holding_chain == chain->swapchain) {
                memcpy(data->mapped, data->result, (size_t)want);
                diag_tr = transfer(data, chain, queue, index, 1);
                held = (diag_tr == 0);
            }
            nr_fg_note(&g_fg, held
                       ? ((data->async && !strcmp(diag_path, "afgi"))
                          ? NR_FG_GENERATED : NR_FG_PROCESSED)
                       : NR_FG_NATIVE);
            if (!held || !data->async)
                diag_path = held ? "eheld" : "enat";
            if (fg_enabled && !fg_avail && !g_fg.unavailable_logged) {
                nr_log("[nr_layer] framegen requested but unavailable "
                       "(no in-process engine; daemon not up); presenting "
                       "native");
                g_fg.unavailable_logged = 1;
            }
        }
        if (nr_fg_stats_due(&g_fg)) {
            nr_log("[nr_layer] fg stats: presents=%lu processed=%lu "
                   "generated=%lu native=%lu",
                   g_fg.presents, g_fg.processed, g_fg.generated, g_fg.native);
            if (data->async)
                nr_log("[nr_layer] submit pacing: skipped=%lu queued=%lu "
                       "evicted=%lu reply_age=%lu",
                       data->submit_skipped,
                       nr_async_submitted(data->async),
                       nr_async_dropped(data->async), data->result_age);
        }
        if (diag_active()) {
            /* pres = residual coverage % (last apply); on a gres serve
             * the GPU counter trails one present; -1 = no pair */
            long pr = data->resid
                      ? nr_residual_last_cov_pm(data->resid) : -1;
            if (!strcmp(diag_path, "gres") && data->gpuwarp)
                pr = nr_gpuwarp_last_cov_pm(data->gpuwarp);
            nr_log("[diag] f=%lu sc=%p idx=%u p=%s up=%08x res=%08x "
                   "in=%08x src=%08x t=%llu tr=%d pres=%ld.%ld",
                   frame_counter, (void *)chain->swapchain, index, diag_path,
                   diag_crc(data->mapped, want),
                   diag_crc(data->result, data->result_size),
                   data->last_in_crc, data->result_src_crc,
                   (unsigned long long)nr_now_ms(), diag_tr,
                   pr < 0 ? -1 : pr / 10, pr < 0 ? 0 : pr % 10);
        }
        }
    }
    return present_now(data, queue, info);
}

#define INTERCEPT(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)nr_##name

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetDeviceProcAddr_(VkDevice device,
                                                               const char *pName)
{
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroyDevice);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    device_data *data = find_device(device);
    return data ? data->get_device_proc(device, pName) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetInstanceProcAddr_(VkInstance instance,
                                                                 const char *pName)
{
    INTERCEPT(CreateInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(CreateWin32SurfaceKHR);
    INTERCEPT(DestroyInstance);
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)nr_GetInstanceProcAddr_;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)nr_GetDeviceProcAddr_;
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroyDevice);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    return next_instance_proc ? next_instance_proc(instance, pName) : NULL;
}

/* Exported under the canonical Vulkan names so both the manifest "functions"
 * mapping and the loader's direct lookup find them. Compiled as C: the
 * header prototypes then carry C linkage and dllexport keeps clean names. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return nr_GetInstanceProcAddr_(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return nr_GetDeviceProcAddr_(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
    /* ("interface" is an MSVC keyword via windows.h - hence iface) */
    if (iface->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    iface->loaderLayerInterfaceVersion = 2;
    iface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        nr_lock_init();
    } else if (reason == DLL_PROCESS_DETACH) {
        /* reserved!=NULL means process termination: wndproc restoration is
         * pointless (windows die with the process) and DllMain is under the
         * loader lock, so only unhook on a real FreeLibrary. */
        if (!reserved) nr_unguard_all();
        if (g_engine && pfn_dlss5_destroy) {
            pfn_dlss5_destroy(g_engine);
            g_engine = NULL;
        }
        if (g_chain_dll) {
            FreeLibrary(g_chain_dll);
            g_chain_dll = NULL;
        }
        pfn_dlss5_create = NULL;
        pfn_dlss5_destroy = NULL;
        pfn_dlss5_process_frame = NULL;
        pfn_dlss5_set_scale = NULL;
        pfn_dlss5_set_gain = NULL;
        pfn_dlss5_set_blend = NULL;
        pfn_dlss5_set_sharpness = NULL;
        pfn_dlss5_framegen_transfer = NULL;
        pfn_dlss5_framegen_interpolate = NULL;
        pfn_dlss5_set_framegen = NULL;
        pfn_dlss5_get_framegen = NULL;
        pfn_dlss5_set_framegen_mode = NULL;
        pfn_dlss5_get_framegen_mode = NULL;
        pfn_dlss5_set_phase30 = NULL;
        pfn_dlss5_set_intensity = NULL;
        pfn_dlss5_set_local_tone = NULL;
        pfn_dlss5_set_local_structure = NULL;
        pfn_dlss5_set_skin_structure = NULL;
        pfn_dlss5_set_auto_mask = NULL;
        pfn_dlss5_set_style_index = NULL;
        pfn_dlss5_set_color_strength = NULL;
        g_inprocess_active = 0;
        g_inprocess_attempted = 0;
        /* A fresh engine must see the knob file again: drop the cached
         * "current" values so the next poll pushes the file state. */
        g_knob_seen = 0;
        memset(&g_knob_stamp, 0, sizeof(g_knob_stamp));
        g_knob_next_poll = 0;
        g_cur_framegen = -1;
        g_cur_scale = g_cur_gain = g_cur_blend = g_cur_sharpness = -1.0f;
        nr_lock_destroy();
    }
    return TRUE;
}
