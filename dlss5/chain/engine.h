// ============================================================================
// dlss5/chain — ChainEngine: the full absolute (echo-free) DLSSNR pipeline.
//   BGRA frame in -> decode -> vendor features -> featpack -> 71-block chain
//   -> vendor compose_head -> encode -> BGRA frame out.
// Owns the Vulkan context, weights residency, chunked arena, chain recorder
// and the M9b front-end buffers/descriptors. No DDA, no overlay, no present —
// used by m11d (daemon) and by the m8b-live echo-free path.
// Extracted from dlss5/m8b-live/main.cpp sections 5-11 + the frame record.
// ============================================================================
#pragma once

#include "vk_util.h"
#include "vk_context.h"
#include "weights.h"
#include "arena.h"
#include "recorder.h"
#include "prof.h"

#include <string>

namespace d5c {

struct EngineConfig {
    uint32_t canvasW = 0, canvasH = 0;                 // frame size
    uint32_t regionLeft = 0, regionTop = 0;            // processing region within the canvas
    uint32_t regionW = 0, regionH = 0;                 // (0 = whole canvas)
    std::string weightsPath;                           // dlssnr-logical.safetensors
    std::string shaderDir;                             // dir with *.spv (trailing slash)
    float headGain = 1.0f;                             // vendor residual scale (--gain)
    VkContextConfig vkCfg{};                           // device selection (headless default)
    bool debugDumps = false;                           // dump live_*.bin on frame index 1
    std::string dumpDir = "out";                       // dump destination
    std::string profPath;                              // M10: per-dispatch CSV (empty = off)
};

struct FrameStats {
    double gpuTotalMs = 0, feMs = 0, fpMs = 0, chainMs = 0, tailMs = 0;
    double wallMs = 0;
};

class ChainEngine {
public:
    bool init(const EngineConfig& cfg);       // heavy: device + weights + arena (~2 s)
    // bgraIn/bgraOut: canvasW*canvasH*4 bytes. frameIndex feeds the vendor
    // deterministic noise (features pc.b.x) — validation vs torch goldens
    // uses frameIndex=1.
    bool processFrame(const uint8_t* bgraIn, uint8_t* bgraOut, long frameIndex,
                      FrameStats* stats = nullptr);
    void shutdown();

    uint32_t netW() const { return netW_; }
    uint32_t netH() const { return netH_; }
    const Stage* stages() const { return st_; }

private:
    bool createFrontEnd();        // buffers/images/descriptors/transport pipelines
    void recordFrame(long frameIndex);  // into cmd (not submitted yet)
    void recordDebugCopies();     // frame==1 chain-path readback into bufDbg
    void writeDebugDumps();       // map bufDbg, write live_*.{bin,f16}
    void barrierAll();

    EngineConfig cfg_{};
    uint32_t W_ = 0, H_ = 0;                 // canvas
    uint32_t regionW_ = 0, regionH_ = 0;
    uint32_t netW_ = 0, netH_ = 0;           // vendor-aligned extent (mult 64, min 320)
    Stage st_[7]{};
    uint64_t T6_ = 0;
    uint32_t T6P_ = 0;

    VkContext vkc_;
    WeightsStore ws_;
    ChainArena arena_;
    ChainRecorder rec_;
    ChainProf prof_;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool tsPool_ = VK_NULL_HANDLE;
    float tsPeriodNs_ = 1.0f;

    VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet setFinal_ = VK_NULL_HANDLE;

    VkPipeline pipeDecode_ = VK_NULL_HANDLE, pipeFeatures_ = VK_NULL_HANDLE,
               pipeCompose_ = VK_NULL_HANDLE, pipeEncode_ = VK_NULL_HANDLE;

    VkDeviceMemory memImgIn_ = VK_NULL_HANDLE, memUpload_ = VK_NULL_HANDLE,
                   memRgb_ = VK_NULL_HANDLE, memMax_ = VK_NULL_HANDLE,
                   memT_ = VK_NULL_HANDLE, memFin_ = VK_NULL_HANDLE, memNr_ = VK_NULL_HANDLE,
                   memHist_ = VK_NULL_HANDLE, memRbFinal_ = VK_NULL_HANDLE,
                   memDbg_ = VK_NULL_HANDLE, memImgFinal_ = VK_NULL_HANDLE,
                   memLastP_ = VK_NULL_HANDLE, memFbStats_ = VK_NULL_HANDLE, memHp_ = VK_NULL_HANDLE;
    VkImage imgIn_ = VK_NULL_HANDLE, imgFinal_ = VK_NULL_HANDLE;
    VkImageView imgInView_ = VK_NULL_HANDLE, viewFinal_ = VK_NULL_HANDLE;
    VkBuffer bufUpload_ = VK_NULL_HANDLE, bufRgb_ = VK_NULL_HANDLE, bufMax_ = VK_NULL_HANDLE,
             bufT_ = VK_NULL_HANDLE, bufFin_ = VK_NULL_HANDLE, bufNr_ = VK_NULL_HANDLE,
             bufHist_ = VK_NULL_HANDLE, bufRbFinal_ = VK_NULL_HANDLE, bufDbg_ = VK_NULL_HANDLE,
             bufLastPresented_ = VK_NULL_HANDLE, bufFbStats_ = VK_NULL_HANDLE, bufHp_ = VK_NULL_HANDLE;
    void* uploadPtr_ = nullptr;
    void* rbFinalPtr_ = nullptr;             // persistently mapped readback

    // debug dump offsets inside bufDbg (frame==1)
    uint64_t dbgOffFeat_ = 0, dbgOffHead_ = 0;
};

} // namespace d5c
