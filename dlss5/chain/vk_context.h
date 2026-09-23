// ============================================================================
// dlss5/chain — VkContext: Vulkan instance + LUID-matched (or first-capable)
// device with the M8a requirement set (coopmat 8x16x16 f16xf16->f32, BDA,
// f16, memory model). Extracted from dlss5/m8b-live/main.cpp section 3.
// ============================================================================
#pragma once

#include "vk_util.h"

namespace d5c {

struct VkContextConfig {
    bool needSurfaceExts = false;   // VK_KHR_surface + VK_KHR_win32_surface (overlay apps)
    bool needSwapchain = false;     // VK_KHR_swapchain device ext (overlay apps)
    bool useLuid = false;           // match the DXGI adapter LUID (m8b capture path)
    LUID luid{};
    const char* appName = "d5c-chain";
};

class VkContext {
public:
    bool create(const VkContextConfig& cfg);
    void destroy();

    VkCtx vk{};   // public POD: helpers (CreateBuf & co) take it by const&

private:
    bool pickDevice(const VkContextConfig& cfg);
    bool checkCoopmat();
    bool createDevice(const VkContextConfig& cfg);

    VkContextConfig cfg_{};
};

} // namespace d5c
