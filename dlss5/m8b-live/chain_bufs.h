// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Chain-side buffers/pipelines + the BDA push-constant structs
// (extracted from main.cpp). These are the 71-block chain's own plumbing
// (BDA address pushes), distinct from the transport Push in vk_util.h.
// ============================================================================
#pragma once

#include "vk_util.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

struct ChainBuf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct ChainPipe {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
};

uint32_t chainMemType(const VkCtx &c, VkMemoryPropertyFlags want);
void     chainAllocDev(const VkCtx &c, VkDeviceSize size, ChainBuf &b);
void     chainAllocHost(const VkCtx &c, VkDeviceSize size, ChainBuf &b);
void     chainMakePipe(const VkCtx &c, const std::string &spv, ChainPipe &out);

// ------------------------------------------------- chain push constants --
// push blocks (scalar layout) - verbatim from m8-full-chain main.cpp
#pragma pack(push, 1)
struct GemmPush {
    uint64_t a, b, c;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
};

struct CosPush { uint64_t a, c, d; uint32_t m, flags; };

struct CosWinPush { uint64_t a, c, d; uint32_t m, flags, C, hcount; };

#pragma pack(pop)  // SMaxPush: scalar layout aligns uint64 `bias` to 8 (offset 32);
                   // pack(1) would place it at 28 -> shader reads garbage nonzero
                   // bias -> wild BDA deref -> async device lost (M8a run-5 root cause)
struct SMaxPush { uint64_t a, c; uint32_t m, n; float p0; uint64_t bias; uint32_t hcount; uint32_t nReal; };
#pragma pack(push, 1)

struct EWPush { uint64_t a, b, c, d, h; uint32_t n, kind, ch; };

struct PartPush { uint64_t a, c; uint32_t Himg, Wimg, C, padTop, padLeft, wp8, flags; };

struct TransWePush { uint64_t a, c; uint32_t W, hcount; };

struct GatherPush {
    uint64_t a, b, d, c, h;
    uint32_t Himg, Wimg, C, padTop, padLeft, wp8, flags;
};

struct MergePush { uint64_t a, b, c, d, e, h; uint32_t n, ch, kind; };

struct PackPush { uint64_t src, dst; uint32_t n; };
// headpack two-pass push: mode 0 accumulates per-channel DC, mode 1 writes
// the calibrated matched residual (see shaders/m8/headpack.comp).
struct HeadPush { uint64_t src, dst, dc; uint32_t ntok; float gain; uint32_t mode; uint32_t cols; };
#pragma pack(pop)

enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1 };
