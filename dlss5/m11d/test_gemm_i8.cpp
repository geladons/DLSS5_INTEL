// ============================================================================
// dlss5/m11d/test_gemm_i8.cpp - Prototype and Benchmark INT8 Cooperative Matrix GEMM
// Target: Intel Arc Pro B50 (Battlemage Xe2, Vulkan 1.3 / 1.4)
//
// Tests coopmat 8x16x32 A=i8 B=i8 C=i32 sat=0 scope=3 against CPU reference,
// benchmarks shapes (552960, 128, 32) and (138240, 128, 32),
// and compares directly against the FP16 baseline.
// ============================================================================

#include <vulkan/vulkan.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define VK_CHECK(x)                                                            \
    do {                                                                       \
        VkResult r_ = (x);                                                     \
        if (r_ != VK_SUCCESS) {                                                \
            std::fprintf(stderr, "[FAIL] Vulkan call %s returned %d at %d\n",   \
                         #x, (int)r_, __LINE__);                               \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// ---- FP16 conversion helpers -----------------------------------------------
static uint16_t f32_to_f16(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3FFu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | mant);
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    if (e == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        int exp = -1;
        while (!(mant & 0x400u)) { mant <<= 1; --exp; }
        mant &= 0x3FFu;
        uint32_t f = sign | ((uint32_t)(114 + exp) << 23) | (mant << 13);
        float out; std::memcpy(&out, &f, 4); return out;
    } else if (e == 31) {
        uint32_t f = sign | 0x7F800000u | (mant << 13);
        float out; std::memcpy(&out, &f, 4); return out;
    }
    uint32_t f = sign | ((e - 15 + 127) << 23) | (mant << 13);
    float out; std::memcpy(&out, &f, 4); return out;
}

// Push constant structs matching shaders
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

struct PushGemmF16 {
    uint64_t a, b, c;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
    uint32_t pad;
    uint64_t res;
    uint64_t skip;
};
#pragma pack(pop)

struct VulkanBenchContext {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qf = 0;
    float timestampPeriodNs = 1.0f;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    uint32_t hostMemType = UINT32_MAX;
    uint32_t deviceMemType = UINT32_MAX;
    PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHR_fn = nullptr;

    void init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "test_gemm_i8";
        app.apiVersion = VK_API_VERSION_1_3;

        const char* instExts[] = {"VK_KHR_get_physical_device_properties2"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = instExts;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

        uint32_t npd = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, nullptr));
        std::vector<VkPhysicalDevice> pds(npd);
        VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds.data()));

        for (auto p : pds) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(p, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || std::strstr(props.deviceName, "Arc")) {
                pd = p;
                std::printf("[VK] Selected GPU: %s (driver %u.%u.%u)\n",
                            props.deviceName,
                            VK_API_VERSION_MAJOR(props.driverVersion),
                            VK_API_VERSION_MINOR(props.driverVersion),
                            VK_API_VERSION_PATCH(props.driverVersion));
                timestampPeriodNs = props.limits.timestampPeriod;
                std::printf("[VK] Timestamp period: %.3f ns\n", timestampPeriodNs);
                break;
            }
        }
        if (!pd) pd = pds[0];

        // Enumerate coopmat configs
        auto fpEnum = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
            vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
        if (fpEnum) {
            uint32_t ncfg = 0;
            fpEnum(pd, &ncfg, nullptr);
            std::vector<VkCooperativeMatrixPropertiesKHR> cfgs(ncfg,
                {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, nullptr});
            fpEnum(pd, &ncfg, cfgs.data());
            std::printf("[VK] Enumerated %u cooperative matrix configs:\n", ncfg);
            for (uint32_t i = 0; i < ncfg; ++i) {
                std::printf("  [%u] %ux%ux%u A=%d B=%d C=%d sat=%d scope=%u\n",
                            i, cfgs[i].MSize, cfgs[i].NSize, cfgs[i].KSize,
                            cfgs[i].AType, cfgs[i].BType, cfgs[i].CType,
                            (int)cfgs[i].saturatingAccumulation, cfgs[i].scope);
            }
        }

        // Enable required features
        const char* devExts[] = {
            VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
        };

        VkPhysicalDeviceFeatures fe{};
        fe.shaderInt16 = VK_TRUE;

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

        f11.pNext = &f12;
        f12.pNext = &fcm;

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f11;
        dci.pEnabledFeatures = &fe;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 2;
        dci.ppEnabledExtensionNames = devExts;
        VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qf, 0, &queue);

        vkGetBufferDeviceAddressKHR_fn = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddressKHR");

        // Find memory types
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
        std::printf("[VK] Memory types (%u):\n", memProps.memoryTypeCount);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            uint32_t f = memProps.memoryTypes[i].propertyFlags;
            std::printf("  [%u] heap %u flags: %s%s%s (0x%x)\n", i,
                        memProps.memoryTypes[i].heapIndex,
                        (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
                        (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
                        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
                        f);
            if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                if (hostMemType == UINT32_MAX) hostMemType = i;
            }
            if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
                if (deviceMemType == UINT32_MAX) deviceMemType = i;
            }
        }
        if (deviceMemType == UINT32_MAX) {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                    deviceMemType = i; break;
                }
            }
        }
        std::printf("[VK] Using hostMemType=%u, deviceMemType=%u\n", hostMemType, deviceMemType);

        // Command pool
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = qf;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cmdPool));

        // Query pool for GPU timestamp measurement
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 64;
        VK_CHECK(vkCreateQueryPool(dev, &qpci, nullptr, &queryPool));
    }

    struct Buffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint64_t bda = 0;
        size_t size = 0;
        void* hostPtr = nullptr;
    };

    Buffer createBuffer(size_t size, bool hostVisible) {
        Buffer b;
        b.size = size;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &b.buf));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b.buf, &req);

        VkMemoryAllocateFlagsInfo afi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        afi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &afi};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = hostVisible ? hostMemType : deviceMemType;
        VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &b.mem));
        VK_CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));

        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = b.buf;
        b.bda = vkGetBufferDeviceAddressKHR_fn(dev, &bai);

        if (hostVisible) {
            VK_CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.hostPtr));
        }
        return b;
    }

    void destroyBuffer(Buffer& b) {
        if (b.hostPtr) {
            vkUnmapMemory(dev, b.mem);
            b.hostPtr = nullptr;
        }
        if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
        if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
        b = Buffer{};
    }

    VkPipeline createPipeline(const std::string& spvPath, uint32_t pushSize, VkPipelineLayout& outLayout) {
        std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
        if (!f) {
            std::fprintf(stderr, "[FAIL] Cannot open shader %s\n", spvPath.c_str());
            std::exit(1);
        }
        size_t sz = f.tellg();
        f.seekg(0);
        std::vector<char> spv(sz);
        f.read(spv.data(), sz);

        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = spv.size();
        smci.pCode = (const uint32_t*)spv.data();
        VkShaderModule sm;
        VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &outLayout));

        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = sm;
        cpci.stage.pName = "main";
        cpci.layout = outLayout;
        VkPipeline pipe;
        VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
        vkDestroyShaderModule(dev, sm, nullptr);
        return pipe;
    }

    void shutdown() {
        if (queryPool) vkDestroyQueryPool(dev, queryPool, nullptr);
        if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
        if (dev) vkDestroyDevice(dev, nullptr);
        if (inst) vkDestroyInstance(inst, nullptr);
    }
};

static void runCorrectnessCheck(VulkanBenchContext& ctx) {
    std::printf("\n=== Phase 1: INT8 GEMM Numerical Verification ===\n");
    VkPipelineLayout play;
    VkPipeline pipe = ctx.createPipeline("dlss5/m8b-live/shaders/m8/gemm_i8.spv", sizeof(PushGemmI8), play);

    // Test shape M=64, N=128, K=32 (covers multiple RM=4 and RN=2 tiles)
    const uint32_t M = 64, N = 128, K = 32;
    auto bA = ctx.createBuffer(M * K * sizeof(int8_t), true);
    auto bB = ctx.createBuffer(K * N * sizeof(int8_t), true);
    auto bC = ctx.createBuffer(M * N * sizeof(uint16_t), true); // FP16 out
    auto bScale = ctx.createBuffer(N * sizeof(float), true);

    int8_t* pA = (int8_t*)bA.hostPtr;
    int8_t* pB = (int8_t*)bB.hostPtr;
    uint16_t* pC = (uint16_t*)bC.hostPtr;
    float* pScale = (float*)bScale.hostPtr;

    for (uint32_t i = 0; i < M * K; ++i) pA[i] = (int8_t)((int(i * 3 + 7) % 25) - 12);
    for (uint32_t i = 0; i < K * N; ++i) pB[i] = (int8_t)((int(i * 5 + 3) % 25) - 12);
    for (uint32_t j = 0; j < N; ++j) pScale[j] = 0.05f + 0.01f * (j % 8);
    float scaleA = 0.025f;

    // CPU Reference computation
    std::vector<float> cpuRef(M * N);
    for (uint32_t r = 0; r < M; ++r) {
        for (uint32_t c = 0; c < N; ++c) {
            int32_t sum = 0;
            for (uint32_t k = 0; k < K; ++k) {
                sum += int32_t(pA[r * K + k]) * int32_t(pB[k * N + c]);
            }
            cpuRef[r * N + c] = float(sum) * scaleA * pScale[c];
        }
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = ctx.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(ctx.dev, &cbai, &cb));

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

    PushGemmI8 pc{};
    pc.a = bA.bda;
    pc.b = bB.bda;
    pc.c = bC.bda;
    pc.b_scale = bScale.bda;
    pc.scale_a = scaleA;
    pc.m = M; pc.n = N; pc.k = K; pc.batch = 1;
    pc.lda = K; pc.ldb = N; pc.ldc = N;
    pc.flags = 0;

    vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, N / 32, (M + 31) / 32, 1);
    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    // Verify GPU results vs CPU reference
    float maxErr = 0.0f;
    uint32_t errCount = 0;
    for (uint32_t i = 0; i < M * N; ++i) {
        float gpuVal = f16_to_f32(pC[i]);
        float refVal = cpuRef[i];
        float diff = std::abs(gpuVal - refVal);
        if (diff > maxErr) maxErr = diff;
        // In FP16, quantization error between float multiplication and fp16 truncation is <= 1 ULP (~0.001)
        if (diff > 0.02f) {
            if (errCount < 5) {
                uint32_t r = i / N, c = i % N;
                std::printf("  MISMATCH at (%u, %u): CPU=%.5f GPU=%.5f diff=%.5f\n",
                            r, c, refVal, gpuVal, diff);
            }
            errCount++;
        }
    }

    if (errCount == 0) {
        std::printf("[PASS] INT8 GEMM (M=%u, N=%u, K=%u): 0 mismatches across %u elements, max err=%.6f\n",
                    M, N, K, M * N, maxErr);
    } else {
        std::printf("[FAIL] INT8 GEMM: %u mismatches (max err %.6f)\n", errCount, maxErr);
    }

    // Now test with transposed B (flags bit 0 = 1)
    std::printf("Testing B-transposed mode (flags=1)...\n");
    // In transposed mode, B is stored as row-major (N, K): element (c, k) at c * K + k
    for (uint32_t c = 0; c < N; ++c) {
        for (uint32_t k = 0; k < K; ++k) {
            pB[c * K + k] = (int8_t)((int(c * 5 + k * 3) % 25) - 12);
        }
    }
    for (uint32_t r = 0; r < M; ++r) {
        for (uint32_t c = 0; c < N; ++c) {
            int32_t sum = 0;
            for (uint32_t k = 0; k < K; ++k) {
                sum += int32_t(pA[r * K + k]) * int32_t(pB[c * K + k]);
            }
            cpuRef[r * N + c] = float(sum) * scaleA * pScale[c];
        }
    }

    VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    pc.flags = 1;
    pc.ldb = K;
    vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, N / 32, (M + 31) / 32, 1);
    VK_CHECK(vkEndCommandBuffer(cb));

    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    errCount = 0; maxErr = 0.0f;
    for (uint32_t i = 0; i < M * N; ++i) {
        float gpuVal = f16_to_f32(pC[i]);
        float refVal = cpuRef[i];
        float diff = std::abs(gpuVal - refVal);
        if (diff > maxErr) maxErr = diff;
        if (diff > 0.02f) {
            if (errCount < 5) {
                uint32_t r = i / N, c = i % N;
                std::printf("  MISMATCH (transposed) at (%u, %u): CPU=%.5f GPU=%.5f diff=%.5f\n",
                            r, c, refVal, gpuVal, diff);
            }
            errCount++;
        }
    }

    if (errCount == 0) {
        std::printf("[PASS] INT8 GEMM Transposed B: 0 mismatches across %u elements, max err=%.6f\n",
                    M * N, maxErr);
    } else {
        std::printf("[FAIL] INT8 GEMM Transposed B: %u mismatches (max err %.6f)\n", errCount, maxErr);
    }

    vkFreeCommandBuffers(ctx.dev, ctx.cmdPool, 1, &cb);
    vkDestroyPipeline(ctx.dev, pipe, nullptr);
    vkDestroyPipelineLayout(ctx.dev, play, nullptr);
    ctx.destroyBuffer(bA);
    ctx.destroyBuffer(bB);
    ctx.destroyBuffer(bC);
    ctx.destroyBuffer(bScale);
}

struct BenchResult {
    double minUs = 0;
    double medianUs = 0;
    double maxUs = 0;
    double tflops = 0;
};

static BenchResult benchmarkShape(VulkanBenchContext& ctx,
                                  VkPipeline pipe, VkPipelineLayout play,
                                  bool isInt8,
                                  uint32_t M, uint32_t N, uint32_t K,
                                  int iterations = 15) {
    // Allocate device buffers
    size_t szA = isInt8 ? (size_t)M * K * 1 : (size_t)M * K * 2;
    size_t szB = isInt8 ? (size_t)K * N * 1 : (size_t)K * N * 2;
    size_t szC = (size_t)M * N * 2; // always narrow FP16 out
    size_t szScale = (size_t)N * sizeof(float);

    // Allocate pure DEVICE_LOCAL VRAM buffers
    auto bA = ctx.createBuffer(szA, false);
    auto bB = ctx.createBuffer(szB, false);
    auto bC = ctx.createBuffer(szC, false);
    auto bScale = isInt8 ? ctx.createBuffer(szScale, false) : VulkanBenchContext::Buffer{};

    // Host staging buffers to initialize device-local memory
    auto stA = ctx.createBuffer(szA, true);
    auto stB = ctx.createBuffer(szB, true);
    auto stScale = isInt8 ? ctx.createBuffer(szScale, true) : VulkanBenchContext::Buffer{};

    if (isInt8) {
        std::memset(stA.hostPtr, 1, szA);
        std::memset(stB.hostPtr, 1, szB);
        float* ps = (float*)stScale.hostPtr;
        for (uint32_t i = 0; i < N; ++i) ps[i] = 1.0f;
    } else {
        uint16_t* pa = (uint16_t*)stA.hostPtr;
        uint16_t* pb = (uint16_t*)stB.hostPtr;
        for (size_t i = 0; i < szA / 2; ++i) pa[i] = f32_to_f16(0.5f);
        for (size_t i = 0; i < szB / 2; ++i) pb[i] = f32_to_f16(0.5f);
    }

    // Copy staging -> device-local
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = ctx.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(ctx.dev, &cbai, &cb));

    VkCommandBufferBeginInfo cbbiCopy{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &cbbiCopy));
    VkBufferCopy cpyA{0, 0, szA};
    vkCmdCopyBuffer(cb, stA.buf, bA.buf, 1, &cpyA);
    VkBufferCopy cpyB{0, 0, szB};
    vkCmdCopyBuffer(cb, stB.buf, bB.buf, 1, &cpyB);
    if (isInt8) {
        VkBufferCopy cpyS{0, 0, szScale};
        vkCmdCopyBuffer(cb, stScale.buf, bScale.buf, 1, &cpyS);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo siCopy{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    siCopy.commandBufferCount = 1;
    siCopy.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &siCopy, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    // Free staging buffers
    ctx.destroyBuffer(stA);
    ctx.destroyBuffer(stB);
    if (isInt8) ctx.destroyBuffer(stScale);

    PushGemmI8 pcI8{};
    if (isInt8) {
        pcI8.a = bA.bda;
        pcI8.b = bB.bda;
        pcI8.c = bC.bda;
        pcI8.b_scale = bScale.bda;
        pcI8.scale_a = 1.0f;
        pcI8.m = M; pcI8.n = N; pcI8.k = K; pcI8.batch = 1;
        pcI8.lda = K; pcI8.ldb = N; pcI8.ldc = N;
        pcI8.flags = 0;
    }

    PushGemmF16 pcF16{};
    if (!isInt8) {
        pcF16.a = bA.bda;
        pcF16.b = bB.bda;
        pcF16.c = bC.bda;
        pcF16.m = M; pcF16.n = N; pcF16.k = K; pcF16.batch = 1;
        pcF16.flags = 0x1000; // narrow f16 out
        pcF16.lda = K; pcF16.ldb = N; pcF16.ldc = N;
    }

    // Warmup
    for (int w = 0; w < 3; ++w) {
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        if (isInt8) {
            vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcI8), &pcI8);
        } else {
            vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcF16), &pcF16);
        }
        vkCmdDispatch(cb, N / 32, (M + 31) / 32, 1);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.queue));
    }

    std::vector<double> timingsUs;
    timingsUs.reserve(iterations);

    for (int iter = 0; iter < iterations; ++iter) {
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));
        vkCmdResetQueryPool(cb, ctx.queryPool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx.queryPool, 0);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        if (isInt8) {
            vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcI8), &pcI8);
        } else {
            vkCmdPushConstants(cb, play, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcF16), &pcF16);
        }
        vkCmdDispatch(cb, N / 32, (M + 31) / 32, 1);

        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.queryPool, 1);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.queue));

        uint64_t ts[2]{0, 0};
        VK_CHECK(vkGetQueryPoolResults(ctx.dev, ctx.queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double deltaNs = double(ts[1] - ts[0]) * ctx.timestampPeriodNs;
        timingsUs.push_back(deltaNs / 1000.0);
    }

    std::sort(timingsUs.begin(), timingsUs.end());
    BenchResult res;
    res.minUs = timingsUs.front();
    res.medianUs = timingsUs[timingsUs.size() / 2];
    res.maxUs = timingsUs.back();
    double ops = 2.0 * double(M) * double(N) * double(K);
    res.tflops = (ops / (res.medianUs * 1e-6)) / 1e12;

    vkFreeCommandBuffers(ctx.dev, ctx.cmdPool, 1, &cb);
    ctx.destroyBuffer(bA);
    ctx.destroyBuffer(bB);
    ctx.destroyBuffer(bC);
    if (isInt8) ctx.destroyBuffer(bScale);

    return res;
}

int main() {
    std::printf("=================================================================\n");
    std::printf("  Intel Arc Pro B50 INT8 Cooperative Matrix GEMM Benchmark\n");
    std::printf("=================================================================\n");

    VulkanBenchContext ctx;
    ctx.init();

    // 1. Correctness verification
    runCorrectnessCheck(ctx);

    // 2. Load pipelines
    std::printf("\n=== Phase 2: Compiling & Loading GEMM Pipelines ===\n");
    VkPipelineLayout playI8, playF16;
    VkPipeline pipeI8 = ctx.createPipeline("dlss5/m8b-live/shaders/m8/gemm_i8.spv", sizeof(PushGemmI8), playI8);
    VkPipeline pipeF16 = ctx.createPipeline("dlss5/m8b-live/build/Release/gemm.spv", sizeof(PushGemmF16), playF16);
    std::printf("[VK] INT8 pipeline:  gemm_i8.spv (TK=32, A=i8 B=i8 C=i32 -> dequant f16)\n");
    std::printf("[VK] FP16 pipeline: gemm.spv    (TK=16, A=f16 B=f16 C=f16/f32 narrow out)\n");

    // Shapes to benchmark
    struct Shape { uint32_t m, n, k; const char* name; };
    Shape shapes[] = {
        {552960, 128, 32, "L0 (552960, 128, 32)"},
        {138240, 128, 32, "L1 (138240, 128, 32)"}
    };

    std::printf("\n=== Phase 3: Benchmarking Pure GPU Execution Time ===\n");

    for (const auto& sh : shapes) {
        std::printf("\n-----------------------------------------------------------------\n");
        std::printf(" Shape: %s | Total ops: %.2f MFLOP\n",
                    sh.name, (2.0 * sh.m * sh.n * sh.k) / 1e6);
        std::printf("-----------------------------------------------------------------\n");

        std::printf("  Running FP16 Baseline...\n");
        auto resF16 = benchmarkShape(ctx, pipeF16, playF16, false, sh.m, sh.n, sh.k, 15);
        std::printf("    FP16: min=%.2f us, MEDIAN=%.2f us (%.3f ms), max=%.2f us | Throughput: %.2f TFLOPS\n",
                    resF16.minUs, resF16.medianUs, resF16.medianUs / 1000.0, resF16.maxUs, resF16.tflops);

        std::printf("  Running INT8 CoopMat (TK=32, 1 tile step)...\n");
        auto resI8 = benchmarkShape(ctx, pipeI8, playI8, true, sh.m, sh.n, sh.k, 15);
        std::printf("    INT8: min=%.2f us, MEDIAN=%.2f us (%.3f ms), max=%.2f us | Throughput: %.2f TOPS\n",
                    resI8.minUs, resI8.medianUs, resI8.medianUs / 1000.0, resI8.maxUs, resI8.tflops);

        double speedup = resF16.medianUs / resI8.medianUs;
        double timeSavedMs = (resF16.medianUs - resI8.medianUs) / 1000.0;
        double pctSaved = ((resF16.medianUs - resI8.medianUs) / resF16.medianUs) * 100.0;
        std::printf("  ==> SPEEDUP: %.2fx (%.2f%% faster, -%.3f ms saved per dispatch)\n",
                    speedup, pctSaved, timeSavedMs);
    }

    std::printf("\n=================================================================\n");
    std::printf("  Benchmark Complete\n");
    std::printf("=================================================================\n");

    vkDestroyPipeline(ctx.dev, pipeI8, nullptr);
    vkDestroyPipelineLayout(ctx.dev, playI8, nullptr);
    vkDestroyPipeline(ctx.dev, pipeF16, nullptr);
    vkDestroyPipelineLayout(ctx.dev, playF16, nullptr);
    ctx.shutdown();
    return 0;
}
