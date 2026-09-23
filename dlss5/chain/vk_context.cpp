// ============================================================================
// dlss5/chain — VkContext implementation (see vk_context.h).
// ============================================================================
#include "vk_context.h"

#include <cstdlib>

namespace d5c {

static std::string LuidStr(const LUID& l) {
    char b[32];
    std::snprintf(b, sizeof b, "%08lx:%08lx", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    return b;
}

bool VkContext::create(const VkContextConfig& cfg) {
    cfg_ = cfg;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = cfg.appName;
    app.apiVersion = VK_API_VERSION_1_2;
    const char* iextSurf[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (cfg.needSurfaceExts) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = iextSurf;
    }
    D5C_VK_RET(vkCreateInstance(&ici, nullptr, &vk.inst));
    if (!pickDevice(cfg)) return false;
    if (!checkCoopmat()) return false;
    // queue family: compute+graphics (the chain and the front-end share one queue)
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qps(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.pd, &nq, qps.data());
    vk.qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
        if ((qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            vk.qf = i;
            vk.sparseBinding = (qps[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
            break;
        }
    if (vk.qf == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no compute+graphics queue\n"); return false; }
    std::printf("[VK] queue family %u sparseBinding=%d\n", vk.qf, (int)vk.sparseBinding);
    vkGetPhysicalDeviceMemoryProperties(vk.pd, &vk.memProps);
    return createDevice(cfg);
}

bool VkContext::pickDevice(const VkContextConfig& cfg) {
    uint32_t n = 0;
    D5C_VK_RET(vkEnumeratePhysicalDevices(vk.inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    D5C_VK_RET(vkEnumeratePhysicalDevices(vk.inst, &n, pds.data()));
    for (auto d : pds) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(d, &p2);
        LUID l{};
        if (id.deviceLUIDValid) std::memcpy(&l, id.deviceLUID, VK_LUID_SIZE);
        bool match = !cfg.useLuid ||
                     (id.deviceLUIDValid &&
                      l.HighPart == cfg.luid.HighPart &&
                      l.LowPart == cfg.luid.LowPart);
        bool picked = match && vk.pd == VK_NULL_HANDLE;
        std::printf("[VK] device \"%s\" LUID=%s %s\n", p2.properties.deviceName,
                    id.deviceLUIDValid ? LuidStr(l).c_str() : "(invalid)",
                    picked ? (cfg.useLuid ? "  <== MATCHES DXGI adapter" : "  <== selected") : "");
        if (picked) vk.pd = d;
    }
    if (!vk.pd) {
        std::fprintf(stderr, "[FAIL] no Vulkan device %s\n",
                     cfg.useLuid ? "LUID-matches DXGI" : "available");
        return false;
    }
    return true;
}

bool VkContext::checkCoopmat() {
    // M8a requirement: cooperative matrices 8x16x16 f16xf16->f32 (chain GEMMs)
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(vk.pd, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    vkEnumerateDeviceExtensionProperties(vk.pd, nullptr, &next, exts.data());
    bool hasCoop = false;
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0) hasCoop = true;
    if (!hasCoop) { std::fprintf(stderr, "[FAIL] matched device lacks VK_KHR_cooperative_matrix\n"); return false; }
    auto fpEnum = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        vkGetInstanceProcAddr(vk.inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    uint32_t ncfg = 0;
    D5C_VK_RET(fpEnum(vk.pd, &ncfg, nullptr));
    std::vector<VkCooperativeMatrixPropertiesKHR> cfgs(ncfg,
        VkCooperativeMatrixPropertiesKHR{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, nullptr});
    D5C_VK_RET(fpEnum(vk.pd, &ncfg, cfgs.data()));
    bool ok816 = false;
    for (auto& cm : cfgs)
        if (cm.MSize == 8 && cm.NSize == 16 && cm.KSize == 16 &&
            cm.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && cm.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            cm.CType == VK_COMPONENT_TYPE_FLOAT32_KHR)
            ok816 = true;
    if (!ok816) { std::fprintf(stderr, "[FAIL] 8x16x16 f16xf16->f32 coopmat config missing\n"); return false; }
    std::printf("[VK] coopmat 8x16x16 f16xf16->f32: OK\n");
    return true;
}

bool VkContext::createDevice(const VkContextConfig& cfg) {
    const char* wantNoSwap[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    const char* wantSwap[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = cfg.needSwapchain ? 2u : 1u;
    dci.ppEnabledExtensionNames = cfg.needSwapchain ? wantSwap : wantNoSwap;
    // m8 shaders declare Float16/Int64/16-bit storage/memory-model/BDA/coopmat —
    // features must be enabled or the device faults (Arc device-lost).
    VkPhysicalDeviceFeatures fe{};
    fe.shaderInt64 = VK_TRUE;
    fe.shaderInt16 = VK_TRUE;
    // Arc driver: merely ENABLING the sparseBinding device feature silently
    // corrupts large DENSE allocations (M10: head meandiff 0.7-1.9 vs 0.008
    // expected, bench looks normal, featV stays golden). Enable the feature
    // only for the D5C_SPARSE=1 experiment; the dense default must create
    // the device without it.
    bool wantSparseFeat = false;
    if (const char* s = std::getenv("D5C_SPARSE")) wantSparseFeat = std::atoi(s) != 0;
    fe.sparseBinding = (vk.sparseBinding && wantSparseFeat) ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.shaderFloat16 = VK_TRUE;
    f12.vulkanMemoryModel = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    fcm.cooperativeMatrix = VK_TRUE;
    dci.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &fcm;
    dci.pEnabledFeatures = &fe;
    D5C_VK_RET(vkCreateDevice(vk.pd, &dci, nullptr, &vk.dev));
    vkGetDeviceQueue(vk.dev, vk.qf, 0, &vk.queue);
    std::printf("[VK] logical device up (compute+graphics family %u%s)\n", vk.qf,
                cfg.needSwapchain ? ", swapchain + coopmat" : ", coopmat (headless)");
    return true;
}

void VkContext::destroy() {
    if (vk.dev) vkDestroyDevice(vk.dev, nullptr);
    if (vk.inst) vkDestroyInstance(vk.inst, nullptr);
    vk = VkCtx{};
}

} // namespace d5c
