// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_gpuwarp.h - GPU motion-warped residual compositor for the layer.
 *
 * The CPU serve-residual path (nr_residual.c + nr_warp.c) kills ghosts by
 * masking per pixel, but coverage sawtooths on sustained motion (the
 * residual is a positional map of an OLDER frame - on a pan the content
 * moved, the flat mask rejects it, coverage dithers 55->40% between the
 * daemon's ~70 ms replies and the rejected share flashes raw).
 *
 * This module moves the compose onto the game's own VkDevice/queue:
 *   pass A: quarter-res symmetric block match srcDS -> curDS produces a
 *           per-DS-pixel motion field (the same 3x3-SAD coarse+fine
 *           search the daemon-side framegen.comp uses);
 *   pass B: per output pixel, the flat CPU mask runs first; rejects then
 *           sample the field (bilinear, full-res units), refine +-2, and
 *           fall back to the dense r<=4 rings - the FIRST position whose
 *           displaced source still matches cur within px_eps supplies the
 *           residual. Ghost-free by the same construction as the CPU
 *           path: res can only land where content verifies against cur.
 *
 * The composite is written straight into the presented swapchain image
 * (buffer->image copy at the tail of the same submit) - the caller skips
 * its usual staging->image write-back. Per present the only input upload
 * is the captured frame copy staging->curBuf; the pair (src, res) rides
 * the same submit only when a fresh reply made it dirty. One semaphore
 * ring slot per apply mirrors the layer's transfer() convention so the
 * NR_LAYER_SYNC=semaphore path stays correct.
 *
 * Failure discipline: ANY Vulkan error poisons the module and the caller
 * falls through to the CPU masked path for the session - the game never
 * sees a black screen from this path.
 *
 * Threading: present-thread only, like nr_residual. No file-scope state.
 * ==========================================================================*/
#ifndef NR_GPUWARP_H
#define NR_GPUWARP_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrGpuWarp NrGpuWarp;

/* Host callbacks keep the module free of layer file-statics:
 *   mem_type_fn - pick a memory type index for (type_bits, want props)
 *                 (the layer's memory_type()); UINT32_MAX = unavailable.
 *   fam_ok_fn   - queue family can run compute+transfer (the layer's
 *                 family_can_capture()); 0 = not usable. */
typedef uint32_t (*nr_gw_mem_type_fn)(void *ctx, uint32_t type_bits,
                                      VkMemoryPropertyFlags want);
typedef int (*nr_gw_fam_ok_fn)(void *ctx, uint32_t family);

/* apply() verdicts */
enum {
    NR_GPUWARP_OK = 0,   /* submitted; dst image holds the composite */
    NR_GPUWARP_SKIP,     /* policy reject (no pair/extent/age) - run CPU */
    NR_GPUWARP_FAIL = -1 /* a Vulkan call failed; module self-poisoned */
};

/* Creates pipelines + the command pool on the game's device. queue_family
 * is the present queue's family (verified for compute first). radius_ds
 * is the block-match radius in QUARTER-RES px (<=0 -> 16 = +-64 full-res).
 * Returns NULL on any init failure (safe: caller keeps the CPU path). */
NrGpuWarp *nr_gpuwarp_create(VkPhysicalDevice phys, VkDevice dev,
                             uint32_t queue_family,
                             PFN_vkGetDeviceProcAddr gdproc,
                             nr_gw_mem_type_fn mem_type, void *mem_ctx,
                             nr_gw_fam_ok_fn fam_ok, void *fam_ctx,
                             long radius_ds, int write_dbg);
void nr_gpuwarp_destroy(NrGpuWarp *g);

/* Per reply: stage the pair (src bytes + computed residual bytes, both
 * w*h*4) into the persistent upload buffer; the device copy rides the
 * next apply() submit. Returns 0 ok / -1 module dead. A pair whose extent
 * differs from the currently presented one is still staged - apply()
 * re-checks at present time like nr_residual does. */
int nr_gpuwarp_set_pair(NrGpuWarp *g, const unsigned char *src,
                        const signed char *res, uint32_t w, uint32_t h,
                        uint64_t chain, uint64_t stamp_ms);

/* Drop the held pair (knob turn invalidates the enhancement it encodes). */
void nr_gpuwarp_invalidate(NrGpuWarp *g);

/* Policy gate, call once per present before touching anything else:
 * 0 = the compose WILL run for (w,h,chain) at now_ms; NR_GPUWARP_SKIP =
 * pair ineligible (caller falls back to the CPU path, which owns the
 * verdict classes); NR_GPUWARP_FAIL = module dead. */
int nr_gpuwarp_will_run(NrGpuWarp *g, uint32_t w, uint32_t h,
                        uint64_t chain, long max_age_ms, uint64_t now_ms);

/* Record + submit the whole compose on the game's present queue:
 * [pair upload + srcDS if dirty] -> fill counters -> staging->cur copy ->
 * curDS -> match -> compose -> outBuf -> swapchain image copy. Coverage
 * counters are read BEFORE recording (the pre-record fence wait retires
 * the previous submit), so the logged coverage trails one present.
 * cur_buf holds the current captured frame (w*h*4 at offset 0).
 * want_signal: nonzero -> the submit signals an internal ring semaphore;
 * nr_gpuwarp_done_sem() returns the slot the caller's present must wait
 * on (the NR_LAYER_SYNC=semaphore convention). Idle mode passes 0.
 * Returns NR_GPUWARP_OK / NR_GPUWARP_FAIL. */
int nr_gpuwarp_run(NrGpuWarp *g, VkQueue queue, VkBuffer cur_buf,
                   VkImage dst, uint32_t w, uint32_t h, int px_eps,
                   int want_signal);

/* The ring semaphore signalled by the last nr_gpuwarp_run submit (only
 * meaningful when want_signal was set). */
VkSemaphore nr_gpuwarp_done_sem(const NrGpuWarp *g);

/* Diag: coverage of the last completed compose (per-mille applied,
 * warped subset, fails). Trails one present by design. */
long nr_gpuwarp_last_cov_pm(const NrGpuWarp *g);
long nr_gpuwarp_last_warp_pm(const NrGpuWarp *g);
int  nr_gpuwarp_poisoned(const NrGpuWarp *g);

/* Test/diag surface (offline harness): the raw per-category pixel counts
 * of the most recent SUBMITTED compose - only meaningful after that
 * submit retired (device idle or a later run() fence wait). Returns 0 on
 * success. */
int nr_gpuwarp_counters(const NrGpuWarp *g, unsigned long *flat,
                        unsigned long *warped, unsigned long *fail);
/* Host-visible per-pixel debug map (only when the module was created
 * with write_dbg): one u32 per pixel = applied full-res offset packed
 * int16x2 (dx | dy<<16), 0x7FFF7FFF = rejected, 0 = flat-applied.
 * NULL when write_dbg is off or no compose ran yet. */
const uint32_t *nr_gpuwarp_dbg(const NrGpuWarp *g);

#ifdef __cplusplus
}
#endif

#endif /* NR_GPUWARP_H */
