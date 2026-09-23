// ============================================================================
// dlss5/chain — ChainArena: the chunked device arena for the 71-block chain.
// Slot layout (per-scale extents) + vector side-table slots + pack region at
// offset 0, split into <=3.5 GiB device buffers (the Arc driver caps ONE
// VkDeviceMemory at maxMemoryAllocationSize ~4 GiB). BDA addressing resolves
// a logical arena offset via chunkOf/localOff.
// Extracted VERBATIM from dlss5/m8b-live/main.cpp sections 8 (arena layout).
// ============================================================================
#pragma once

#include "vk_util.h"
#include "weights.h"

#include <map>

namespace d5c {

inline uint32_t pad8u(uint32_t v) { return (v + 7u) & ~7u; }
struct Stage { uint32_t H, W, TOK; };

struct ChainBuf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

// Every chain scratch slot's logical arena offset (filled by layout()).
struct ArenaOffsets {
    VkDeviceSize oHeadW = 0;
    VkDeviceSize oX16 = 0, oADA = 0;
    VkDeviceSize oG1 = 0, oG2 = 0, oG3 = 0, oG4 = 0, oRAW = 0, oPOOL = 0;
    VkDeviceSize oWIN = 0, oPROJW = 0, oQW = 0, oKW = 0, oVW = 0, oSCW = 0,
                 oPRW = 0, oMGW = 0, oATW = 0, oABW = 0;
    VkDeviceSize oHG = 0, oPROJG = 0, oQG = 0, oKG = 0, oVG = 0, oSCG = 0,
                 oPRG = 0, oMGG = 0, oATG = 0, oABG = 0, oBRG = 0;
    VkDeviceSize oB0RAW = 0, oFRS = 0, oSKIP0 = 0, oSKIP1 = 0, oSKIP2 = 0, oSKIP3 = 0,
                 oSS = 0, oB4DS = 0, oB22DS = 0, oBRIDGE = 0, oB38 = 0, oB39 = 0,
                 oB48 = 0, oB69 = 0, oMRG39 = 0, oMRG70 = 0, oHEAD = 0;
    VkDeviceSize oFeatV = 0, oHeadDC = 0;
};

class ChainArena {
public:
    // Slot layout for the given 7-stage geometry; pack rides [0, packTotal).
    // Vector side-table slots are allocated per the weights pack contents.
    void layout(const Stage st[7], uint64_t T6P, uint64_t packTotal, const WeightsStore& ws);
    void allocDevice(const VkCtx& c);
    void freeDevice(const VkCtx& c);
    // Per-chunk staging upload of the preprocessed host image.
    void upload(const VkCtx& c, VkCommandPool pool, VkFence fence, const char* hostImg) const;

    // BDA resolution
    int chunkOf(VkDeviceSize o) const;
    uint64_t A(VkDeviceSize o) const { int i = chunkOf(o); return chunks[i].baseA + (uint64_t)(o - chunks[i].start); }
    VkBuffer bufOf(VkDeviceSize o) const { return chunks[chunkOf(o)].b.buf; }
    VkDeviceSize localOff(VkDeviceSize o) const { return o - chunks[chunkOf(o)].start; }

    ArenaOffsets off;
    std::map<std::string, VkDeviceSize> vecOff;
    VkDeviceSize scratchBytes = 0;   // ar.off — scratch after the pack
    VkDeviceSize totalBytes = 0;     // packTotal + scratch

    struct Chunk { VkDeviceSize start = 0, end = 0; ChainBuf b; uint64_t baseA = 0; };
    std::vector<Chunk> chunks;
};

void chainAllocDev(const VkCtx& c, VkDeviceSize size, ChainBuf& b);
void chainAllocHost(const VkCtx& c, VkDeviceSize size, ChainBuf& b);

} // namespace d5c
