// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain — shared low-level Vulkan helpers for the DLSSNR chain modules.
// Extracted VERBATIM from dlss5/m8b-live/main.cpp (2026-09-22, M11 refactor).
// Namespace d5c = "dlss5 chain". No DDA / overlay / present logic here.
// ============================================================================
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
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace d5c {

// VK_CHECK variant for module code: init paths return false (D5C_VK_RET).
// D5C_VK_FATAL is used where the function cannot signal failure through its
// return value (handle/void returns). Since d5c_chain.dll now runs INSIDE
// game processes (m11-layer x64, m12-dxgi), a library must never kill its
// host: FATAL throws std::runtime_error, which unwinds to the C ABI
// try/catch in chain_c_api.cpp and surfaces as a nonzero return. In the
// standalone m11d the same throw escapes main -> terminate -> nonzero exit,
// i.e. a real error exit, not a silent 0.
#define D5C_VK_RET(x)                                                           \
    do {                                                                        \
        VkResult res_ = (x);                                                    \
        if (res_ != VK_SUCCESS) {                                               \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                   \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define D5C_VK_FATAL(x)                                                         \
    do {                                                                        \
        VkResult res_ = (x);                                                    \
        if (res_ != VK_SUCCESS) {                                               \
            char d5cmsg_[256];                                                  \
            std::snprintf(d5cmsg_, sizeof d5cmsg_,                               \
                          "[VK-FAIL] %s -> %d at %s:%d", #x, (int)res_,          \
                          __FILE__, __LINE__);                                  \
            std::fprintf(stderr, "%s\n", d5cmsg_);                              \
            throw std::runtime_error(d5cmsg_);                                  \
        }                                                                       \
    } while (0)

// -------------------------------------------------- vulkan small helpers --
struct VkCtx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qf = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool sparseBinding = false;   // queue family supports VK_QUEUE_SPARSE_BINDING_BIT
};

// VK_EXT_device_fault: dump whatever the driver reports after DEVICE_LOST
// (faulting VA ranges, vendor codes, human description). No-op when the
// extension or entry point is absent. Call right after a failed submit.
inline void DumpDeviceFault(const VkCtx& c) {
    auto fp = (PFN_vkGetDeviceFaultInfoEXT)
        vkGetDeviceProcAddr(c.dev, "vkGetDeviceFaultInfoEXT");
    if (!fp) return;
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (fp(c.dev, &counts, nullptr) != VK_SUCCESS) return;
    std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addrs.data();
    info.pVendorInfos = vendors.data();
    if (fp(c.dev, &counts, &info) != VK_SUCCESS) return;
    std::fprintf(stderr, "[VK-FAULT] \"%s\" addrs=%u vendors=%u binary=%llu\n",
                 info.description, counts.addressInfoCount,
                 counts.vendorInfoCount,
                 (unsigned long long)counts.vendorBinarySize);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i)
        std::fprintf(stderr, "[VK-FAULT] addr[%u] type=%d va=0x%llx prec=%llu\n",
                     i, (int)addrs[i].addressType,
                     (unsigned long long)addrs[i].reportedAddress,
                     (unsigned long long)addrs[i].addressPrecision);
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i)
        std::fprintf(stderr, "[VK-FAULT] vendor[%u] code=0x%llx \"%s\"\n", i,
                     (unsigned long long)vendors[i].vendorFaultCode,
                     vendors[i].description);
    std::fflush(stderr);
}

inline uint32_t FindMemType(const VkCtx& c, uint32_t typeBits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        if ((c.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

inline VkBuffer CreateBuf(const VkCtx& c, uint64_t bytes, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags want, VkDeviceMemory* memOut) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    D5C_VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, buf, &req);
    uint32_t idx = FindMemType(c, req.memoryTypeBits, want);
    if (idx == UINT32_MAX) {
        idx = FindMemType(c, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (idx == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no memory type for buffer\n");
                             throw std::runtime_error("no memory type for buffer"); }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = idx;
    VkDeviceMemory mem;
    D5C_VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
    D5C_VK_FATAL(vkBindBufferMemory(c.dev, buf, mem, 0));
    *memOut = mem;
    return buf;
}

inline VkImage CreateImage2D(const VkCtx& c, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImageUsageFlags usage, VkDeviceMemory* memOut) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    D5C_VK_FATAL(vkCreateImage(c.dev, &ici, nullptr, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.dev, img, &req);
    uint32_t idx = FindMemType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = idx;
    VkDeviceMemory mem;
    D5C_VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
    D5C_VK_FATAL(vkBindImageMemory(c.dev, img, mem, 0));
    *memOut = mem;
    return img;
}

inline VkImageView CreateView(const VkCtx& c, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v;
    D5C_VK_FATAL(vkCreateImageView(c.dev, &vci, nullptr, &v));
    return v;
}

inline VkShaderModule LoadSpv(const VkCtx& c, const std::string& file) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "[FAIL] cannot open %s\n", file.c_str());
              throw std::runtime_error("cannot open " + file); }
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> code((size_t)sz);
    if (!f.read(code.data(), sz)) { std::fprintf(stderr, "[FAIL] read spv\n");
              throw std::runtime_error("read spv " + file); }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = (size_t)sz;
    smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    D5C_VK_FATAL(vkCreateShaderModule(c.dev, &smci, nullptr, &m));
    return m;
}

// transport push block (decode/features/compose/encode share one layout)
struct Push { int32_t a[4]; int32_t b[4]; float c[4]; float d[4]; };

// one-shot command helpers (startup copies / uploads). The fence must be
// unsignalled on entry; it is reset before returning.
inline VkCommandBuffer BeginOneShot(const VkCtx& c, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    D5C_VK_FATAL(vkAllocateCommandBuffers(c.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D5C_VK_FATAL(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

inline double SubmitOneShot(const VkCtx& c, VkCommandPool pool, VkFence fence, VkCommandBuffer one) {
    D5C_VK_FATAL(vkEndCommandBuffer(one));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &one;
    D5C_VK_FATAL(vkQueueSubmit(c.queue, 1, &si, fence));
    D5C_VK_FATAL(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkResetFences(c.dev, 1, &fence);
    vkFreeCommandBuffers(c.dev, pool, 1, &one);
    return 0.0;
}

} // namespace d5c
