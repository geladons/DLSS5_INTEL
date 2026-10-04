// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Vulkan helpers + error macros + the transport Push block
// (extracted from main.cpp). VK_CHECK/HR_CHECK `return 1` - call sites must
// be inside int-returning scopes (main).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult res_ = (x);                                                     \
        if (res_ != VK_SUCCESS) {                                                \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                    \
            return 1;                                                            \
        }                                                                        \
    } while (0)
#define VK_FATAL(x)                                                              \
    do {                                                                         \
        VkResult res_ = (x);                                                     \
        if (res_ != VK_SUCCESS) {                                                \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                    \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)
#define HR_CHECK(x)                                                              \
    do {                                                                         \
        HRESULT hr_ = (x);                                                       \
        if (FAILED(hr_)) {                                                       \
            std::fprintf(stderr, "[D3D-FAIL] %s -> 0x%08lx at %s:%d\n", #x,      \
                         (unsigned long)hr_, __FILE__, __LINE__);                \
            return 1;                                                            \
        }                                                                        \
    } while (0)

struct VkCtx {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties memProps;
};

// Transport-shader push block (the shared 16-int/16-float push layout every
// front-end shader consumes).
struct Push { int32_t a[4]; int32_t b[4]; float c[4]; float d[4]; };

uint32_t       FindMemType(const VkCtx& c, uint32_t typeBits, VkMemoryPropertyFlags want);
VkBuffer       CreateBuf(const VkCtx& c, uint64_t bytes, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags want, VkDeviceMemory* memOut);
VkImage        CreateImage2D(const VkCtx& c, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImageUsageFlags usage, VkDeviceMemory* memOut);
VkImageView    CreateView(const VkCtx& c, VkImage img, VkFormat fmt);
VkShaderModule LoadSpv(const VkCtx& c, const std::string& file);

// ============================================================================
