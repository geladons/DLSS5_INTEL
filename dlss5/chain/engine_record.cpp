// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine per-frame record (recordFrame) (see engine.h).
// ============================================================================
#include "engine.h"


namespace d5c {

// ------------------------------------------------------------ record ------
void ChainEngine::recordFrame(long frameIndex) {
    const uint32_t rl = cfg_.regionLeft, rt = cfg_.regionTop;
    if (prof_.enabled()) prof_.begin(cmd_);
    vkCmdResetQueryPool(cmd_, tsPool_, 0, 6);
    // Gain-anchor stats: [0]=cur input luma sum, [1]=cur predicted luma
    // sum (both accumulated this frame). A history reset (framesDone_==0)
    // zeroes ALL slots so the anchor restarts with prev=0 => off until
    // temporal_stats/temporal_nr fill two frames of sums.
    vkCmdFillBuffer(cmd_, bufStats_, 0, framesDone_ == 0 ? 32 : 8, 0);
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    {   // upload -> imgIn
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = bufUpload_;
        bb.size = VK_WHOLE_SIZE;
        bb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &bb, 0, nullptr);
        VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.image = imgIn_;
        imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imb.srcAccessMask = 0; imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {W_, H_, 1};
        vkCmdCopyBufferToImage(cmd_, bufUpload_, imgIn_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
        imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
    }
    auto push = [&](const Push& p) {
        vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    };
    if (cfg_.renderScale == 1.0f) {
        {   // decode: BGRA -> float RGB (absolute mode; driver-reversed rgba8 read)
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                                    &setFinal_, 0, nullptr);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "decode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1,
                                               (uint64_t)W_ * H_ * 16);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 0);
            // features at the full vendor extent (temporal OFF: channels 7-9 take
            // the vendor first-frame layout every frame)
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFeatures_);
            float p30Ctrl[5];
            cfg_.phase30.computeFeatureControls(p30Ctrl);
            Push p{};
            p.a[0] = (int32_t)netW_; p.a[1] = (int32_t)netH_;
            p.a[2] = (int32_t)regionW_; p.a[3] = (int32_t)regionH_;
            // Pinned vendor noise seed: a per-frame seed redraws noise every frame and shimmers static content.
            p.b[0] = 1;
            p.b[1] = (int32_t)rl; p.b[2] = (int32_t)rt; p.b[3] = (int32_t)W_;
            p.c[0] = p30Ctrl[0]; // ch 10: style
            p.c[1] = p30Ctrl[1]; // ch 11: tone
            p.c[2] = p30Ctrl[2]; // ch 12: structure
            p.c[3] = p30Ctrl[3]; // ch 13: skin_structure
            p.d[0] = (!cfg_.temporal || framesDone_ == 0)
                      ? 0.0f : (histSuppressed_ ? 2.0f : 1.0f);  // histValid
            p.d[1] = p30Ctrl[4]; // ch 14: auto_mask_structure
            p.d[2] = 1.0f;       // phase30Valid
            p.d[3] = 0.0f;
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, ((uint64_t)netW_ * netH_ + 255) / 256, 1, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "features", "", 0, 16, 0,
                                               (uint32_t)(((uint64_t)netW_ * netH_ + 255) / 256), 1, 1,
                                               (uint64_t)netW_ * netH_ * 12 + (uint64_t)st_[0].TOK * 64);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 1);
        }
        {   // featpack is gone: features.comp writes the f16 chain input oX16
            // itself (binding 4). ts slot 2 still marks the chain boundary so
            // FrameStats (fpMs ~= 0) stays layout-compatible.
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 2);
            // full 71-block DLSSNR U-Net chain (~1400 dispatches), replayed
            // from the cached secondary command buffer
            recordChainCached();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 3);
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 4);
        }
        if (cfg_.temporal) {
            // temporal_stats + temporal_nr: engine_temporal.cpp
            recordTemporal();
        }
        {   // compose (vendor compose_head on the raw head, absolute mode) + encode
            Push p{};
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeCompose_);
            p.a[0] = (int32_t)W_; p.a[1] = (int32_t)rt; p.a[2] = (int32_t)rl; p.a[3] = (int32_t)netW_;
            p.b[0] = (int32_t)regionW_; p.b[1] = (int32_t)regionH_;
            p.b[2] = 4;                           // head is compact [TOK,4] fp32
            p.c[0] = 12.0f / 255.0f;              // legacy accumulate clamp (unused in absolute mode)
            p.c[1] = 0.0f;                        // absolute mode (no accumulate)
            p.c[2] = cfg_.headGain;               // vendor residual scale
            p.c[3] = cfg_.phase30.intensity;      // Phase 30 intensity extrapolation (default 1.6)
            p.d[0] = cfg_.phase30.colorStrength;  // Phase 30 color strength (default 1.0)
            p.d[1] = 1.0f;                        // Phase 30 compose active flag
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, ((uint64_t)regionW_ * regionH_ + 255) / 256, 1, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "compose", "", 0, 0, 0,
                                               (uint32_t)(((uint64_t)regionW_ * regionH_ + 255) / 256), 1, 1,
                                               (uint64_t)regionW_ * regionH_ * 24 + (uint64_t)st_[0].TOK * 64);
            barrierAll();
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode_);
            p.a[0] = (int32_t)W_; p.a[1] = (int32_t)H_; p.a[2] = (int32_t)rl; p.a[3] = (int32_t)rt;
            p.b[0] = (int32_t)regionW_; p.b[1] = (int32_t)regionH_; p.b[2] = 0;
            p.b[3] = 0;                   // absolute (direct color)
            p.c[0] = 0.0f;
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "encode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1,
                                               (uint64_t)regionW_ * regionH_ * 12 + (uint64_t)W_ * H_ * 4);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 5);
        }
    } else {
        {   // decode: BGRA -> float RGB into bufRgb_
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                                    &setDown_, 0, nullptr);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "decode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1,
                                               (uint64_t)W_ * H_ * 16);
            barrierAll();

            // Downscale: 2 passes of pipeRescale_
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale_);
            // V pass: axis 0, bufRgb_(1) -> bufT_(2)
            Push p{};
            p.a[0] = (int32_t)W_;
            p.a[1] = (int32_t)(rt * W_ + rl);
            p.a[2] = (int32_t)regionH_;
            p.a[3] = (int32_t)regionW_;
            p.b[0] = (int32_t)netH_;
            p.b[1] = (int32_t)regionW_;
            p.b[2] = 0; // V axis
            p.b[3] = 3; // channels
            p.d[0] = 1.0f; // src buf 1 (bufRgb_)
            p.d[1] = 2.0f; // dst buf 2 (bufT_)
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (regionW_ + 15) / 16, (netH_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "rescale_down_v", "", 0, 0, 0, (regionW_ + 15) / 16, (netH_ + 15) / 16, 1,
                                               (uint64_t)regionW_ * (regionH_ + netH_) * 12);
            barrierAll();

            // H pass: axis 1, bufT_(2) -> bufNetRgb_(4)
            p.a[0] = (int32_t)regionW_;
            p.a[1] = 0;
            p.a[2] = (int32_t)regionW_;
            p.a[3] = (int32_t)netH_;
            p.b[0] = (int32_t)netW_;
            p.b[1] = (int32_t)netW_;
            p.b[2] = 1; // H axis
            p.b[3] = 3; // channels
            p.d[0] = 2.0f; // src buf 2 (bufT_)
            p.d[1] = 4.0f; // dst buf 4 (bufNetRgb_)
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (netW_ + 15) / 16, (netH_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "rescale_down_h", "", 0, 0, 0, (netW_ + 15) / 16, (netH_ + 15) / 16, 1,
                                               (uint64_t)netH_ * (regionW_ + netW_) * 12);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 0);

            // features at net dimensions on bufNetRgb_
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFeatures_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                                    &setFinal_, 0, nullptr);
            p = Push{};
            float p30Ctrl[5];
            cfg_.phase30.computeFeatureControls(p30Ctrl);
            p.a[0] = (int32_t)netW_; p.a[1] = (int32_t)netH_;
            p.a[2] = (int32_t)netW_; p.a[3] = (int32_t)netH_;
            // Pinned vendor noise seed: a per-frame seed redraws noise every frame and shimmers static content.
            p.b[0] = 1;
            p.b[1] = 0; p.b[2] = 0; p.b[3] = (int32_t)netW_;
            p.c[0] = p30Ctrl[0]; // ch 10: style
            p.c[1] = p30Ctrl[1]; // ch 11: tone
            p.c[2] = p30Ctrl[2]; // ch 12: structure
            p.c[3] = p30Ctrl[3]; // ch 13: skin_structure
            p.d[0] = (!cfg_.temporal || framesDone_ == 0)
                      ? 0.0f : (histSuppressed_ ? 2.0f : 1.0f);  // histValid
            p.d[1] = p30Ctrl[4]; // ch 14: auto_mask_structure
            p.d[2] = 1.0f;       // phase30Valid
            p.d[3] = 0.0f;
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, ((uint64_t)netW_ * netH_ + 255) / 256, 1, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "features", "", 0, 16, 0,
                                               (uint32_t)(((uint64_t)netW_ * netH_ + 255) / 256), 1, 1,
                                               (uint64_t)netW_ * netH_ * 12 + (uint64_t)st_[0].TOK * 64);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 1);
        }
        {   // featpack is gone: features.comp writes the f16 chain input oX16
            // itself (binding 4); ts slot 2 still marks the chain boundary.
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 2);
            // full 71-block DLSSNR U-Net chain, replayed from the cached
            // secondary command buffer
            recordChainCached();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 3);
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 4);
        }
        if (cfg_.temporal) {
            // temporal_stats + temporal_nr: engine_temporal.cpp
            recordTemporal();
        }
        {   // compose on (netW_, netH_) with output into bufFinLow_
            Push p{};
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeCompose_);
            p.a[0] = (int32_t)netW_; p.a[1] = 0; p.a[2] = 0; p.a[3] = (int32_t)netW_;
            p.b[0] = (int32_t)netW_; p.b[1] = (int32_t)netH_;
            p.b[2] = 4;                           // head is compact [TOK,4] fp32
            p.c[0] = 12.0f / 255.0f;
            p.c[1] = 0.0f;
            p.c[2] = cfg_.headGain;
            p.c[3] = cfg_.phase30.intensity;
            p.d[0] = cfg_.phase30.colorStrength;
            p.d[1] = 1.0f;
            p.d[2] = 1.0f;  // delta output: bufFinLow_ carries the SIGNED
                            // residual, upscaled and added to full-res native
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, ((uint64_t)netW_ * netH_ + 255) / 256, 1, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "compose", "", 0, 0, 0,
                                               (uint32_t)(((uint64_t)netW_ * netH_ + 255) / 256), 1, 1,
                                               (uint64_t)netW_ * netH_ * 24 + (uint64_t)st_[0].TOK * 64);
            barrierAll();

            // Upscale: 2 passes of pipeRescale_
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                                    &setUp_, 0, nullptr);
            // V pass: axis 0, bufFinLow_(4) -> bufT_(2)
            p = Push{};
            p.a[0] = (int32_t)netW_;
            p.a[1] = 0;
            p.a[2] = (int32_t)netH_;
            p.a[3] = (int32_t)netW_;
            p.b[0] = (int32_t)regionH_;
            p.b[1] = (int32_t)netW_;
            p.b[2] = 0; // V axis
            p.b[3] = 3; // channels
            p.d[0] = 4.0f; // src buf 4 (bufFinLow_)
            p.d[1] = 2.0f; // dst buf 2 (bufT_)
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (netW_ + 15) / 16, (regionH_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "rescale_up_v", "", 0, 0, 0, (netW_ + 15) / 16, (regionH_ + 15) / 16, 1,
                                               (uint64_t)netW_ * (netH_ + regionH_) * 12);
            barrierAll();

            // H pass: axis 1, bufT_(2) -> bufFin_(6)
            p.a[0] = (int32_t)netW_;
            p.a[1] = 0;
            p.a[2] = (int32_t)netW_;
            p.a[3] = (int32_t)regionH_;
            p.b[0] = (int32_t)regionW_;
            p.b[1] = (int32_t)regionW_;
            p.b[2] = 1; // H axis
            p.b[3] = 3; // channels
            p.d[0] = 2.0f; // src buf 2 (bufT_)
            p.d[1] = 6.0f; // dst buf 6 (bufFin_)
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (regionW_ + 15) / 16, (regionH_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "rescale_up_h", "", 0, 0, 0, (regionW_ + 15) / 16, (regionH_ + 15) / 16, 1,
                                               (uint64_t)regionH_ * (netW_ + regionW_) * 12);
            barrierAll();

            // Encode: bufFin_(6) + bufRgb_(1) -> viewFinal_(9)
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode_);
            p.a[0] = (int32_t)W_; p.a[1] = (int32_t)H_; p.a[2] = (int32_t)rl; p.a[3] = (int32_t)rt;
            p.b[0] = (int32_t)regionW_; p.b[1] = (int32_t)regionH_; p.b[2] = 0;
            p.b[3] = 2;                   // residual reconstruction:
                                          // region px = native + upscaled delta
            p.c[0] = cfg_.sharpness;
            push(p);
            if (prof_.enabled()) prof_.markBegin(cmd_);
            vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
            if (prof_.enabled()) prof_.markEnd(cmd_, "encode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1,
                                               (uint64_t)regionW_ * regionH_ * 12 + (uint64_t)W_ * H_ * 4);
            barrierAll();
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 5);
        }
    }
    if (cfg_.debugDumps && frameIndex == 1) recordDebugCopies();
    {   // readback: imgFinal -> bufRbFinal (host-coherent)
        VkImageMemoryBarrier vb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.image = imgFinal_;
        vb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vb.oldLayout = VK_IMAGE_LAYOUT_GENERAL; vb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; vb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &vb);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {W_, H_, 1};
        vkCmdCopyImageToBuffer(cmd_, imgFinal_, VK_IMAGE_LAYOUT_GENERAL, bufRbFinal_, 1, &rgn);
        // FG anchor bookkeeping. Mode 2 keeps a two-anchor pair:
        //   procA <- procB <- imgFinal   (bufPrevFrame_ <- bufCurProc_ <- new)
        //   rawA  <- rawB  <- imgIn      (bufPrevRaw_  <- bufCurrFrame_ <- new)
        // Mode <=1 keeps the single previous pair (byte-identical stream).
        // fgMode_==0 skips the copies outright: nothing consumes the anchor
        // buffers while framegen is disabled, and the validity flags stay
        // cleared (see processFrame) so a re-enable request falls back to a
        // full process that re-records them fresh.
        const bool fg2 = (fgMode_ == 2 && bufCurProc_);
        const VkDeviceSize fb2 = (VkDeviceSize)W_ * H_ * 4;
        if (fgMode_ != 0) {
        if (fg2) {
            VkBufferCopy cpRot{0, 0, fb2};
            vkCmdCopyBuffer(cmd_, bufCurProc_, bufPrevFrame_, 1, &cpRot);
            vkCmdCopyImageToBuffer(cmd_, imgFinal_, VK_IMAGE_LAYOUT_GENERAL, bufCurProc_, 1, &rgn);
        } else if (bufPrevFrame_) {
            vkCmdCopyImageToBuffer(cmd_, imgFinal_, VK_IMAGE_LAYOUT_GENERAL, bufPrevFrame_, 1, &rgn);
        }
        if (bufPrevRaw_) {
            // imgIn_ (raw input) -> anchor raw for the framegen pass;
            // imgIn_ is GENERAL and last read by decode.
            VkImageMemoryBarrier vbIn{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            vbIn.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vbIn.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vbIn.image = imgIn_;
            vbIn.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vbIn.oldLayout = VK_IMAGE_LAYOUT_GENERAL; vbIn.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            vbIn.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; vbIn.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &vbIn);
            if (fg2) {
                VkBufferCopy cpRot{0, 0, fb2};
                vkCmdCopyBuffer(cmd_, bufCurrFrame_, bufPrevRaw_, 1, &cpRot);
                vkCmdCopyImageToBuffer(cmd_, imgIn_, VK_IMAGE_LAYOUT_GENERAL, bufCurrFrame_, 1, &rgn);
            } else {
                vkCmdCopyImageToBuffer(cmd_, imgIn_, VK_IMAGE_LAYOUT_GENERAL, bufPrevRaw_, 1, &rgn);
            }
        }
        }
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    }
    // D5C_DEBUG_MV: stage this frame's block motion vectors for the
    // post-fence log in submitAndReadback (every frame - the log only
    // prints every 30th, the staged copy is always the latest frame's).
    if (mvDebug_ && bufMv_ && bufMvRb_) {
        VkBufferMemoryBarrier mvb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        mvb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mvb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mvb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mvb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mvb.buffer = bufMv_; mvb.offset = 0;
        mvb.size = (VkDeviceSize)((netW_ + 7) / 8) * ((netH_ + 7) / 8) * 16;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             1, &mvb, 0, nullptr);
        VkBufferCopy mcp{0, 0, mvb.size};
        vkCmdCopyBuffer(cmd_, bufMv_, bufMvRb_, 1, &mcp);
    }
}
} // namespace d5c
