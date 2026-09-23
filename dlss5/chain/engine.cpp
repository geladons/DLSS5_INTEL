// ============================================================================
// dlss5/chain — ChainEngine implementation (see engine.h).
// ============================================================================
#include "engine.h"
#include "fp16.h"

#include <chrono>
#include <direct.h>

namespace d5c {

// --------------------------- debug readback layout (frame==1) -------------
struct DbgLayout {
    uint64_t szFeat, szHead, szX16, szADA, szMRG, szG2, szHW, szB4, szB22,
             szB38, szB48, szB69, szSin, szCos, szFRS;
    uint64_t offFeat, offHead, offX16, offADA, offMRG, offG2, offHW, offB4,
             offB22, offB38, offB48, offB69, offSin, offCos, offFRS;
    uint64_t total;
};

static DbgLayout dbgLayout(const Stage st[7], uint32_t T6P) {
    auto al = [](uint64_t v) { return (v + 255) & ~255ull; };
    DbgLayout d{};
    d.szFeat = al((uint64_t)st[0].TOK * 16 * 4);
    d.szHead = al((uint64_t)st[0].TOK * 16 * 4);
    d.szX16 = al((uint64_t)st[0].TOK * 16 * 2);
    d.szADA = al((uint64_t)st[0].TOK * 32 * 4);
    d.szMRG = al((uint64_t)st[0].TOK * 32 * 4);
    d.szG2 = al((uint64_t)st[0].TOK * 32 * 4);
    d.szHW = al(1024);
    d.szB4 = al((uint64_t)st[2].TOK * 64 * 2);
    d.szB22 = al((uint64_t)st[5].TOK * 512 * 2);
    d.szB38 = al((uint64_t)T6P * 1024 * 2);
    d.szB48 = al((uint64_t)st[4].TOK * 256 * 2);
    d.szB69 = al((uint64_t)st[1].TOK * 32 * 2);
    d.szSin = al(32 * 4);
    d.szCos = al(32 * 4);
    d.szFRS = al((uint64_t)st[0].TOK * 32 * 2);
    d.offFeat = 0;
    d.offHead = d.offFeat + d.szFeat;
    d.offX16 = d.offHead + d.szHead;
    d.offADA = d.offX16 + d.szX16;
    d.offMRG = d.offADA + d.szADA;
    d.offG2 = d.offMRG + d.szMRG;
    d.offHW = d.offG2 + d.szG2;
    d.offB4 = d.offHW + d.szHW;
    d.offB22 = d.offB4 + d.szB4;
    d.offB38 = d.offB22 + d.szB22;
    d.offB48 = d.offB38 + d.szB38;
    d.offB69 = d.offB48 + d.szB48;
    d.offSin = d.offB69 + d.szB69;
    d.offCos = d.offSin + d.szSin;
    d.offFRS = d.offCos + d.szCos;
    d.total = d.offFRS + d.szFRS;
    return d;
}

// ----------------------------------------------------------------- init ---
bool ChainEngine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    W_ = cfg.canvasW; H_ = cfg.canvasH;
    if (!W_ || !H_) {
        std::fprintf(stderr, "[FAIL] ChainEngine: canvas size is zero\n");
        return false;
    }
    regionW_ = cfg.regionW ? cfg.regionW : W_;
    regionH_ = cfg.regionH ? cfg.regionH : H_;
    if (cfg.regionLeft + regionW_ > W_ || cfg.regionTop + regionH_ > H_) {
        std::fprintf(stderr, "[FAIL] ChainEngine: region exceeds canvas\n");
        return false;
    }
    InitFp16();
    InitSwizzle();
    _mkdir(cfg.dumpDir.c_str());

    if (!vkc_.create(cfg.vkCfg)) return false;
    const VkCtx& c = vkc_.vk;

    // command pool + fence + timestamp pool
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = c.qf;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        D5C_VK_RET(vkCreateCommandPool(c.dev, &ci, nullptr, &cmdPool_));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        D5C_VK_RET(vkAllocateCommandBuffers(c.dev, &ai, &cmd_));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        D5C_VK_RET(vkCreateFence(c.dev, &fci, nullptr, &fence_));
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 6;
        D5C_VK_RET(vkCreateQueryPool(c.dev, &qpci, nullptr, &tsPool_));
        VkPhysicalDeviceProperties pp{};
        vkGetPhysicalDeviceProperties(c.pd, &pp);
        tsPeriodNs_ = pp.limits.timestampPeriod;
    }

    // ---- geometry: vendor-aligned extent (mult 64, min 320, aligned UP) ----
    auto align64up = [](uint32_t v) { return (std::max(320u, v) + 63u) & ~63u; };
    netW_ = align64up(regionW_);
    netH_ = align64up(regionH_);
    st_[0] = {netH_, netW_, 0};
    st_[1] = {netH_ / 2, netW_ / 2, 0};
    st_[2] = {netH_ / 4, netW_ / 4, 0};
    st_[3] = {netH_ / 8, netW_ / 8, 0};
    st_[4] = {netH_ / 16, netW_ / 16, 0};
    st_[5] = {pad8u(st_[4].H) / 2, pad8u(st_[4].W) / 2, 0};
    st_[6] = {pad8u(st_[5].H) / 2, pad8u(st_[5].W) / 2, 0};
    for (auto& s : st_) s.TOK = s.H * s.W;
    T6_ = st_[6].TOK;
    T6P_ = (st_[6].TOK + 31) & ~31u;
    std::printf("[geometry] region %ux%u -> vendor-aligned network extent %ux%u (U-Net 7 scales)\n",
                regionW_, regionH_, netW_, netH_);
    for (int i = 0; i < 7; ++i)
        std::printf("  L%d: %4ux%-4u  TOK=%u\n", i, st_[i].H, st_[i].W, st_[i].TOK);

    // ---- weights: parse + pack + preprocess + chunked upload ----
    std::printf("loading DLSSNR graph... (weights + chain setup; ~1-2 s, not a hang)\n");
    auto tW0 = std::chrono::steady_clock::now();
    if (!ws_.load(cfg.weightsPath)) return false;
    if (!ws_.computePack()) return false;
    arena_.layout(st_, T6P_, ws_.packTotal, ws_);
    arena_.allocDevice(c);
    std::vector<char> hostImg((size_t)arena_.totalBytes);
    ws_.preprocessInto(hostImg.data(), arena_.vecOff, arena_.off.oHeadW);
    {   // host-image hash: detects CPU/RAM-side pack corruption (GPU
        // flakiness vs host flakiness discriminator; FNV-1a, one pass)
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < (size_t)arena_.totalBytes; ++i) {
            h ^= (unsigned char)hostImg[i];
            h *= 1099511628211ull;
        }
        std::printf("hostImg fnv1a: %016llx\n", (unsigned long long)h);
    }
    {
        auto tUp = std::chrono::steady_clock::now();
        arena_.upload(c, cmdPool_, fence_, hostImg.data());
        double upMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tUp).count();
        std::printf("weight upload: %.1f MB in %.1f ms (%.2f GB/s incl. preprocessing)\n",
                    arena_.totalBytes / 1048576.0, upMs,
                    arena_.totalBytes / 1073741824.0 / (upMs / 1e3));
    }
    std::printf("[chain] weights resident, setup %.0f ms\n",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tW0).count());
    hostImg.clear(); hostImg.shrink_to_fit();   // ~2 GB host copy no longer needed

    rec_.init(c, arena_, ws_, st_, cfg.shaderDir);
    if (!cfg.profPath.empty()) {
        prof_.create(c, cfg.profPath);
        rec_.setProf(&prof_);
        std::printf("[prof] per-dispatch profiling ON -> %s\n", cfg.profPath.c_str());
    }
    return createFrontEnd();
}

// --------------------------------------------------------- front-end ------
bool ChainEngine::createFrontEnd() {
    const VkCtx& c = vkc_.vk;
    const ArenaOffsets& O = arena_.off;

    imgIn_ = CreateImage2D(c, W_, H_, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &memImgIn_);
    imgInView_ = CreateView(c, imgIn_, VK_FORMAT_B8G8R8A8_UNORM);
    bufUpload_ = CreateBuf(c, (uint64_t)W_ * H_ * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &memUpload_);
    D5C_VK_RET(vkMapMemory(c.dev, memUpload_, 0, (uint64_t)W_ * H_ * 4, 0, &uploadPtr_));

    // shared 16-binding descriptor layout (identical to m8b: the same
    // transport shaders are used; legacy bindings stay bound to dummies)
    {
        VkDescriptorSetLayoutBinding binds[16]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 1; i <= 8; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 10; i <= 15; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 16; ci.pBindings = binds;
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
    std::printf("[VK] 4 transport pipelines up (decode/features/compose/encode; absolute mode)\n");

    // buffers (m8b parity: same bindings, dummy sizes where the M9b absolute
    // path never reads them)
    bufRgb_ = CreateBuf(c, (uint64_t)W_ * H_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memRgb_);
    bufMax_ = CreateBuf(c, 4096 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &memMax_);
    const uint64_t Tfloats = 4096;   // legacy binding-2 scratch
    bufT_ = CreateBuf(c, Tfloats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memT_);
    bufFin_ = CreateBuf(c, (uint64_t)regionW_ * regionH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFin_);
    bufNr_ = CreateBuf(c, (uint64_t)regionW_ * regionH_ * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memNr_);
    bufHist_ = CreateBuf(c, (uint64_t)st_[0].TOK * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHist_);
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
    bufHp_ = CreateBuf(c, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHp_);

    imgFinal_ = CreateImage2D(c, W_, H_, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgFinal_);
    viewFinal_ = CreateView(c, imgFinal_, VK_FORMAT_B8G8R8A8_UNORM);

    // descriptors
    {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 4;
        dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
        D5C_VK_RET(vkCreateDescriptorPool(c.dev, &dpci, nullptr, &dpool_));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsLayout_;
        D5C_VK_RET(vkAllocateDescriptorSets(c.dev, &ai, &setFinal_));
    }
    auto writeBuf = [&](uint32_t bind, VkBuffer buf, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, 0, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = setFinal_; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeBufRange = [&](uint32_t bind, VkBuffer buf, VkDeviceSize off, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, off, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = setFinal_; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeImg = [&](uint32_t bind, VkImageView v) {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = setFinal_; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    writeImg(0, imgInView_);
    writeBuf(1, bufRgb_, (uint64_t)W_ * H_ * 3 * 4);
    writeBuf(2, bufT_, Tfloats * 4);
    writeBufRange(3, arena_.bufOf(O.oFeatV), arena_.localOff(O.oFeatV), (uint64_t)st_[0].TOK * 16 * 4);
    writeBufRange(4, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 16 * 4);
    writeBufRange(5, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 16 * 4);
    writeBuf(6, bufFin_, (uint64_t)regionW_ * regionH_ * 3 * 4);
    writeBuf(7, bufNr_, (uint64_t)regionW_ * regionH_ * 3 * 4);
    writeBuf(8, bufMax_, 4096 * 4);
    writeImg(9, viewFinal_);
    writeBuf(10, bufLastPresented_, (uint64_t)W_ * H_ * 4);
    writeBuf(11, bufFbStats_, 8);
    writeBuf(12, bufHp_, 65536);
    writeBuf(13, bufHist_, (uint64_t)st_[0].TOK * 3 * 4);
    writeBufRange(14, arena_.bufOf(O.oHEAD), arena_.localOff(O.oHEAD), (uint64_t)st_[0].TOK * 16 * 4);
    writeBufRange(15, arena_.bufOf(O.oHeadDC), arena_.localOff(O.oHeadDC), 1024);
    std::printf("[mem] front-end up: canvas %ux%u, region %ux%u at (%u,%u), extent %ux%u\n",
                W_, H_, regionW_, regionH_, cfg_.regionLeft, cfg_.regionTop, netW_, netH_);
    return true;
}

void ChainEngine::barrierAll() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ------------------------------------------------------------ record ------
void ChainEngine::recordFrame(long frameIndex) {
    const uint32_t rl = cfg_.regionLeft, rt = cfg_.regionTop;
    if (prof_.enabled()) prof_.begin(cmd_);
    vkCmdResetQueryPool(cmd_, tsPool_, 0, 6);
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
    {   // decode: BGRA -> float RGB (absolute mode; driver-reversed rgba8 read)
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                                &setFinal_, 0, nullptr);
        vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
        if (prof_.enabled()) prof_.mark(cmd_, "decode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1);
        barrierAll();
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 0);
        // features at the full vendor extent (temporal OFF: channels 7-9 take
        // the vendor first-frame layout every frame)
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFeatures_);
        Push p{};
        p.a[0] = (int32_t)netW_; p.a[1] = (int32_t)netH_;
        p.a[2] = (int32_t)regionW_; p.a[3] = (int32_t)regionH_;
        p.b[0] = (int32_t)(frameIndex & 0x7fffffff);
        p.b[1] = (int32_t)rl; p.b[2] = (int32_t)rt; p.b[3] = (int32_t)W_;
        p.c[0] = 0.0f; p.c[1] = 1.0f; p.c[2] = 1.0f;
        p.d[0] = 0.0f; p.d[1] = 0.0f; p.d[2] = 0.0f; p.d[3] = 0.0f;
        push(p);
        vkCmdDispatch(cmd_, ((uint64_t)netW_ * netH_ + 255) / 256, 1, 1);
        if (prof_.enabled()) prof_.mark(cmd_, "features", "", 0, 16, 0,
                                        (uint32_t)(((uint64_t)netW_ * netH_ + 255) / 256), 1, 1);
        barrierAll();
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 1);
    }
    {   // featpack: features fp32 [TOK0,16] -> chain input f16 (oX16)
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, rec_.featpackPipe().pipe);
        PackPush fp{arena_.A(arena_.off.oFeatV), arena_.A(arena_.off.oX16), st_[0].TOK * 16};
        vkCmdPushConstants(cmd_, rec_.featpackPipe().layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(fp), &fp);
        vkCmdDispatch(cmd_, (st_[0].TOK * 16 + 255) / 256, 1, 1);
        if (prof_.enabled()) prof_.mark(cmd_, "featpack", "", 0, 16, 0,
                                        (st_[0].TOK * 16 + 255) / 256, 1, 1);
        barrierAll();
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 2);
        // full 71-block DLSSNR U-Net chain (~1400 dispatches)
        rec_.recordChain(cmd_);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 3);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 4);
    }
    {   // compose (vendor compose_head on the raw head, absolute mode) + encode
        Push p{};
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeCompose_);
        p.a[0] = (int32_t)W_; p.a[1] = (int32_t)rt; p.a[2] = (int32_t)rl; p.a[3] = (int32_t)netW_;
        p.b[0] = (int32_t)regionW_; p.b[1] = (int32_t)regionH_;
        p.c[0] = 12.0f / 255.0f;      // legacy accumulate clamp (unused in absolute mode)
        p.c[1] = 0.0f;                // absolute mode (no accumulate)
        p.c[2] = cfg_.headGain;       // vendor residual scale
        push(p);
        vkCmdDispatch(cmd_, ((uint64_t)regionW_ * regionH_ + 255) / 256, 1, 1);
        if (prof_.enabled()) prof_.mark(cmd_, "compose", "", 0, 0, 0,
                                        (uint32_t)(((uint64_t)regionW_ * regionH_ + 255) / 256), 1, 1);
        barrierAll();
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode_);
        p.a[0] = (int32_t)W_; p.a[1] = (int32_t)H_; p.a[2] = (int32_t)rl; p.a[3] = (int32_t)rt;
        p.b[0] = (int32_t)regionW_; p.b[1] = (int32_t)regionH_; p.b[2] = 0;
        p.b[3] = 0;                   // absolute (direct color)
        push(p);
        vkCmdDispatch(cmd_, (W_ + 15) / 16, (H_ + 15) / 16, 1);
        if (prof_.enabled()) prof_.mark(cmd_, "encode", "", 0, 0, 0, (W_ + 15) / 16, (H_ + 15) / 16, 1);
        barrierAll();
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool_, 5);
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
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    }
}

void ChainEngine::recordDebugCopies() {
    const ArenaOffsets& O = arena_.off;
    DbgLayout dl = dbgLayout(st_, T6P_);
    // M9b: sources live in different arena chunks -> one copy per entry
    // (every slot sits whole inside its chunk by construction).
    struct DbgCp { VkDeviceSize srcOff, dstOff, size; };
    DbgCp dcps[15] = {
        {O.oFeatV, dl.offFeat, (uint64_t)st_[0].TOK * 16 * 4},
        {O.oHEAD, dl.offHead, (uint64_t)st_[0].TOK * 16 * 4},
        {O.oX16, dl.offX16, (uint64_t)st_[0].TOK * 16 * 2},
        {O.oADA, dl.offADA, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oMRG70, dl.offMRG, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oG2, dl.offG2, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oHeadW, dl.offHW, 1024},
        {O.oB4DS, dl.offB4, (uint64_t)st_[2].TOK * 64 * 2},
        {O.oB22DS, dl.offB22, (uint64_t)st_[5].TOK * 512 * 2},
        {O.oB38, dl.offB38, (uint64_t)T6P_ * 1024 * 2},
        {O.oB48, dl.offB48, (uint64_t)st_[4].TOK * 256 * 2},
        {O.oB69, dl.offB69, (uint64_t)st_[1].TOK * 32 * 2},
        {arena_.vecOff["block70.layer0.inp_merge_sin"], dl.offSin, 32 * 4},
        {arena_.vecOff["block70.layer0.inp_merge_cos"], dl.offCos, 32 * 4},
        {O.oFRS, dl.offFRS, (uint64_t)st_[0].TOK * 32 * 2},
    };
    for (auto& e : dcps) {
        VkBufferCopy c1{arena_.localOff(e.srcOff), e.dstOff, e.size};
        vkCmdCopyBuffer(cmd_, arena_.bufOf(e.srcOff), bufDbg_, 1, &c1);
    }
    VkMemoryBarrier db{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; db.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &db, 0, nullptr, 0, nullptr);
}

void ChainEngine::writeDebugDumps() {
    DbgLayout dl = dbgLayout(st_, T6P_);
    void* pd = nullptr;
    D5C_VK_FATAL(vkMapMemory(vkc_.vk.dev, memDbg_, 0, VK_WHOLE_SIZE, 0, &pd));
    const char* db = (const char*)pd;
    auto wr = [&](const char* name, const void* p, uint64_t bytes) {
        std::string path = cfg_.dumpDir + "\\" + name;
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(p, 1, (size_t)bytes, f); fclose(f); }
    };
    const uint64_t T0 = st_[0].TOK;
    wr("live_featV.bin", db + dl.offFeat, (uint64_t)T0 * 16 * 4);
    wr("live_head.bin", db + dl.offHead, (uint64_t)T0 * 16 * 4);
    wr("live_x16.bin", db + dl.offX16, (uint64_t)T0 * 16 * 2);
    wr("live_ada.bin", db + dl.offADA, (uint64_t)T0 * 32 * 4);
    wr("live_mrg70.bin", db + dl.offMRG, (uint64_t)T0 * 32 * 4);
    wr("live_g2.bin", db + dl.offG2, (uint64_t)T0 * 32 * 4);
    wr("live_b4ds.f16", db + dl.offB4, (uint64_t)st_[2].TOK * 64 * 2);
    wr("live_b22ds.f16", db + dl.offB22, (uint64_t)st_[5].TOK * 512 * 2);
    wr("live_b38.f16", db + dl.offB38, (uint64_t)T6P_ * 1024 * 2);
    wr("live_b48.f16", db + dl.offB48, (uint64_t)st_[4].TOK * 256 * 2);
    wr("live_b69.f16", db + dl.offB69, (uint64_t)st_[1].TOK * 32 * 2);
    wr("live_frs.f16", db + dl.offFRS, (uint64_t)T0 * 32 * 2);
    vkUnmapMemory(vkc_.vk.dev, memDbg_);
    std::printf("[dbg] frame-1 chain-path dumps written to %s\\live_*\n", cfg_.dumpDir.c_str());
}

// ------------------------------------------------------------ process -----
bool ChainEngine::processFrame(const uint8_t* bgraIn, uint8_t* bgraOut, long frameIndex,
                               FrameStats* stats) {
    const VkCtx& c = vkc_.vk;
    auto t0 = std::chrono::steady_clock::now();
    std::memcpy(uploadPtr_, bgraIn, (size_t)W_ * H_ * 4);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_RET(vkBeginCommandBuffer(cmd_, &bi));
    recordFrame(frameIndex);
    D5C_VK_RET(vkEndCommandBuffer(cmd_));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd_;
    D5C_VK_RET(vkQueueSubmit(c.queue, 1, &si, fence_));
    D5C_VK_RET(vkWaitForFences(c.dev, 1, &fence_, VK_TRUE, UINT64_MAX));
    vkResetFences(c.dev, 1, &fence_);
    if (prof_.enabled()) prof_.collect(c.dev, tsPeriodNs_, frameIndex);

    std::memcpy(bgraOut, rbFinalPtr_, (size_t)W_ * H_ * 4);
    if (cfg_.debugDumps && frameIndex == 1) writeDebugDumps();

    if (stats) {
        uint64_t ts[6]{};
        VkResult qr = vkGetQueryPoolResults(c.dev, tsPool_, 0, 6, sizeof(ts), ts,
                                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (qr == VK_SUCCESS) {
            auto d = [&](int i, int j) { return (double)(ts[j] - ts[i]) * (double)tsPeriodNs_ / 1e6; };
            stats->feMs = d(0, 1); stats->fpMs = d(1, 2); stats->chainMs = d(2, 3);
            stats->tailMs = d(4, 5); stats->gpuTotalMs = d(0, 5);
        }
        stats->wallMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

// ----------------------------------------------------------- teardown -----
void ChainEngine::shutdown() {
    if (!vkc_.vk.dev) return;
    const VkCtx& c = vkc_.vk;
    vkDeviceWaitIdle(c.dev);
    if (uploadPtr_) vkUnmapMemory(c.dev, memUpload_);
    if (rbFinalPtr_) vkUnmapMemory(c.dev, memRbFinal_);
    uploadPtr_ = nullptr; rbFinalPtr_ = nullptr;
    rec_.destroy(c);
    for (VkPipeline p : {pipeDecode_, pipeFeatures_, pipeCompose_, pipeEncode_})
        if (p) vkDestroyPipeline(c.dev, p, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(c.dev, pipeLayout_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(c.dev, dpool_, nullptr);
    if (dsLayout_) vkDestroyDescriptorSetLayout(c.dev, dsLayout_, nullptr);
    for (VkBuffer b : {bufRgb_, bufMax_, bufT_, bufFin_, bufNr_, bufRbFinal_, bufDbg_,
                       bufUpload_, bufLastPresented_, bufFbStats_, bufHp_, bufHist_})
        if (b) vkDestroyBuffer(c.dev, b, nullptr);
    for (VkDeviceMemory m : {memRgb_, memMax_, memT_, memFin_, memNr_, memRbFinal_, memDbg_,
                             memImgFinal_, memImgIn_, memUpload_, memLastP_, memFbStats_, memHp_, memHist_})
        if (m) vkFreeMemory(c.dev, m, nullptr);
    if (imgInView_) vkDestroyImageView(c.dev, imgInView_, nullptr);
    if (viewFinal_) vkDestroyImageView(c.dev, viewFinal_, nullptr);
    if (imgIn_) vkDestroyImage(c.dev, imgIn_, nullptr);
    if (imgFinal_) vkDestroyImage(c.dev, imgFinal_, nullptr);
    arena_.freeDevice(c);
    prof_.destroy(c.dev);
    if (fence_) vkDestroyFence(c.dev, fence_, nullptr);
    if (tsPool_) vkDestroyQueryPool(c.dev, tsPool_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(c.dev, cmdPool_, nullptr);
    vkc_.destroy();
    std::printf("[chain] engine shut down\n");
}

} // namespace d5c
