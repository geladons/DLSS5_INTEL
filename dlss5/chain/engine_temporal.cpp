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
//   1. temporal_stats (optional - needs temporal_stats.spv): accumulates
//      this frame's predicted-output luma sum into the stats buffer for the
//      gain anchor, and rotates last frame's next-slots into the prev slots
//      (see temporal_stats.comp for the slot map). Runs on the raw head
//      BEFORE temporal_nr rewrites it.
//   2. temporal_nr: blends the head toward the previous OUTPUT via the
//      learned per-pixel alpha (head[3]) - damps per-frame reconstruction
//      jitter. In-place head rewrite + next-frame history write. tp.c[0]
//      carries the gain-anchor EMA alpha (forced 0 when pipeStats_ is
//      missing - graceful degrade when temporal_stats.spv is absent).
//   Both shaders get histValid in d[0] (same convention as features'
//   pc.d.x): 0 on a history-reset frame (first frame after a scene cut /
//   stream start / resetHistory) -> the blend is skipped outright instead
//   of dragging a col01-substituted "history".
void ChainEngine::recordTemporal() {
    const float histValid = (cfg_.temporal && framesDone_ > 0
                             && !histSuppressed_) ? 1.0f : 0.0f;
    if (pipeStats_) {
        Push sp{};
        sp.a[0] = (int32_t)st_[0].TOK;
        sp.d[0] = histValid;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeStats_);
        vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(Push), &sp);
        vkCmdDispatch(cmd_, (st_[0].TOK + 255) / 256, 1, 1);
        barrierAll();
    }
    Push tp{};
    tp.a[0] = (int32_t)st_[0].TOK;
    tp.c[0] = pipeStats_ ? cfg_.gainLock : 0.0f;
    tp.d[0] = histValid;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeTemporalNr_);
    vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(Push), &tp);
    vkCmdDispatch(cmd_, (st_[0].TOK + 255) / 256, 1, 1);
    barrierAll();
}

} // namespace d5c
