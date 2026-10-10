// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine front-end setup (createFrontEnd) (see engine.h).
// ============================================================================
#include "engine.h"
#include "engine_dbg.h"

namespace d5c {

// --------------------------------------------------------- front-end ------
bool ChainEngine::createFrontEnd() {
    const VkCtx& c = vkc_.vk;
    const ArenaOffsets& O = arena_.off;

    imgIn_ = CreateImage2D(c, W_, H_, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // Stage 25: imgIn_ -> bufPrevRaw_ copy
                           &memImgIn_);
    imgInView_ = CreateView(c, imgIn_, VK_FORMAT_B8G8R8A8_UNORM);
    const uint64_t uploadBytes = (uint64_t)W_ * H_ * 4 * 2;
    bufUpload_ = CreateBuf(c, uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &memUpload_);
    D5C_VK_RET(vkMapMemory(c.dev, memUpload_, 0, uploadBytes, 0, &uploadPtr_));

    // shared 19-binding descriptor layout (identical to m8b: the same
    // transport shaders are used; legacy bindings stay bound to dummies).
    // 17 = previous-frame temporal state copy (hsPrev), 18 = block mvs.
    {
        VkDescriptorSetLayoutBinding binds[19]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 1; i <= 8; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 10; i <= 18; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 19; ci.pBindings = binds;
        D5C_VK_RET(vkCreateDescriptorSetLayout(c.dev, &ci, nullptr, &dsLayout_));
    }
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1; ci.pSetLayouts = &dsLayout_;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pcr;
        D5C_VK_RET(vkCreatePipelineLayout(c.dev, &ci, nullptr, &pipeLayout_));
    }
    auto makePipe = [&](const char* name) -> VkPipeline {
        VkShaderModule sm = LoadSpv(c, cfg_.shaderDir + name + ".spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = sm; ci.stage.pName = "main";
        ci.layout = pipeLayout_;
        VkPipeline p;
        D5C_VK_FATAL(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        vkDestroyShaderModule(c.dev, sm, nullptr);
        return p;
    };
    pipeDecode_ = makePipe("decode");
    pipeFeatures_ = makePipe("features");
    pipeCompose_ = makePipe("compose");
    pipeEncode_ = makePipe("encode");
    pipeFrameGen_ = makePipe("framegen");
    try {
        pipeFgInterp_ = makePipe("fginterp");
    } catch (const std::exception& e) {
        // Deployment without fginterp.spv: mode 2 clamps to mode 1.
        std::fprintf(stderr, "[VK] fginterp unavailable (%s) - fg mode 2 off\n", e.what());
        pipeFgInterp_ = VK_NULL_HANDLE;
    }
    if (cfg_.temporal) {
        try {
            pipeMotionEst_ = makePipe("motion_est");
        } catch (const std::exception& e) {
            // REQUIRED by the v10 temporal path: without it temporal_nr
            // would warp state on uninitialised motion vectors - degrade
            // the whole temporal stage instead of emitting garbage.
            std::fprintf(stderr, "[VK] motion_est unavailable (%s) - temporal off\n", e.what());
            cfg_.temporal = false;
        }
        try {
            pipeTemporalNr_ = makePipe("temporal_nr");
        } catch (const std::exception& e) {
            // A deployment missing temporal_nr.spv must not brick the whole
            // engine - degrade to memoryless reconstruction instead.
            std::fprintf(stderr, "[VK] temporal_nr unavailable (%s) - temporal off\n", e.what());
            cfg_.temporal = false;
        }
        try {
            pipeStats_ = makePipe("temporal_stats");
        } catch (const std::exception& e) {
            // Missing temporal_stats.spv only disables the gain anchor -
            // temporal_nr runs with pc.c.x = 0 and never touches binding 16.
            std::fprintf(stderr, "[VK] temporal_stats unavailable (%s) - gain lock off\n", e.what());
            pipeStats_ = VK_NULL_HANDLE;
        }
    }
    const int tpipes = cfg_.temporal ? (pipeStats_ ? 2 : 1) : 0;
    if (cfg_.renderScale < 1.0f) {
        pipeRescale_ = makePipe("rescale");
        std::printf("[VK] %d pipelines up (decode/rescale/features/compose/encode/framegen%s%s; absolute mode)\n",
                    6 + tpipes, tpipes ? "/temporal" : "",
                    pipeStats_ ? "+stats" : "");
    } else {
        std::printf("[VK] %d pipelines up (decode/features/compose/encode/framegen%s%s; absolute mode)\n",
                    5 + tpipes, tpipes ? "/temporal" : "",
                    pipeStats_ ? "+stats" : "");
    }

    // buffers (m8b parity: same bindings, dummy sizes where the M9b absolute
    // path never reads them)
    bufRgb_ = CreateBuf(c, (uint64_t)W_ * H_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memRgb_);
    bufMax_ = CreateBuf(c, 4096 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &memMax_);
    const uint64_t Tfloats = (cfg_.renderScale < 1.0f)
        ? std::max((uint64_t)regionW_ * netH_ * 3, (uint64_t)netW_ * regionH_ * 3)
        : 4096;
    bufT_ = CreateBuf(c, Tfloats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memT_);
    bufFin_ = CreateBuf(c, (uint64_t)regionW_ * regionH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFin_);
    bufNr_ = CreateBuf(c, (uint64_t)regionW_ * regionH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memNr_);
    // Temporal history + residual-hold state (binding 13), fp32[TOK*10]:
    // [0,3*TOK)      = emitted-byte history (scaled_color, unchanged)
    // [3*TOK,6*TOK)  = previous frame's input col01 (rewritten every frame)
    // [6*TOK,9*TOK)  = anchor input (col01 at the last residual refresh /
    //                  fresh emit) - temporal_nr's residual hold emits
    //                  col01 + held residual while |col01 - anchor| <= D_REF
    // [9*TOK,10*TOK) = remaining refresh frames (bounded R_FRAMES budget).
    bufHist_ = CreateBuf(c, (uint64_t)st_[0].TOK * 10 * 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHist_);
    // bufHistPrev_ (binding 17): same layout, refreshed each frame by a
    // whole-buffer copy at the top of recordTemporal BEFORE the temporal
    // dispatches. temporal_stats/temporal_nr read prior state from here
    // so the warped (motion-compensated) neighbour taps never race
    // temporal_nr's own writes to binding 13.
    bufHistPrev_ = CreateBuf(c, (uint64_t)st_[0].TOK * 10 * 4,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHistPrev_);
    // bufMv_ (binding 18): one vec4 per 8x8 block of the net grid -
    // (mvx, mvy, conf, unused) in net pixels, written by motion_est.comp.
    const uint64_t mvFloats =
        (uint64_t)((netW_ + 7) / 8) * (uint64_t)((netH_ + 7) / 8) * 4;
    bufMv_ = CreateBuf(c, mvFloats * 4,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memMv_);
    // Host-visible staging for the D5C_DEBUG_MV periodic readback - only
    // allocated when the env asked for it; the per-frame copy in
    // recordFrame is gated on mvDebug_ as well, so the debug path costs
    // nothing in normal operation.
    if (mvDebug_) {
        bufMvRb_ = CreateBuf(c, mvFloats * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &memMvRb_);
        D5C_VK_RET(vkMapMemory(c.dev, memMvRb_, 0, mvFloats * 4, 0, &mvRbPtr_));
    }
    bufRbFinal_ = CreateBuf(c, (uint64_t)W_ * H_ * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &memRbFinal_);
    D5C_VK_RET(vkMapMemory(c.dev, memRbFinal_, 0, (uint64_t)W_ * H_ * 4, 0, &rbFinalPtr_));
    DbgLayout dl = dbgLayout(st_, T6P_);
    bufDbg_ = CreateBuf(c, dl.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &memDbg_);
    // fbcancel-era buffers: encode declares binding 10 (LastP); keep valid
    // device-local buffers bound even though absolute mode never reads them
    // (host-visible STORAGE buffers fault the Arc 101.8805 driver).
    bufLastPresented_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memLastP_);
    bufFbStats_ = CreateBuf(c, 8,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFbStats_);
    // Gain-anchor luma sums (binding 16): 8 x u32. [0] current input luma
    // (features), [1] current predicted luma (temporal_stats), [2:3]
    // previous frame's input/achieved-output sums (rotated in by
    // temporal_stats), [4:5] next-frame sums (temporal_nr). recordFrame
    // zeroes [0:1] every frame and the whole buffer on a history reset.
    bufStats_ = CreateBuf(c, 32,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memStats_);
    bufHp_ = CreateBuf(c, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHp_);

    bufPrevFrame_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memPrevFrame_);
    bufCurrFrame_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memCurrFrame_);
    bufInterpOut_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memInterpOut_);
    // FrameGen residual transfer: raw input of the last processed frame
    // (binding 4). Written by processFrame, read by transferFrame.
    bufPrevRaw_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memPrevRaw_);
    // Mode-2 anchor interpolation: newest processed anchor procB
    // (binding 5; procA rides in bufPrevFrame_) and the real captured
    // middle frame midRaw (binding 10) - the confidence fallback that
    // keeps scene cuts and disocclusion honest.
    bufCurProc_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memCurProc_);
    bufMidRaw_ = CreateBuf(c, (uint64_t)W_ * H_ * 4,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memMidRaw_);

    if (cfg_.renderScale < 1.0f) {
        bufNetRgb_ = CreateBuf(c, (uint64_t)netW_ * netH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memNetRgb_);
        bufFinLow_ = CreateBuf(c, (uint64_t)netW_ * netH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFinLow_);
    }

    imgFinal_ = CreateImage2D(c, W_, H_, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgFinal_);
    viewFinal_ = CreateView(c, imgFinal_, VK_FORMAT_B8G8R8A8_UNORM);

    // descriptors
    {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 8;
        dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
        D5C_VK_RET(vkCreateDescriptorPool(c.dev, &dpci, nullptr, &dpool_));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsLayout_;
        D5C_VK_RET(vkAllocateDescriptorSets(c.dev, &ai, &setFinal_));
        D5C_VK_RET(vkAllocateDescriptorSets(c.dev, &ai, &setFrameGen_));
        if (cfg_.renderScale < 1.0f) {
            D5C_VK_RET(vkAllocateDescriptorSets(c.dev, &ai, &setDown_));
            D5C_VK_RET(vkAllocateDescriptorSets(c.dev, &ai, &setUp_));
        }
    }
    auto writeBuf = [&](VkDescriptorSet ds, uint32_t bind, VkBuffer buf, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, 0, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeBufRange = [&](VkDescriptorSet ds, uint32_t bind, VkBuffer buf, VkDeviceSize off, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, off, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeImg = [&](VkDescriptorSet ds, uint32_t bind, VkImageView v) {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };

    if (cfg_.renderScale == 1.0f) {
        writeImg(setFinal_, 0, imgInView_);
        writeBuf(setFinal_, 1, bufRgb_, (uint64_t)W_ * H_ * 3 * 4);
        writeBuf(setFinal_, 2, bufT_, Tfloats * 4);
        writeBufRange(setFinal_, 3, arena_.bufOf(O.oFeatV), arena_.localOff(O.oFeatV), (uint64_t)st_[0].TOK * 16 * 4);
        // binding 4: features.comp emits the f16 chain input oX16 itself
        // (featpack pass removed); oHEAD is compact [TOK,4] fp32 now.
        writeBufRange(setFinal_, 4, arena_.bufOf(O.oX16), arena_.localOff(O.oX16), (uint64_t)st_[0].TOK * 16 * 2);
        writeBufRange(setFinal_, 5, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 4 * 4);
        writeBuf(setFinal_, 6, bufFin_, (uint64_t)regionW_ * regionH_ * 3 * 4);
        writeBuf(setFinal_, 7, bufNr_, (uint64_t)regionW_ * regionH_ * 3 * 4);
        writeBuf(setFinal_, 8, bufMax_, 4096 * 4);
        writeImg(setFinal_, 9, viewFinal_);
        writeBuf(setFinal_, 10, bufLastPresented_, (uint64_t)W_ * H_ * 4);
        writeBuf(setFinal_, 11, bufFbStats_, 8);
        writeBuf(setFinal_, 12, bufHp_, 65536);
        writeBuf(setFinal_, 13, bufHist_, (uint64_t)st_[0].TOK * 10 * 4);
        writeBufRange(setFinal_, 14, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 4 * 4);
        writeBufRange(setFinal_, 15, arena_.bufOf(O.oHeadDC), arena_.localOff(O.oHeadDC), 1024);
        writeBuf(setFinal_, 16, bufStats_, 32);
        writeBuf(setFinal_, 17, bufHistPrev_, (uint64_t)st_[0].TOK * 10 * 4);
        writeBuf(setFinal_, 18, bufMv_, mvFloats * 4);
    } else {
        writeImg(setFinal_, 0, imgInView_);
        writeBuf(setFinal_, 1, bufNetRgb_, (uint64_t)netW_ * netH_ * 3 * 4);
        writeBuf(setFinal_, 2, bufT_, Tfloats * 4);
        writeBufRange(setFinal_, 3, arena_.bufOf(O.oFeatV), arena_.localOff(O.oFeatV), (uint64_t)st_[0].TOK * 16 * 4);
        // binding 4: features.comp emits the f16 chain input oX16 itself
        writeBufRange(setFinal_, 4, arena_.bufOf(O.oX16), arena_.localOff(O.oX16), (uint64_t)st_[0].TOK * 16 * 2);
        writeBufRange(setFinal_, 5, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 4 * 4);
        writeBuf(setFinal_, 6, bufFinLow_, (uint64_t)netW_ * netH_ * 3 * 4);
        writeBuf(setFinal_, 7, bufNr_, (uint64_t)regionW_ * regionH_ * 3 * 4);
        writeBuf(setFinal_, 8, bufMax_, 4096 * 4);
        writeImg(setFinal_, 9, viewFinal_);
        writeBuf(setFinal_, 10, bufLastPresented_, (uint64_t)W_ * H_ * 4);
        writeBuf(setFinal_, 11, bufFbStats_, 8);
        writeBuf(setFinal_, 12, bufHp_, 65536);
        writeBuf(setFinal_, 13, bufHist_, (uint64_t)st_[0].TOK * 10 * 4);
        writeBufRange(setFinal_, 14, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 4 * 4);
        writeBufRange(setFinal_, 15, arena_.bufOf(O.oHeadDC), arena_.localOff(O.oHeadDC), 1024);
        writeBuf(setFinal_, 16, bufStats_, 32);
        writeBuf(setFinal_, 17, bufHistPrev_, (uint64_t)st_[0].TOK * 10 * 4);
        writeBuf(setFinal_, 18, bufMv_, mvFloats * 4);

        writeImg(setDown_, 0, imgInView_);
        writeBuf(setDown_, 1, bufRgb_, (uint64_t)W_ * H_ * 3 * 4);
        writeBuf(setDown_, 2, bufT_, Tfloats * 4);
        writeBuf(setDown_, 4, bufNetRgb_, (uint64_t)netW_ * netH_ * 3 * 4);
        writeBuf(setDown_, 8, bufMax_, 4096 * 4);

        writeImg(setUp_, 0, imgInView_);
        writeBuf(setUp_, 1, bufRgb_, (uint64_t)W_ * H_ * 3 * 4);
        writeBuf(setUp_, 2, bufT_, Tfloats * 4);
        writeBuf(setUp_, 4, bufFinLow_, (uint64_t)netW_ * netH_ * 3 * 4);
        writeBuf(setUp_, 5, bufFin_, (uint64_t)regionW_ * regionH_ * 3 * 4);
        writeBuf(setUp_, 6, bufFin_, (uint64_t)regionW_ * regionH_ * 3 * 4);
        writeBuf(setUp_, 8, bufMax_, 4096 * 4);
        writeImg(setUp_, 9, viewFinal_);
        writeBuf(setUp_, 10, bufLastPresented_, (uint64_t)W_ * H_ * 4);
    }

    writeBuf(setFrameGen_, 1, bufPrevFrame_, (uint64_t)W_ * H_ * 4);
    writeBuf(setFrameGen_, 2, bufCurrFrame_, (uint64_t)W_ * H_ * 4);
    writeBuf(setFrameGen_, 3, bufInterpOut_, (uint64_t)W_ * H_ * 4);
    writeBuf(setFrameGen_, 4, bufPrevRaw_, (uint64_t)W_ * H_ * 4);
    writeBuf(setFrameGen_, 5, bufCurProc_, (uint64_t)W_ * H_ * 4);
    writeBuf(setFrameGen_, 10, bufMidRaw_, (uint64_t)W_ * H_ * 4);

    std::printf("[mem] front-end up: canvas %ux%u, region %ux%u at (%u,%u), extent %ux%u\n",
                W_, H_, regionW_, regionH_, cfg_.regionLeft, cfg_.regionTop, netW_, netH_);
    return true;
}
} // namespace d5c
