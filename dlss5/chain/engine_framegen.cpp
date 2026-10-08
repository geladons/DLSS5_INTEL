// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine frame-generation methods (see engine.h).
// Split out of engine.cpp (file-size discipline): interpolateFrame /
// transferFrame / interpolateAnchors plus their shared record/submit tail.
// ============================================================================
#include "engine.h"

#include <chrono>
#include <cstring>

namespace d5c {

// ---------------------------------------------------------- framegen ------
// Records (into cmd_, already begun): timestamps + framegen dispatch on
// `pipe` + output barrier + readback copies. rollPrev additionally rolls
// bufCurrFrame_ into bufPrevFrame_ (interpolate cadence only; the residual
// transfer path keeps prev state anchored to the last processed frame).
void ChainEngine::recordFrameGenTail(const Push& p, VkPipeline pipe, bool rollPrev) {
    const size_t frameBytes = (size_t)W_ * H_ * 4;

    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 0);

    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                            &setFrameGen_, 0, nullptr);
    vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);

    vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);

    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 1);

    // Barrier: compute shader write -> transfer read
    VkBufferMemoryBarrier outBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    outBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    outBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outBarrier.buffer = bufInterpOut_;
    outBarrier.offset = 0;
    outBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &outBarrier, 0, nullptr);

    // Copy generated buffer -> host readback buffer
    VkBufferCopy cpOut{0, 0, (VkDeviceSize)frameBytes};
    vkCmdCopyBuffer(cmd_, bufInterpOut_, bufRbFinal_, 1, &cpOut);

    if (rollPrev) {
        // Keep bufPrevFrame_ updated with bufCurrFrame_
        VkBufferCopy cpPrev{0, 0, (VkDeviceSize)frameBytes};
        vkCmdCopyBuffer(cmd_, bufCurrFrame_, bufPrevFrame_, 1, &cpPrev);
    }

    // Barrier: transfer write -> host read
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
}

// Ends cmd_, submits, waits, then copies the readback into out and collects
// the framegen GPU timestamp. Shared by all framegen entry points.
bool ChainEngine::submitAndReadback(void* out, size_t bytes,
                                    std::chrono::steady_clock::time_point t0,
                                    FrameStats* stats) {
    const VkCtx& c = vkc_.vk;
    D5C_VK_RET(vkEndCommandBuffer(cmd_));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    D5C_VK_RET(vkQueueSubmit(c.queue, 1, &si, fence_));
    D5C_VK_RET(vkWaitForFences(c.dev, 1, &fence_, VK_TRUE, UINT64_MAX));
    vkResetFences(c.dev, 1, &fence_);

    std::memcpy(out, rbFinalPtr_, bytes);

    uint64_t ts[2]{};
    VkResult qr = vkGetQueryPoolResults(c.dev, tsPool_, 0, 2, sizeof(ts), ts,
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (qr == VK_SUCCESS) {
        lastFrameGenGpuMs_ = static_cast<double>(ts[1] - ts[0]) * static_cast<double>(tsPeriodNs_) / 1e6;
        if (stats) {
            stats->gpuTotalMs = lastFrameGenGpuMs_;
        }
    }

    if (stats) {
        stats->wallMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    return true;
}

// Upload barrier: transfer write -> compute shader read for a framegen buffer.
static void barrierUpToShader(VkCommandBuffer cmd, VkBuffer buf) {
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &b, 0, nullptr);
}

// ------------------------------------------------------------ mode --------
void ChainEngine::setFrameGenMode(int mode) {
    if (mode == 2 && !pipeFgInterp_) {
        // Deployment without fginterp.spv cannot run mode 2 - clamp so the
        // caller's mode query and the record path agree on mode 1.
        mode = 1;
    }
    if (mode != fgMode_) {
        // mode 0 disables framegen entirely: processFrame skips the anchor
        // copies and leaves the validity flags cleared, so a later transfer/
        // interp request falls back to a full process which re-records them.
        fgMode_ = (mode == 2) ? 2 : (mode >= 1 ? 1 : 0);
        resetAnchors();   // anchors are mode-shaped state
    }
}

// -------------------------------------------------------- interpolate -----
bool ChainEngine::interpolateFrame(uint32_t w, uint32_t h, const void* f0, const void* f1,
                                   void* out, float factor, FrameStats* stats) {
    if (!w || !h || !f1 || !out) return false;
    if (w != W_ || h != H_) return false;
    if (!pipeFrameGen_) return false;

    auto t0 = std::chrono::steady_clock::now();

    const size_t frameBytes = (size_t)W_ * H_ * 4;
    uint8_t* up0 = static_cast<uint8_t*>(uploadPtr_);
    uint8_t* up1 = up0 + frameBytes;

    if (f0) {
        std::memcpy(up0, f0, frameBytes);
    }
    std::memcpy(up1, f1, frameBytes);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_RET(vkBeginCommandBuffer(cmd_, &bi));

    vkCmdResetQueryPool(cmd_, tsPool_, 0, 2);

    // Staging upload -> device local buffers
    if (f0) {
        VkBufferCopy cp0{0, 0, (VkDeviceSize)frameBytes};
        vkCmdCopyBuffer(cmd_, bufUpload_, bufPrevFrame_, 1, &cp0);
    }
    VkBufferCopy cp1{(VkDeviceSize)frameBytes, 0, (VkDeviceSize)frameBytes};
    vkCmdCopyBuffer(cmd_, bufUpload_, bufCurrFrame_, 1, &cp1);

    if (f0) barrierUpToShader(cmd_, bufPrevFrame_);
    barrierUpToShader(cmd_, bufCurrFrame_);

    Push p{};
    p.a[0] = static_cast<int32_t>(W_);
    p.a[1] = static_cast<int32_t>(H_);
    p.a[2] = 8; // search radius
    p.a[3] = 2; // search step
    p.c[0] = factor;
    p.c[1] = 0.20f; // occlusion threshold
    recordFrameGenTail(p, pipeFrameGen_, true);

    return submitAndReadback(out, frameBytes, t0, stats);
}

// ----------------------------------------------------------- transfer -----
// 1:2 cadence generated frame: cur raw in, residual of the last processed
// frame carried along the raw->raw motion (framegen.comp mode 1).
bool ChainEngine::transferFrame(uint32_t w, uint32_t h, const void* curRaw,
                                void* out, FrameStats* stats) {
    if (!w || !h || !curRaw || !out) return false;
    if (w != W_ || h != H_) return false;
    if (!pipeFrameGen_ || !havePrevProcessed_) return false;

    auto t0 = std::chrono::steady_clock::now();

    const size_t frameBytes = (size_t)W_ * H_ * 4;
    uint8_t* up1 = static_cast<uint8_t*>(uploadPtr_) + frameBytes;

    std::memcpy(up1, curRaw, frameBytes);   // only cur raw is uploaded

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_RET(vkBeginCommandBuffer(cmd_, &bi));

    vkCmdResetQueryPool(cmd_, tsPool_, 0, 2);

    VkBufferCopy cp1{(VkDeviceSize)frameBytes, 0, (VkDeviceSize)frameBytes};
    vkCmdCopyBuffer(cmd_, bufUpload_, bufCurrFrame_, 1, &cp1);
    barrierUpToShader(cmd_, bufCurrFrame_);

    Push p{};
    p.a[0] = static_cast<int32_t>(W_);
    p.a[1] = static_cast<int32_t>(H_);
    p.a[2] = 16;   // search radius (2x the interpolate radius)
    p.a[3] = 2;    // search step
    p.b[0] = 1;    // mode 1 = residual transfer
    p.c[1] = 0.20f; // occlusion threshold
    recordFrameGenTail(p, pipeFrameGen_, false);   // prev stays anchored

    return submitAndReadback(out, frameBytes, t0, stats);
}

// ------------------------------------------------ anchor interpolation ----
// Mode-2 generated frame: the midpoint of the last two processed anchors.
// procA = bufPrevFrame_, rawA = bufPrevRaw_, procB = bufCurProc_,
// rawB = bufCurrFrame_ (all rotated by processFrame's record tail);
// midRaw (the real captured middle frame) is the confidence fallback.
bool ChainEngine::interpolateAnchors(uint32_t w, uint32_t h, const void* midRaw,
                                     void* out, float factor, FrameStats* stats) {
    if (!w || !h || !out) return false;
    if (w != W_ || h != H_) return false;
    if (!pipeFgInterp_ || anchorCount_ < 1) return false;

    auto t0 = std::chrono::steady_clock::now();

    const size_t frameBytes = (size_t)W_ * H_ * 4;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_RET(vkBeginCommandBuffer(cmd_, &bi));

    vkCmdResetQueryPool(cmd_, tsPool_, 0, 2);

    if (midRaw) {
        uint8_t* up1 = static_cast<uint8_t*>(uploadPtr_) + frameBytes;
        std::memcpy(up1, midRaw, frameBytes);
        VkBufferCopy cp1{(VkDeviceSize)frameBytes, 0, (VkDeviceSize)frameBytes};
        vkCmdCopyBuffer(cmd_, bufUpload_, bufMidRaw_, 1, &cp1);
        barrierUpToShader(cmd_, bufMidRaw_);
    }

    if (anchorCount_ < 2) {
        // Warmup: one anchor only - copy the newest processed anchor
        // (procB lives in bufCurProc_; it is the correct on-time frame).
        VkBufferCopy cpP{0, 0, (VkDeviceSize)frameBytes};
        vkCmdCopyBuffer(cmd_, bufCurProc_, bufInterpOut_, 1, &cpP);
        VkBufferMemoryBarrier outBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        outBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        outBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        outBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outBarrier.buffer = bufInterpOut_;
        outBarrier.offset = 0;
        outBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &outBarrier, 0, nullptr);
        VkBufferCopy cpOut{0, 0, (VkDeviceSize)frameBytes};
        vkCmdCopyBuffer(cmd_, bufInterpOut_, bufRbFinal_, 1, &cpOut);
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
        return submitAndReadback(out, frameBytes, t0, stats);
    }

    Push p{};
    p.a[0] = static_cast<int32_t>(W_);
    p.a[1] = static_cast<int32_t>(H_);
    p.a[2] = 48;   // symmetric half-motion coverage (two-frame interval)
    p.a[3] = 2;    // search step
    p.b[0] = midRaw ? 1 : 0;   // hasMid: binding-10 midRaw valid
    p.c[0] = factor;
    p.c[1] = 0.20f; // occlusion threshold
    recordFrameGenTail(p, pipeFgInterp_, false);

    return submitAndReadback(out, frameBytes, t0, stats);
}

} // namespace d5c
