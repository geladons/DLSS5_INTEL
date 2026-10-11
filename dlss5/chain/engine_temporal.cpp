// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine temporal methods (see engine.h). Split out of
// engine.cpp (file-size discipline): the temporal section is identical in
// the scale==1.0 and scale<1.0 record paths, so it lives in one place.
// ============================================================================
#include "engine.h"

namespace d5c {

// ----------------------------------------------------------- temporal -----
// Records (into cmd_, already begun):
//   0. bufHist_ -> bufHistPrev_ snapshot + transfer->compute barrier:
//      all previous-state reads (E/P/A/R, incl. warped taps) go through
//      binding 17 so they never race temporal_nr's binding-13 writes.
//   1. motion_est (required for temporal on): per-8x8-block motion
//      vectors into bufMv_ (binding 18), estimated from current input
//      luma vs hsPrev's previous-input P.
//   2. temporal_stats (optional - needs temporal_stats.spv): accumulates
//      this frame's predicted-output luma sum into the stats buffer for the
//      gain anchor, and rotates last frame's next-slots into the prev slots
//      (see temporal_stats.comp for the slot map). Runs on the raw head
//      BEFORE temporal_nr rewrites it.
//   3. temporal_nr: blends the head toward the previous OUTPUT via the
//      learned per-pixel alpha (head[3]) - damps per-frame reconstruction
//      jitter. In-place head rewrite + next-frame history write. tp.c[0]
//      carries the gain-anchor EMA alpha (forced 0 when pipeStats_ is
//      missing - graceful degrade when temporal_stats.spv is absent).
//   Both shaders get histValid in d[0] (same convention as features'
//   pc.d.x): 0 on a history-reset frame (first frame after a scene cut /
//   stream start / resetHistory) -> the blend is skipped outright instead
//   of dragging a col01-substituted "history".
void ChainEngine::recordTemporal() {
    /* histValid is a 3-state mode: 0 = reset frame (no history read -
     * bufHist holds another scene's emit, so P/A/R validity is forced
     * off and every pixel takes the fresh-emit path), 1 = normal, 2 =
     * drift-gate suppressed (history stays honest in features). The
     * shaders treat 2 exactly like 1: the residual hold needs no
     * special-casing - the pan/drift that tripped the gate moves every
     * pixel's input past IN_CUT, so the emit goes fresh and no held
     * residual can freeze a stale mask, and no transition flash (the
     * field's "picture literally pulses" was a latch->emit jump).
     * Suppression must NOT drop to 0: pure raw-prediction frames shimmer
     * every pixel each frame, and on sustained pans the gate would hold
     * that mode for the whole pan (out mad med 1.89 vs 0.33
     * unsuppressed, real GTA IV flythrough replay). */
    const float histValid = (!cfg_.temporal || framesDone_ == 0)
                            ? 0.0f : (histSuppressed_ ? 2.0f : 1.0f);
    // Snapshot last frame's temporal state into bufHistPrev_ (binding
    // 17). Everything in this section (motion_est, temporal_stats,
    // temporal_nr) reads previous state from hsPrev - never from hs -
    // so the motion-compensated warps can sample arbitrary source
    // pixels without racing temporal_nr's own writes to binding 13.
    // (hs itself still holds last frame's E/P/A/R: features read it
    // earlier this frame, and nothing writes it until temporal_nr.)
    {
        VkBufferCopy cp{0, 0, (VkDeviceSize)st_[0].TOK * 10 * 4};
        vkCmdCopyBuffer(cmd_, bufHist_, bufHistPrev_, 1, &cp);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &mb, 0, nullptr, 0, nullptr);
    }
    if (pipeMotionEst_) {
        // Per-8x8-block motion estimate (cur input luma vs hsPrev P).
        Push mp{};
        mp.a[0] = (int32_t)st_[0].TOK;
        mp.a[1] = (int32_t)netW_; mp.a[2] = (int32_t)netH_;
        mp.c[0] = mvEma_;
        mp.c[1] = mvZbias_; mp.c[2] = mvZabs_;
        mp.c[3] = mvSubt_;
        mp.d[0] = histValid;
        mp.d[1] = mvImpt_;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeMotionEst_);
        vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(Push), &mp);
        vkCmdDispatch(cmd_, (netW_ + 7) / 8, (netH_ + 7) / 8, 1);
        barrierAll();
    }
    if (pipeStats_) {
        Push sp{};
        sp.a[0] = (int32_t)st_[0].TOK;
        sp.a[1] = (int32_t)netW_; sp.a[2] = (int32_t)netH_;
        sp.c[1] = mvBeta_; sp.c[2] = mvInterp_;
        sp.c[3] = mvPixStatic_;
        sp.d[0] = histValid;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeStats_);
        vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(Push), &sp);
        vkCmdDispatch(cmd_, (st_[0].TOK + 255) / 256, 1, 1);
        barrierAll();
    }
    Push tp{};
    tp.a[0] = (int32_t)st_[0].TOK;
    tp.a[1] = (int32_t)netW_; tp.a[2] = (int32_t)netH_;
    tp.c[0] = pipeStats_ ? cfg_.gainLock : 0.0f;
    tp.c[1] = mvBeta_; tp.c[2] = mvInterp_;
    tp.c[3] = mvPixStatic_;
    tp.d[0] = histValid;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeTemporalNr_);
    vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(Push), &tp);
    vkCmdDispatch(cmd_, (st_[0].TOK + 255) / 256, 1, 1);
    barrierAll();
}

} // namespace d5c
