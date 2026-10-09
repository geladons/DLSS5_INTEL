// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine implementation (see engine.h).
// ============================================================================
#include "engine.h"
#include "fp16.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <new>

namespace d5c {


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

    // Scene-cut gate (detectSceneCut): threshold on the mean abs luma diff
    // of the decimated input thumb, normalized to 1.0 (0.15 ~= 38/255 -
    // the reference content-reset level). D5C_CUT_LIMIT / DLSS5_CUT_LIMIT
    // override, 0 disables entirely; D5C_DEBUG_CUT=1 logs each detected
    // cut to stderr. Env is launch-frozen so re-reading per init is just
    // the baseline snapshot; a re-init (extent/config change) also drops
    // the stale thumb baseline via cutThumbValid_.
    cutLimit_ = 0.15f;
    if (const char* e = std::getenv("D5C_CUT_LIMIT"))
        cutLimit_ = (float)std::atof(e);
    else if (const char* e2 = std::getenv("DLSS5_CUT_LIMIT"))
        cutLimit_ = (float)std::atof(e2);
    if (cutLimit_ < 0.0f) cutLimit_ = 0.0f;
    if (const char* d = std::getenv("D5C_DEBUG_CUT"))
        cutDebug_ = (std::atoi(d) != 0);
    else
        cutDebug_ = false;
    cutThumbValid_ = false;

    // Motion gate (histSuppressed_): fraction of thumb cells that moved
    // suppresses history participation for the frame - hysteresis hi/lo,
    // D5C_TMGATE=0 (or the DLSS5_/NR_LAYER_ aliases) disables the gate.
    if (const char* e = std::getenv("D5C_TMGATE"))
        tmGate_ = (std::atoi(e) != 0);
    if (const char* e = std::getenv("DLSS5_TMGATE"))
        tmGate_ = (std::atoi(e) != 0);
    if (const char* e = std::getenv("NR_LAYER_TMGATE"))
        tmGate_ = (std::atoi(e) != 0);
    if (const char* e = std::getenv("D5C_TMGATE_HI"))
        tmSuppressHi_ = (float)std::atof(e);
    else if (const char* e = std::getenv("DLSS5_TMGATE_HI"))
        tmSuppressHi_ = (float)std::atof(e);
    if (const char* e = std::getenv("D5C_TMGATE_LO"))
        tmSuppressLo_ = (float)std::atof(e);
    else if (const char* e = std::getenv("DLSS5_TMGATE_LO"))
        tmSuppressLo_ = (float)std::atof(e);
    if (const char* d = std::getenv("D5C_DEBUG_TMGATE"))
        tmDebug_ = (std::atoi(d) != 0);
    histSuppressed_ = false;

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
        ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;   // cached 71-block chain (see recordChainCached)
        D5C_VK_RET(vkAllocateCommandBuffers(c.dev, &ai, &chainCmd_));
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
    if (cfg_.renderScale < 0.33f) cfg_.renderScale = 0.33f;
    if (cfg_.renderScale > 1.0f) cfg_.renderScale = 1.0f;
    uint32_t targetW = (uint32_t)round(regionW_ * cfg_.renderScale);
    uint32_t targetH = (uint32_t)round(regionH_ * cfg_.renderScale);
    netW_ = align64up(targetW);
    netH_ = align64up(targetH);
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
    // Optional INT8 quantized weights pack (auto-probe sibling or work/mlxw/dlssnr-int8.bin)
    std::string int8Path;
    if (const char* envI8 = std::getenv("D5C_INT8_PACK")) {
        int8Path = envI8;
    } else {
        auto slash = cfg.weightsPath.find_last_of("/\\");
        std::string dir = (slash != std::string::npos) ? cfg.weightsPath.substr(0, slash + 1) : "";
        if (std::ifstream(dir + "dlssnr-int8.bin", std::ios::binary).good()) {
            int8Path = dir + "dlssnr-int8.bin";
        } else if (std::ifstream("work/mlxw/dlssnr-int8.bin", std::ios::binary).good()) {
            int8Path = "work/mlxw/dlssnr-int8.bin";
        }
    }
    if (!int8Path.empty() && std::getenv("D5C_NO_INT8") == nullptr) {
        std::printf("[chain] loading INT8 weights pack alongside: %s\n", int8Path.c_str());
        ws_.loadInt8(int8Path);
    }
    if (!ws_.computePack()) return false;
    arena_.layout(st_, T6P_, ws_.packTotal, ws_);
    arena_.allocDevice(c);
    std::printf("[chain] allocating host weight staging: %.1f MB\n",
                arena_.totalBytes / 1048576.0);
    std::fflush(stdout);
    std::vector<char> hostImg;
    try {
        hostImg.resize((size_t)arena_.totalBytes);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "[FAIL] host weight staging allocation failed (%.1f MB)\n",
                     arena_.totalBytes / 1048576.0);
        return false;
    }
    std::printf("[chain] host staging allocated; preprocessing tensors\n");
    std::fflush(stdout);
    ws_.preprocessInto(hostImg.data(), arena_.vecOff, arena_.off.oHeadW);
    std::printf("[chain] host weight preprocessing complete\n");
    std::fflush(stdout);
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
    // Everything the chain records is (re)built above: stage extents, arena
    // chunk BDA bases, weight-pack offsets, recorder pipelines, and the
    // D5C_INT8_* pilot envs read while recording. Mark the cached secondary
    // stale so it re-records on the next processFrame. This is the ONLY
    // invalidation point - reconfigure goes through shutdown()+init().
    chainDirty_ = true;
    if (!cfg.profPath.empty()) {
        prof_.create(c, cfg.profPath);
        rec_.setProf(&prof_);
        std::printf("[prof] per-dispatch profiling ON -> %s\n", cfg.profPath.c_str());
    }
    return createFrontEnd();
}
void ChainEngine::barrierAll() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// Emits the 71-block chain into cmd_. recordChain's push constants are BDA
// addresses into the device arena + weight pack and its barriers are plain
// compute-stage, so the command stream is extent/config-static between
// inits: record it once into chainCmd_ (secondary, SIMULTANEOUS_USE) and
// replay per frame via vkCmdExecuteCommands. chainDirty_ (set in init, the
// only place chain-affecting state changes) re-records lazily on the first
// frame after. Re-recording is safe mid-frame: cmd_ recording is host-side
// and the previous submit already passed its fence.
// With the per-dispatch profiler enabled the chain records inline instead:
// prof marks carry CPU-side slot bookkeeping (count_/evs_) that advances
// only while recording, so replaying a cached buffer would corrupt the CSV.
void ChainEngine::recordChainCached() {
    if (prof_.enabled()) {
        rec_.recordChain(cmd_);
        return;
    }
    if (chainDirty_) {
        VkCommandBufferInheritanceInfo inh{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        bi.pInheritanceInfo = &inh;
        D5C_VK_FATAL(vkBeginCommandBuffer(chainCmd_, &bi));   // implicit reset of prior record
        rec_.recordChain(chainCmd_);
        D5C_VK_FATAL(vkEndCommandBuffer(chainCmd_));
        chainDirty_ = false;
    }
    vkCmdExecuteCommands(cmd_, 1, &chainCmd_);
}
// ------------------------------------------------------ scene-cut gate ----
// Content-based temporal reset (field report: "phantom" ghost trails and
// colour drift on scene cuts / camera switches - the temporal blend kept
// dragging stale history across unrelated content; the reference
// implementation drops history when mean|frame-diff| > ~0.15). Compares a
// strided 64x36 luma thumb of the CURRENT host input against the previous
// frame's; mean abs diff above cutLimit_ flags a cut. Pure CPU work on the
// already-resident input pointer (~2.3K samples), no GPU buffers. Runs
// regardless of cfg_.temporal so a live re-enable never compares against a
// stale baseline. The caller resets ONLY framesDone_ (histValid + the
// stats-slot clear in recordFrame) - FG anchors keep their own staleness
// logic, and extent changes re-init the engine (fresh thumb baseline).
bool ChainEngine::detectSceneCut(const uint8_t* bgraIn) {
    if (cutLimit_ <= 0.0f || !W_ || !H_ || !bgraIn) return false;
    const uint32_t ax = std::max(1u, W_ / (uint32_t)kCutThumbW);   // grid stride
    const uint32_t ay = std::max(1u, H_ / (uint32_t)kCutThumbH);
    uint8_t cur[kCutThumbW * kCutThumbH];
    uint64_t acc = 0;
    uint32_t moved = 0;
    for (int ty = 0; ty < kCutThumbH; ++ty) {
        const uint32_t y = std::min(H_ - 1, (uint32_t)ty * ay);
        const uint8_t* row = bgraIn + (size_t)y * W_ * 4;
        for (int tx = 0; tx < kCutThumbW; ++tx) {
            const uint32_t x = std::min(W_ - 1, (uint32_t)tx * ax);
            const uint8_t* px = row + (size_t)x * 4;                        // BGRA
            const int l = (px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8;     // BT.601 luma
            cur[ty * kCutThumbW + tx] = (uint8_t)l;
            if (cutThumbValid_) {
                const int dl = std::abs(l - (int)cutThumb_[ty * kCutThumbW + tx]);
                acc += (uint64_t)dl;
                if (dl > kTmCellDelta) ++moved;
            }
        }
    }
    bool cut = false;
    if (cutThumbValid_) {
        tmFrac_ = (float)moved / (float)(kCutThumbW * kCutThumbH);
        if (tmGate_) {
            if (!histSuppressed_ && tmFrac_ > tmSuppressHi_) {
                histSuppressed_ = true;
                if (tmDebug_)
                    std::fprintf(stderr, "[chain] motion gate: history off "
                                 "(moved %.1f%%)\n", (double)(tmFrac_ * 100));
            } else if (histSuppressed_ && tmFrac_ < tmSuppressLo_) {
                histSuppressed_ = false;
                if (tmDebug_)
                    std::fprintf(stderr, "[chain] motion gate: history on "
                                 "(moved %.1f%%)\n", (double)(tmFrac_ * 100));
            }
        }
        const float mean = (float)((double)acc / (double)(kCutThumbW * kCutThumbH) / 255.0);
        cut = mean > cutLimit_;
        if (cut && cutDebug_)
            std::fprintf(stderr, "[chain] scene cut (meandiff=%.3f) - history reset\n",
                         (double)mean);
    }
    std::memcpy(cutThumb_, cur, sizeof(cur));
    cutThumbValid_ = true;
    return cut;
}

// ------------------------------------------------------------ process -----
bool ChainEngine::processFrame(const uint8_t* bgraIn, uint8_t* bgraOut, long frameIndex,
                               FrameStats* stats) {
    const VkCtx& c = vkc_.vk;
    auto t0 = std::chrono::steady_clock::now();
    // Scene-cut gate: on a cut bufHist_ holds unrelated output, so drop
    // framesDone_ BEFORE recording - this frame runs the first-frame path
    // (histValid=0, stats slots cleared) instead of blending stale history.
    if (detectSceneCut(bgraIn)) framesDone_ = 0;
    std::memcpy(uploadPtr_, bgraIn, (size_t)W_ * H_ * 4);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_RET(vkBeginCommandBuffer(cmd_, &bi));
    recordFrame(frameIndex);
    D5C_VK_RET(vkEndCommandBuffer(cmd_));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd_;
    {
        VkResult sr = vkQueueSubmit(c.queue, 1, &si, fence_);
        if (sr != VK_SUCCESS) {
            std::fprintf(stderr, "[VK-FAIL] vkQueueSubmit -> %d at %s:%d\n",
                         (int)sr, __FILE__, __LINE__);
            if (sr == VK_ERROR_DEVICE_LOST) DumpDeviceFault(c);
            return false;
        }
    }
    D5C_VK_RET(vkWaitForFences(c.dev, 1, &fence_, VK_TRUE, UINT64_MAX));
    vkResetFences(c.dev, 1, &fence_);
    if (prof_.enabled()) prof_.collect(c.dev, tsPeriodNs_, frameIndex);

    std::memcpy(bgraOut, rbFinalPtr_, (size_t)W_ * H_ * 4);
    // Anchor validity must mirror what the record tail actually wrote: with
    // framegen disabled (fgMode_ 0) no anchor copies were recorded, so the
    // flags stay cleared and a later transfer/interp falls back to a full
    // process - which then re-records fresh anchors on its way out.
    if (fgMode_ != 0) {
        havePrevProcessed_ = true;   // bufPrevFrame_/bufPrevRaw_ now hold a pair
        if (anchorCount_ < 2) anchorCount_++;   // mode-2 anchors rotate too
    }
    framesDone_++;               // bufHist_ now holds this frame's output -
                                 // next frame's features read it as history
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

// ------------------------------------------------------ debug fetch -------
// Diagnostics-only: stage `src` to host memory after the last submitted frame
// (fence already waited in processFrame) and write the raw bytes to `path`.
// Runs a one-shot copy on the same queue - never called on the hot path.
bool ChainEngine::debugFetch(int which, const char* path) {
    const VkCtx& c = vkc_.vk;
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize off = 0, size = 0;
    const uint64_t T0 = st_[0].TOK;
    switch (which) {
        case 0: src = bufHist_; size = T0 * 3 * 4; break;
        case 1: src = arena_.bufOf(arena_.off.oFeatV);
                off = arena_.localOff(arena_.off.oFeatV); size = T0 * 16 * 4; break;
        case 2: src = arena_.bufOf(arena_.off.oHEAD);
                off = arena_.localOff(arena_.off.oHEAD); size = T0 * 4 * 4; break;
        case 3: src = arena_.bufOf(arena_.off.oX16);
                off = arena_.localOff(arena_.off.oX16); size = T0 * 16 * 2; break;
        case 4: src = bufStats_; size = 32; break;
        case 5: if (!bufNetRgb_) return false;
                src = bufNetRgb_; size = (uint64_t)netW_ * netH_ * 3 * 4; break;
        default: return false;
    }
    if (!src || !path) return false;
    ChainBuf stg{};
    chainAllocHost(c, size, stg);
    VkCommandBuffer cb = BeginOneShot(c, cmdPool_);
    VkBufferCopy cp{off, 0, size};
    vkCmdCopyBuffer(cb, src, stg.buf, 1, &cp);
    SubmitOneShot(c, cmdPool_, fence_, cb);
    bool ok = false;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(stg.mapped, 1, (size_t)size, f);
        std::fclose(f);
        ok = true;
    }
    if (stg.mapped) vkUnmapMemory(c.dev, stg.mem);
    vkDestroyBuffer(c.dev, stg.buf, nullptr);
    vkFreeMemory(c.dev, stg.mem, nullptr);
    return ok;
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
    for (VkPipeline p : {pipeDecode_, pipeFeatures_, pipeCompose_, pipeEncode_, pipeRescale_, pipeFrameGen_, pipeTemporalNr_, pipeFgInterp_, pipeStats_})
        if (p) vkDestroyPipeline(c.dev, p, nullptr);
    pipeDecode_ = pipeFeatures_ = pipeCompose_ = pipeEncode_ = pipeRescale_ = pipeFrameGen_ = pipeTemporalNr_ = pipeFgInterp_ = pipeStats_ = VK_NULL_HANDLE;
    if (pipeLayout_) vkDestroyPipelineLayout(c.dev, pipeLayout_, nullptr);
    pipeLayout_ = VK_NULL_HANDLE;
    if (dpool_) vkDestroyDescriptorPool(c.dev, dpool_, nullptr);
    dpool_ = VK_NULL_HANDLE;
    setFinal_ = setDown_ = setUp_ = setFrameGen_ = VK_NULL_HANDLE;
    if (dsLayout_) vkDestroyDescriptorSetLayout(c.dev, dsLayout_, nullptr);
    dsLayout_ = VK_NULL_HANDLE;
    for (VkBuffer b : {bufRgb_, bufMax_, bufT_, bufFin_, bufNr_, bufRbFinal_, bufDbg_,
                       bufUpload_, bufLastPresented_, bufFbStats_, bufHp_, bufHist_,
                       bufNetRgb_, bufFinLow_, bufPrevFrame_, bufCurrFrame_, bufInterpOut_,
                       bufPrevRaw_, bufCurProc_, bufMidRaw_, bufStats_})
        if (b) vkDestroyBuffer(c.dev, b, nullptr);
    bufRgb_ = bufMax_ = bufT_ = bufFin_ = bufNr_ = bufRbFinal_ = bufDbg_ =
        bufUpload_ = bufLastPresented_ = bufFbStats_ = bufHp_ = bufHist_ =
        bufNetRgb_ = bufFinLow_ = bufPrevFrame_ = bufCurrFrame_ = bufInterpOut_ =
        bufPrevRaw_ = bufCurProc_ = bufMidRaw_ = bufStats_ = VK_NULL_HANDLE;
    for (VkDeviceMemory m : {memRgb_, memMax_, memT_, memFin_, memNr_, memRbFinal_, memDbg_,
                             memImgFinal_, memImgIn_, memUpload_, memLastP_, memFbStats_, memHp_, memHist_,
                             memNetRgb_, memFinLow_, memPrevFrame_, memCurrFrame_, memInterpOut_,
                             memPrevRaw_, memCurProc_, memMidRaw_, memStats_})
        if (m) vkFreeMemory(c.dev, m, nullptr);
    memRgb_ = memMax_ = memT_ = memFin_ = memNr_ = memRbFinal_ = memDbg_ =
        memImgFinal_ = memImgIn_ = memUpload_ = memLastP_ = memFbStats_ = memHp_ = memHist_ =
        memNetRgb_ = memFinLow_ = memPrevFrame_ = memCurrFrame_ = memInterpOut_ =
        memPrevRaw_ = memCurProc_ = memMidRaw_ = memStats_ = VK_NULL_HANDLE;
    resetAnchors();
    fgMode_ = 1;
    framesDone_ = 0;
    if (imgInView_) vkDestroyImageView(c.dev, imgInView_, nullptr);
    if (viewFinal_) vkDestroyImageView(c.dev, viewFinal_, nullptr);
    if (imgIn_) vkDestroyImage(c.dev, imgIn_, nullptr);
    if (imgFinal_) vkDestroyImage(c.dev, imgFinal_, nullptr);
    arena_.freeDevice(c);
    prof_.destroy(c.dev);
    if (fence_) vkDestroyFence(c.dev, fence_, nullptr);
    if (tsPool_) vkDestroyQueryPool(c.dev, tsPool_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(c.dev, cmdPool_, nullptr);
    cmd_ = chainCmd_ = VK_NULL_HANDLE;   // command buffers die with the pool
    chainDirty_ = true;
    vkc_.destroy();
    std::printf("[chain] engine shut down\n");
}

} // namespace d5c