// ============================================================================
// test_pilot0.cpp - Debug Block 0 INT8 Quantization and GEMM
// ============================================================================
#include <vulkan/vulkan.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "VK FAIL %d at %d\n", (int)r_, __LINE__); std::exit(1); } } while(0)

#pragma pack(push, 1)
struct PushGemmI8 {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t b_scale;
    uint64_t a_scale;
    float scale_a;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
    uint32_t pad;
};

struct PushQuantAct {
    uint64_t a;
    uint64_t aq;
    uint64_t scales;
    uint32_t m, k;
};
#pragma pack(pop)

int main() {
    std::printf("=== test_pilot0: debugging Block 0 INT8 pipeline ===\n");

    // Initialize Vulkan
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "test_pilot0";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstance inst;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, nullptr);
    std::vector<VkPhysicalDevice> pds(npd);
    vkEnumeratePhysicalDevices(inst, &npd, pds.data());
    VkPhysicalDevice pd = pds[0];

    const char* devExts[] = {
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
    };
    VkPhysicalDeviceFeatures fe{};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.shaderFloat16 = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.vulkanMemoryModel = VK_TRUE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    fcm.cooperativeMatrix = VK_TRUE;
    f11.pNext = &f12; f12.pNext = &fcm;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f11; dci.pEnabledFeatures = &fe;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = devExts;
    VkDevice dev;
    VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    auto getBda = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddressKHR");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t hostMemType = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hostMemType = i; break;
        }
    }

    auto makeBuf = [&](size_t sz) {
        VkBuffer b; VkDeviceMemory m; void* ptr; uint64_t bda;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = (sz + 255) & ~255ull;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &b));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, b, &req);
        VkMemoryAllocateFlagsInfo afi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        afi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &afi};
        mai.allocationSize = req.size; mai.memoryTypeIndex = hostMemType;
        VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &m));
        VK_CHECK(vkBindBufferMemory(dev, b, m, 0));
        VK_CHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &ptr));
        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = b;
        bda = getBda(dev, &bai);
        return std::make_tuple(b, m, ptr, bda);
    };

    auto makePipe = [&](const char* path, uint32_t pushSz, VkPipelineLayout& play) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
        size_t sz = f.tellg(); f.seekg(0);
        std::vector<char> spv(sz); f.read(spv.data(), sz);
        VkShaderModule sm;
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sz; smci.pCode = (const uint32_t*)spv.data();
        VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSz};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &play));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = sm; cpci.stage.pName = "main";
        cpci.layout = play;
        VkPipeline p; VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &p));
        vkDestroyShaderModule(dev, sm, nullptr);
        return p;
    };

    VkPipelineLayout playQ, playG;
    VkPipeline pipeQ = makePipe("dlss5/m8b-live/build/Release/quant_act.spv", sizeof(PushQuantAct), playQ);
    VkPipeline pipeG = makePipe("dlss5/m8b-live/build/Release/gemm_i8.spv", sizeof(PushGemmI8), playG);

    // Block 0 shape: M = 1088 * 1344 = 1462272 tokens, C = 32, 4*C = 128
    const uint32_t M = 1462272, K = 32, N = 128;
    const uint32_t numBlocks = (M + 31) / 32;

    auto [bX16, mX16, pX16, aX16] = makeBuf((size_t)M * K * 2);
    auto [bQAct, mQAct, pQAct, aQAct] = makeBuf((size_t)M * K * 1);
    auto [bQScale, mQScale, pQScale, aQScale] = makeBuf((size_t)numBlocks * 4);
    auto [bW, mW, pW, aW] = makeBuf((size_t)K * N * 1);
    auto [bWScale, mWScale, pWScale, aWScale] = makeBuf((size_t)N * 4);
    auto [bOut, mOut, pOut, aOut] = makeBuf((size_t)M * N * 2);

    // Initialize inputs: simple test patterns
    uint16_t* x16 = (uint16_t*)pX16;
    for (size_t i = 0; i < (size_t)M * K; ++i) {
        // float between -2.0 and +2.0
        float val = ((float)(i % 100) - 50.0f) / 25.0f;
        uint32_t x; std::memcpy(&x, &val, 4);
        uint32_t sign = (x >> 16) & 0x8000u;
        int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
        uint32_t mant = (x >> 13) & 0x3FFu;
        x16[i] = (exp <= 0) ? (uint16_t)sign : (uint16_t)(sign | (exp << 10) | mant);
    }
    // Load real weights and scales from work/mlxw/dlssnr-int8.bin
    // From inspect_bin: block0.layer0.weight1 has data_off=14848, w_bytes=4096, scale_off=4096
    std::ifstream fw("work/mlxw/dlssnr-int8.bin", std::ios::binary);
    if (!fw) { std::fprintf(stderr, "cannot open dlssnr-int8.bin\n"); return 1; }
    fw.seekg(124672 + 14848); // data_base (124672) + data_off (14848)
    fw.read((char*)pW, 4096);
    fw.seekg(124672 + 14848 + 4096); // scales
    fw.read((char*)pWScale, 128 * sizeof(float));
    std::printf("Real weight1 loaded: w[0]=%d, scale[0]=%.6f, scale[1]=%.6f\n",
                ((int8_t*)pW)[0], ((float*)pWScale)[0], ((float*)pWScale)[1]);

    VkCommandPool cp;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cp; cbai.commandBufferCount = 1;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));

    // 1. Quantize
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeQ);
    PushQuantAct pq{aX16, aQAct, aQScale, M, K};
    vkCmdPushConstants(cb, playQ, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pq), &pq);
    vkCmdDispatch(cb, numBlocks, 1, 1);

    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    // 2. GEMM I8 with EPI_GATE_E4M3
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeG);
    uint32_t flags = (3u << 8) | 0x1000u; // EPI_GATE_E4M3 | narrow
    PushGemmI8 pg{aQAct, aW, aOut, aWScale, aQScale, 0.0f, M, N, K, 1, 0, 0, 0, flags, K, N, N, 0};
    vkCmdPushConstants(cb, playG, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pg), &pg);
    vkCmdDispatch(cb, N / 32, (M + 31) / 32, 1);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(q));

    // Inspect results
    float* scales = (float*)pQScale;
    std::printf("Scales[0..4]: %.6f, %.6f, %.6f, %.6f\n", scales[0], scales[1], scales[2], scales[3]);
    uint32_t nanScales = 0;
    for (uint32_t b = 0; b < numBlocks; ++b) {
        if (std::isnan(scales[b]) || std::isinf(scales[b])) nanScales++;
    }
    std::printf("NaN/Inf in scales: %u / %u\n", nanScales, numBlocks);

    int8_t* aq = (int8_t*)pQAct;
    int minAq = 127, maxAq = -128;
    for (size_t i = 0; i < (size_t)M * K; ++i) {
        if (aq[i] < minAq) minAq = aq[i];
        if (aq[i] > maxAq) maxAq = aq[i];
    }
    std::printf("Quantized A range: [%d, %d]\n", minAq, maxAq);

    uint16_t* out = (uint16_t*)pOut;
    uint32_t nanOut = 0, infOut = 0;
    for (size_t i = 0; i < (size_t)M * N; ++i) {
        uint32_t exp = (out[i] >> 10) & 0x1Fu;
        uint32_t mant = out[i] & 0x3FFu;
        if (exp == 31) {
            if (mant == 0) infOut++; else nanOut++;
        }
    }
    std::printf("Output (M=%u, N=%u): NaN=%u, Inf=%u across %zu elements\n", M, N, nanOut, infOut, (size_t)M * N);

    return 0;
}
