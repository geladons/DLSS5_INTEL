#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <dxgi.h>

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define VK_CHECK(x)                                                            \
    do {                                                                       \
        VkResult res_ = (x);                                                   \
        if (res_ != VK_SUCCESS) {                                              \
            std::fprintf(stderr, "VK error %d at line %d: %s\n", (int)res_,    \
                         __LINE__, #x);                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

using clk = std::chrono::steady_clock;
static double msSince(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    auto pos = s.find_last_of('\\');
    if (pos != std::string::npos) s.resize(pos + 1);
    return s;
}

static int readFile(const std::string &path, std::vector<char> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 1;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (!f.read(out.data(), sz)) return 1;
    return 0;
}

// ----------------------------------------------------------- f16 <-> f32 ---
static bool g_hasF16C = false;

static uint16_t f32_to_f16_soft(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x7FFFFFu;
    int e = (int)((x >> 23) & 0xFFu);
    if (e == 255) {
        if (mant == 0) return (uint16_t)(sign | 0x7C00u);
        return (uint16_t)(sign | 0x7C00u | (uint16_t)(mant >> 13) | 1u);
    }
    int hexp = e - 112;
    if (hexp >= 31) return (uint16_t)(sign | 0x7C00u);
    if (hexp <= 0) {
        if (hexp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        int shift = 14 - hexp;
        uint32_t m = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1u);
        uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (m & 1u))) {
            m++;
            if (m == 0x400u) return (uint16_t)(sign | 0x400u);
        }
        return (uint16_t)(sign | m);
    }
    uint32_t m = mant >> 13;
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u))) {
        m++;
        if (m == 0x400u) { m = 0; hexp++; if (hexp >= 31) return (uint16_t)(sign | 0x7C00u); }
    }
    return (uint16_t)(sign | ((uint32_t)hexp << 10) | m);
}

static uint16_t f32_to_f16(float f) {
    if (g_hasF16C) {
        __m128 v = _mm_set_ss(f);
        __m128i h = _mm_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT);
        return (uint16_t)_mm_extract_epi16(h, 0);
    }
    return f32_to_f16_soft(f);
}

static float f16_bits_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t b;
    if (e == 0) {
        if (mant == 0) b = sign;
        else {
            int exp = -1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            b = sign | ((uint32_t)(114 + exp) << 23) | (mant << 13);
        }
    } else if (e == 31) {
        b = sign | 0x7F800000u | (mant << 13);
    } else {
        b = sign | ((e - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &b, 4);
    return out;
}

static float round_even_cpu(float v) {
    float fl = std::floor(v);
    float diff = v - fl;
    if (diff > 0.5f) return fl + 1.0f;
    if (diff < 0.5f) return fl;
    return (std::fmod(fl, 2.0f) == 0.0f) ? fl : fl + 1.0f;
}

static float e4m3_cpu(float x) {  // publish.glsl
    float magnitude = std::fmin(std::fabs(x), 448.0f);
    uint32_t bits;
    std::memcpy(&bits, &magnitude, 4);
    int exponent = (int)((bits >> 23) & 0xFFu);
    exponent = std::max(exponent, 121) - 3;
    float step, reciprocal;
    uint32_t sb = (uint32_t)exponent << 23;
    uint32_t rb = (uint32_t)(254 - exponent) << 23;
    std::memcpy(&step, &sb, 4);
    std::memcpy(&reciprocal, &rb, 4);
    float rounded = round_even_cpu(magnitude * reciprocal) * step;
    return x < 0.0f ? -rounded : rounded;
}

static float gate_cpu(float x) {
    float wide = f16_bits_to_f32(f32_to_f16(x));
    float clamped = std::clamp(wide, -4.0f, 4.0f);
    float linear = std::fabs(clamped) * -0.055908203125f;
    linear += 0.447265625f;
    linear = f16_bits_to_f32(f32_to_f16(linear));
    linear *= clamped;
    linear += 0.89453125f;
    linear = f16_bits_to_f32(f32_to_f16(linear));
    return f16_bits_to_f32(f32_to_f16(wide * linear));
}

// ------------------------------------------------------- safetensors -------
struct Tensor {
    std::string dtype;
    std::vector<int64_t> shape;
    uint64_t off0 = 0, off1 = 0;
    uint64_t numel() const {
        uint64_t n = 1;
        for (auto s : shape) n *= (uint64_t)s;
        return n;
    }
};

struct WeightsFile {
    std::vector<char> bytes;
    size_t dataBase = 0;
    std::map<std::string, Tensor> tensors;
};

static void stParse(const std::string &path, WeightsFile &wf) {
    if (readFile(path, wf.bytes)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
    if (wf.bytes.size() < 8) { std::fprintf(stderr, "safetensors too small\n"); std::exit(1); }
    uint64_t hlen;
    std::memcpy(&hlen, wf.bytes.data(), 8);
    if (8 + hlen > wf.bytes.size()) { std::fprintf(stderr, "bad header len\n"); std::exit(1); }
    const char *p = wf.bytes.data() + 8;
    const char *end = p + hlen;
    auto ws = [&]() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; };
    auto expect = [&](char c) { ws(); if (p >= end || *p++ != c) { std::fprintf(stderr, "JSON: expected %c\n", c); std::exit(1); } };
    auto string = [&]() -> std::string {
        ws(); expect('"');
        std::string s;
        while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) ++p; s += *p++; }
        expect('"');
        return s;
    };
    auto integer = [&]() -> int64_t {
        ws(); bool neg = false;
        if (p < end && *p == '-') { neg = true; ++p; }
        int64_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    };
    ws(); expect('{');
    while (true) {
        ws();
        if (p < end && *p == '}') { ++p; break; }
        std::string key = string();
        ws(); expect(':');
        if (key == "__metadata__") {
            int depth = 0;
            ws(); expect('{'); depth = 1;
            while (p < end && depth) { char c = *p++; if (c == '{') ++depth; else if (c == '}') --depth; }
        } else {
            Tensor t;
            ws(); expect('{');
            while (true) {
                ws();
                if (*p == '}') { ++p; break; }
                std::string k = string();
                ws(); expect(':');
                if (k == "dtype") t.dtype = string();
                else if (k == "shape") {
                    ws(); expect('[');
                    while (true) { ws(); if (*p == ']') { ++p; break; } t.shape.push_back(integer()); ws(); if (*p == ',') ++p; }
                } else if (k == "data_offsets") {
                    ws(); expect('[');
                    ws(); t.off0 = (uint64_t)integer(); ws(); expect(','); ws(); t.off1 = (uint64_t)integer();
                    ws(); expect(']');
                } else {
                    ws();
                    if (*p == '"') string();
                    else while (p < end && *p != ',' && *p != '}') ++p;
                }
                ws(); if (*p == ',') ++p;
            }
            wf.tensors[key] = t;
        }
        ws(); if (p < end && *p == ',') ++p;
    }
    wf.dataBase = 8 + hlen;
    std::printf("safetensors: %zu tensors, data base %zu\n", wf.tensors.size(), wf.dataBase);
}

// ------------------------------------------------------------- Vulkan ------
struct VkCtx {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    uint32_t qf;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool cmdPool;
    uint32_t timestampBits = 0;
    float tsPeriod = 1.0f;
};

static void vkInit(VkCtx &vk) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "m8-full-chain";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &vk.inst));

    uint32_t npd = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(vk.inst, &npd, nullptr));
    std::vector<VkPhysicalDevice> pds(npd);
    VK_CHECK(vkEnumeratePhysicalDevices(vk.inst, &npd, pds.data()));

    VkDeviceSize bestHeap = 0;
    for (auto d : pds) {
        uint32_t next = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &next, nullptr);
        std::vector<VkExtensionProperties> exts(next);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &next, exts.data());
        bool hasCoop = false;
        for (auto &e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0) hasCoop = true;
        if (!hasCoop) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qps.data());
        uint32_t q = 0;
        for (; q < nq; ++q) if (qps[q].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
        if (q == nq) continue;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(d, &mp);
        VkDeviceSize heap = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap += mp.memoryHeaps[i].size;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        std::printf("candidate: %s heap=%.2f GB\n", props.deviceName, heap / 1e9);
        if (heap > bestHeap) { bestHeap = heap; vk.pd = d; vk.qf = q; }
    }
    if (vk.pd == VK_NULL_HANDLE) { std::fprintf(stderr, "no coopmat compute device\n"); std::exit(1); }

    const char *exts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    // shaders declare Float16/Int64/16-bit storage/memory-model/BDA/coopmat —
    // features must be enabled or the device faults (Arc device-lost).
    VkPhysicalDeviceFeatures fe{};
    fe.shaderInt64 = VK_TRUE;
    fe.shaderInt16 = VK_TRUE;
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
    VK_CHECK(vkCreateDevice(vk.pd, &dci, nullptr, &vk.dev));
    vkGetDeviceQueue(vk.dev, vk.qf, 0, &vk.queue);
    vkGetPhysicalDeviceMemoryProperties(vk.pd, &vk.mem);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.pd, &props);
    vk.timestampBits = props.limits.timestampComputeAndGraphics ? 32 : 0;
    vk.tsPeriod = props.limits.timestampPeriod;
    std::printf("device: %s  (timestampBits=%u period=%.3f ns)\n",
                props.deviceName, vk.timestampBits, vk.tsPeriod);

    auto fpEnum = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        vkGetInstanceProcAddr(vk.inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    uint32_t ncfg = 0;
    VK_CHECK(fpEnum(vk.pd, &ncfg, nullptr));
    std::vector<VkCooperativeMatrixPropertiesKHR> cfgs(ncfg,
        VkCooperativeMatrixPropertiesKHR{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, nullptr});
    VK_CHECK(fpEnum(vk.pd, &ncfg, cfgs.data()));
    bool ok816 = false;
    for (auto &c : cfgs)
        if (c.MSize == 8 && c.NSize == 16 && c.KSize == 16 &&
            c.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && c.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            c.CType == VK_COMPONENT_TYPE_FLOAT32_KHR)
            ok816 = true;
    if (!ok816) { std::fprintf(stderr, "8x16x16 f16xf16->f32 coopmat config missing\n"); std::exit(1); }
    std::printf("coopmat 8x16x16 f16xf16->f32: OK (%u configs)\n", ncfg);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk.qf;
    VK_CHECK(vkCreateCommandPool(vk.dev, &cpci, nullptr, &vk.cmdPool));
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

static uint32_t memType(const VkCtx &vk, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vk.mem.memoryTypeCount; ++i)
        if ((vk.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
    std::exit(1);
}

static void allocDev(const VkCtx &vk, VkDeviceSize size, Buffer &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo mafi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &mafi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(vk, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.dev, &mai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(vk.dev, b.buf, b.mem, 0));
    b.size = size;
}

static void allocHost(const VkCtx &vk, VkDeviceSize size, Buffer &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(vk, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.dev, &mai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(vk.dev, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(vk.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    b.size = size;
}

static void freeBuf(const VkCtx &vk, Buffer &b) {
    if (b.mapped) vkUnmapMemory(vk.dev, b.mem);
    if (b.buf) vkDestroyBuffer(vk.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(vk.dev, b.mem, nullptr);
    b = Buffer{};
}

// ---------------------------------------------------------------- shaders --
struct Pipeline {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;   // layouts reference it — keep alive
};

static VkShaderModule loadShader(const VkCtx &vk, const std::string &path) {
    std::vector<char> spv;
    if (readFile(path, spv)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = (const uint32_t *)spv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(vk.dev, &smci, nullptr, &m));
    return m;
}

static void makePipeline(const VkCtx &vk, const std::string &spv, Pipeline &out) {
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(vk.dev, &plci, nullptr, &out.layout));
    VkShaderModule mod = loadShader(vk, spv);
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = out.layout;
    VK_CHECK(vkCreateComputePipelines(vk.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out.pipe));
    vkDestroyShaderModule(vk.dev, mod, nullptr);
    out.dsl = dsl;   // NOT destroyed: pipeline layout references it (device-lost on Arc otherwise)
}

static VkCommandBuffer beginCmd(const VkCtx &vk) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = vk.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(vk.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void submitWait(const VkCtx &vk, VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkFence fence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(vk.dev, &fci, nullptr, &fence));
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(vk.dev, fence, nullptr);
    vkFreeCommandBuffers(vk.cmdPool ? vk.dev : vk.dev, vk.cmdPool, 1, &cb);
}

static void fullBarrier(VkCommandBuffer cb, VkBuffer arena) {
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = arena;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &b, 0, nullptr);
}

// push blocks (scalar layout)
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
#pragma pack(pop)

enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1 };

// ===========================================================================
// M9 — full 71-block DLSSNR U-Net at runtime extent (vendor dataflow).
//
// Unlike M8a (constant 24x12 grid, pools neutralized), this runs the REAL
// spatial dataflow of mlxdlss/model.py:
//   L0 full res    : b0, b70+head                    (C=32)
//   L1 1/2         : b1-3, b66-69                    (C=32)
//   L2 1/4         : b4(ds), b5-7, b62-65            (C=64)
//   L3 1/8         : b8(ds), b9-13, b56-61           (C=128)
//   L4 1/16        : b14(ds), b15-21, b48-55         (C=256)
//   L5 pad8(L4)/2  : b22(ds,pad8), b23-30, b39-47    (C=512)
//   L6 pad8(L5)/2  : b30 bridge, b31-38 global MHA   (C=1024)
// ds = avgpool2 (zero pad-to-8 for b22) -> e4m3 -> gemm weight0 -> e4m3.
// up = gemm weight0 -> nearest-x2 crop -> + skip*sin -> e4m3 -> window block.
// Golden: work/_ref_test/golden_*.bin (dump_torch.py, torch CPU reference).
// ===========================================================================
static uint32_t IMG_W = 512, IMG_H = 320;   // --extent WxH (mult of 64, >=320)
static int g_to = 70;        // --to N: stop after block N
static long g_dispCount = 0;
static long g_maxDisp = 1 << 30;
static bool g_dispDbg = false;
static bool g_noTs = false;
static int g_bench = 0;      // --bench N: N extra timed chains
#define DISP_GATE(tag)                                        \
    do {                                                      \
        ++g_dispCount;                                        \
        if (g_dispDbg) std::fprintf(stderr, "[disp %ld] %s\n", g_dispCount, tag); \
        if (g_dispCount > g_maxDisp) return;                  \
    } while (0)

static uint32_t pad8u(uint32_t v) { return (v + 7u) & ~7u; }

static int blockIndexOf(const std::string &name) {
    auto dot = name.find('.');
    return std::atoi(name.substr(5, dot - 5).c_str());
}

// Fragment swizzle (nr_model._fragment_swizzle_indices): the single-head
// window blocks (0-4, 66-70) and the 16-head split blocks (23-30, 40-47) store
// attn_bias in fused-kernel mma fragment order; logical[j] = stored[SWZ[j]].
// The 2/4/8-head blocks (5-22, 48-65) store the logical layout already.
static uint32_t SWZ[4096];
static void initSwizzle() {
    for (int entry = 0; entry < 4096; ++entry) {
        int q = entry / 64, k = entry % 64;
        int qy = q / 8, qx = q % 8, ky = k / 8, kx = k % 8;
        auto bit = [](int v, int p) { return (v >> p) & 1; };
        SWZ[entry] = (uint32_t)((bit(qy, 2) << 11) | (bit(qx, 2) << 10) | (bit(ky, 2) << 9) |
                                (bit(kx, 2) << 8) | (bit(qy, 0) << 7) | (bit(qx, 1) << 6) |
                                (bit(qx, 0) << 5) | (bit(ky, 0) << 4) | (bit(kx, 1) << 3) |
                                (bit(ky, 1) << 2) | (bit(qy, 1) << 1) | bit(kx, 0));
    }
}
static bool biasNeedsSwizzle(const std::string &n) {
    auto ends = [&](const char *suf) {
        size_t l = std::strlen(suf);
        return n.size() >= l && n.compare(n.size() - l, l, suf) == 0;
    };
    int blk = blockIndexOf(n);
    if (ends(".layer0.attn_bias")) return blk <= 4 || blk >= 66;
    if (ends(".layer2.attn_bias")) return (blk >= 23 && blk <= 30) || (blk >= 40 && blk <= 47);
    return false;
}

struct Stage { uint32_t H, W, TOK; };

int main(int argc, char **argv) {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    g_hasF16C = (cpuInfo[2] & (1 << 29)) != 0;
    initSwizzle();
    std::string featPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--extent") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%ux%u", &IMG_W, &IMG_H);
        }
        if (std::strcmp(argv[i], "--features") == 0 && i + 1 < argc) featPath = argv[++i];
        if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) g_to = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--maxdisp") == 0 && i + 1 < argc) g_maxDisp = std::atol(argv[++i]);
        if (std::strcmp(argv[i], "--dispdbg") == 0) g_dispDbg = true;
        if (std::strcmp(argv[i], "--nots") == 0) g_noTs = true;
        if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) g_bench = std::atoi(argv[++i]);
    }
    if (IMG_W % 64 || IMG_H % 64 || IMG_W < 320 || IMG_H < 320) {
        std::fprintf(stderr, "extent must be multiples of 64 and >= 320 (got %ux%u)\n", IMG_W, IMG_H);
        return 1;
    }
    std::printf("M9 U-Net full-chain — Arc Pro B50, extent %ux%u\n", IMG_W, IMG_H);
    std::printf("CPU F16C: %s\n", g_hasF16C ? "yes" : "no (software RNE)");

    VkCtx vk{};
    vkInit(vk);

    std::string dir = exeDir();
    std::string repo = "C:\\Users\\AI\\Desktop\\DLSS5_INTEL";
    std::string outDir = dir + "out\\";
    CreateDirectoryA(outDir.c_str(), nullptr);

    auto tAll0 = clk::now();

    // ---- stage geometry ----------------------------------------------------
    Stage st[7];
    st[0] = {IMG_H, IMG_W, 0};
    st[1] = {IMG_H / 2, IMG_W / 2, 0};
    st[2] = {IMG_H / 4, IMG_W / 4, 0};
    st[3] = {IMG_H / 8, IMG_W / 8, 0};
    st[4] = {IMG_H / 16, IMG_W / 16, 0};
    st[5] = {pad8u(IMG_H / 16) / 2, pad8u(IMG_W / 16) / 2, 0};
    st[6] = {pad8u(st[5].H) / 2, pad8u(st[5].W) / 2, 0};
    for (auto &s : st) s.TOK = s.H * s.W;
    for (int i = 0; i < 7; ++i)
        std::printf("  L%d: %4ux%-4u  TOK=%u\n", i, st[i].H, st[i].W, st[i].TOK);

    // ======================= 1. weight pack layout ==========================
    WeightsFile wf;
    stParse(repo + "\\work\\mlxw\\dlssnr-logical.safetensors", wf);

    struct PackEntry { std::string name; uint64_t off, bytes; int blk; };
    std::vector<PackEntry> pack;
    for (auto &kv : wf.tensors) {
        const Tensor &t = kv.second;
        uint64_t esz = (t.dtype == "F32") ? 4 : 2;
        pack.push_back({kv.first, 0, t.numel() * esz, blockIndexOf(kv.first)});
    }
    std::sort(pack.begin(), pack.end(), [](const PackEntry &a, const PackEntry &b) {
        if (a.blk != b.blk) return a.blk < b.blk;
        return a.name < b.name;
    });
    {
        uint64_t o = 0;
        for (auto &e : pack) {
            o = (o + 255) & ~255ull;
            e.off = o;
            o += e.bytes;
        }
    }
    uint64_t packTotal = 291535872ull;
    std::map<std::string, uint64_t> poff;
    for (auto &e : pack) poff[e.name] = e.off;
    std::printf("pack: %zu tensors (%.2f MiB)\n", pack.size(), packTotal / 1048576.0);
    bool packOk = poff["block31.layer0.weight"] == 45084928ull &&
                  poff["block39.layer0.conv_weight"] == 246448384ull &&
                  poff["block0.layer0.input_adapter_weight"] == 8960ull;
    if (!packOk) { std::fprintf(stderr, "pack offset mismatch vs pack-layout.txt\n"); return 1; }

    auto tname = [](const char *fmt, int i) -> std::string {
        char b[160]; std::snprintf(b, sizeof b, fmt, i); return b;
    };

    // ======================= 2. arena layout =================================
    struct Ar {
        VkDeviceSize base = 0;
        VkDeviceSize off = 0;
        VkDeviceSize alloc(VkDeviceSize bytes) {
            VkDeviceSize a = (off + 255) & ~(VkDeviceSize)255;
            off = a + bytes;
            return base + a;
        }
    } ar;
    ar.base = packTotal;
    std::map<std::string, VkDeviceSize> vecOff;
    auto needVec = [&](const std::string &n) {
        if (!vecOff.count(n)) { VkDeviceSize o = ar.alloc(4096); vecOff[n] = o; }
        return vecOff[n];
    };
    VkDeviceSize oHeadW = ar.alloc(1024);

    auto slot = [&](VkDeviceSize bytes) { return ar.alloc(bytes); };

    // per-scale maxima (window scales s0..s5: C = 32,32,64,128,256,512; heads = C/32)
    uint64_t maxTokC = 0, maxTok4C = 0;
    const uint32_t stageC[6] = {32, 32, 64, 128, 256, 512};
    for (int s = 0; s < 6; ++s) {
        maxTokC = std::max(maxTokC, (uint64_t)st[s].TOK * stageC[s]);
        maxTok4C = std::max(maxTok4C, (uint64_t)st[s].TOK * 4 * stageC[s]);
    }
    maxTokC = std::max(maxTokC, (uint64_t)st[6].TOK * 1024);
    uint64_t wWin = 0, wProj = 0, wQkv = 0, wSc = 0;
    for (int s = 0; s < 6; ++s) {
        uint32_t hp8 = (st[s].H + 4 + 7) / 8, wp8 = (st[s].W + 4 + 7) / 8;
        uint64_t W = (uint64_t)hp8 * wp8, Hd = stageC[s] / 32;
        wWin = std::max(wWin, W * 64 * stageC[s]);
        wProj = std::max(wProj, W * 64 * 3 * stageC[s]);
        wQkv = std::max(wQkv, W * Hd * 64 * 32);
        wSc = std::max(wSc, W * Hd * 64 * 64);
    }

    VkDeviceSize oX = slot((uint64_t)st[0].TOK * 16 * 4);
    VkDeviceSize oX16 = slot((uint64_t)st[0].TOK * 16 * 2);
    VkDeviceSize oADA = slot((uint64_t)st[0].TOK * 32 * 4);
    VkDeviceSize oG1 = slot(maxTokC * 4);
    VkDeviceSize oG2 = slot(maxTokC * 4);
    VkDeviceSize oG3 = slot(maxTok4C * 4);
    VkDeviceSize oG4 = slot(maxTokC * 4);
    VkDeviceSize oRAW = slot(maxTokC * 4);
    VkDeviceSize oPOOL = slot(maxTokC * 2);
    // window attention set
    VkDeviceSize oWIN = slot(wWin * 2);
    VkDeviceSize oPROJW = slot(wProj * 4);
    VkDeviceSize oQW = slot(wQkv * 2), oKW = slot(wQkv * 2), oVW = slot(wQkv * 2);
    VkDeviceSize oSCW = slot(wSc * 4), oPRW = slot(wSc * 2);
    VkDeviceSize oMGW = slot(wQkv * 4), oATW = slot(wWin * 2), oABW = slot(wWin * 4);
    // global set (L6); token count padded to 32 for the coopmat score GEMM —
    // softmax masks cols >= TOK6 (nReal), padded rows carry zeros through.
    uint64_t T6 = st[6].TOK;
    uint32_t T6P = (st[6].TOK + 31) & ~31u;
    VkDeviceSize oHG = slot((uint64_t)T6P * 4096 * 2);
    VkDeviceSize oPROJG = slot((uint64_t)T6P * 3072 * 4);
    VkDeviceSize oQG = slot((uint64_t)T6P * 1024 * 2), oKG = slot((uint64_t)T6P * 1024 * 2), oVG = slot((uint64_t)T6P * 1024 * 2);
    VkDeviceSize oSCG = slot(32ull * T6P * T6P * 4), oPRG = slot(32ull * T6P * T6P * 2);
    VkDeviceSize oMGG = slot((uint64_t)T6P * 1024 * 4), oATG = slot((uint64_t)T6P * 1024 * 2);
    VkDeviceSize oABG = slot((uint64_t)T6P * 1024 * 4), oBRG = slot((uint64_t)T6P * 1024 * 4);
    // boundary / skip slots
    VkDeviceSize oB0RAW = slot((uint64_t)st[0].TOK * 32 * 4), oFRS = slot((uint64_t)st[0].TOK * 32 * 2);
    VkDeviceSize oSKIP0 = slot((uint64_t)st[1].TOK * 32 * 2), oSKIP1 = slot((uint64_t)st[2].TOK * 64 * 2);
    VkDeviceSize oSKIP2 = slot((uint64_t)st[3].TOK * 128 * 2), oSKIP3 = slot((uint64_t)st[4].TOK * 256 * 2);
    VkDeviceSize oSS = slot((uint64_t)st[5].TOK * 512 * 2);
    VkDeviceSize oB4DS = slot((uint64_t)st[2].TOK * 64 * 2), oB22DS = slot((uint64_t)st[5].TOK * 512 * 2);
    VkDeviceSize oBRIDGE = slot((uint64_t)T6P * 1024 * 2);
    VkDeviceSize oB38 = slot((uint64_t)T6P * 1024 * 2), oB39 = slot((uint64_t)st[5].TOK * 512 * 2);
    VkDeviceSize oB48 = slot((uint64_t)st[4].TOK * 256 * 2);
    VkDeviceSize oB69 = slot((uint64_t)st[1].TOK * 32 * 2);
    VkDeviceSize oMRG39 = slot((uint64_t)st[5].TOK * 512 * 4);
    VkDeviceSize oMRG70 = slot((uint64_t)st[0].TOK * 32 * 4), oHEAD = slot((uint64_t)st[0].TOK * 16 * 4);
    VkDeviceSize oDBG = slot(256ull * 1024 * 1024);   // mid-chain bisect copies

    // fp32 vector side tables (same set as M8a)
    auto preVec = [&](const std::string &n) { if (poff.count(n)) needVec(n); };
    for (int i = 0; i <= 70; ++i) {
        if (i == 39) { preVec("block39.layer0.inp_upsample_sin"); continue; }
        if (i >= 31 && i <= 38) {
            preVec(tname("block%d.layer1.ffn_cos_skip", i));
            preVec(tname("block%d.layer4.attn_cos_skip", i));
            needVec(tname("block%d.layer2.attn_scale.q32", i));
            continue;
        }
        if ((i >= 23 && i <= 30) || (i >= 40 && i <= 47)) {
            preVec(tname("block%d.layer1.ffn_cos_skip", i));
            preVec(tname("block%d.layer3.attn_cos_skip", i));
        } else {
            preVec(tname("block%d.layer0.ffn_cos_skip", i));
            preVec(tname("block%d.layer0.attn_cos_skip", i));
        }
        if (i == 48 || i == 56 || i == 62 || i == 66)
            preVec(tname("block%d.layer0.sin", i));
        if (i == 70) {
            preVec("block70.layer0.inp_merge_sin");
            preVec("block70.layer0.inp_merge_cos");
        }
    }

    VkDeviceSize totalBytes = packTotal + ar.off;
    std::printf("arena: %.2f MB scratch after weights; one device buffer, total %.1f MB\n",
                ar.off / 1048576.0, totalBytes / 1048576.0);

    Buffer dev;
    allocDev(vk, totalBytes, dev);

    // ======================= 3. staging + preprocessing ====================
    auto tUp0 = clk::now();
    Buffer staging;
    allocHost(vk, packTotal + ar.off, staging);
    char *sp = (char *)staging.mapped;

    auto isBranchedExpand = [&](const std::string &n) {
        return n.size() >= 18 && n.substr(n.size() - 18) == ".ffn_expand_weight";
    };

    for (auto &e : pack) {
        const Tensor &t = wf.tensors[e.name];
        const char *src = wf.bytes.data() + wf.dataBase + t.off0;
        char *dst = sp + e.off;
        if (isBranchedExpand(e.name)) {
            uint64_t G = t.shape[0];
            const uint16_t *s = (const uint16_t *)src;
            uint16_t *d = (uint16_t *)dst;
            for (uint64_t oh = 0; oh < G; ++oh)
                for (uint64_t ih = 0; ih < G; ++ih)
                    for (uint64_t r = 0; r < 32; ++r)
                        for (uint64_t br = 0; br < 4; ++br)
                            for (uint64_t c = 0; c < 32; ++c)
                                d[((oh * G + ih) * 32 + r) * 128 + br * 32 + c] =
                                    s[((oh * 4 + br) * G + ih) * 1024 + r * 32 + c];
        } else if (biasNeedsSwizzle(e.name)) {
            const uint16_t *s = (const uint16_t *)src;
            uint16_t *d = (uint16_t *)dst;
            uint64_t heads = t.numel() / 4096;
            for (uint64_t h = 0; h < heads; ++h)
                for (uint32_t j = 0; j < 4096; ++j)
                    d[h * 4096 + j] = s[h * 4096 + SWZ[j]];
        } else {
            std::memcpy(dst, src, e.bytes);
        }
    }
    auto placeVecF16 = [&](const std::string &n) {
        auto it = poff.find(n);
        if (it == poff.end()) { std::fprintf(stderr, "missing vec tensor %s\n", n.c_str()); return; }
        const Tensor &t = wf.tensors[n];
        VkDeviceSize o = needVec(n);
        const uint16_t *s = (const uint16_t *)(sp + it->second);
        float *d = (float *)(sp + o);
        for (uint64_t i = 0; i < t.numel(); ++i) d[i] = f16_bits_to_f32(s[i]);
    };
    for (int i = 0; i <= 70; ++i) {
        if (i == 39) { placeVecF16("block39.layer0.inp_upsample_sin"); continue; }
        if (i >= 31 && i <= 38) {
            placeVecF16(tname("block%d.layer1.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer4.attn_cos_skip", i));
            continue;
        }
        if ((i >= 23 && i <= 30) || (i >= 40 && i <= 47)) {
            placeVecF16(tname("block%d.layer1.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer3.attn_cos_skip", i));
        } else {
            placeVecF16(tname("block%d.layer0.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer0.attn_cos_skip", i));
        }
        if (i == 48 || i == 56 || i == 62 || i == 66)
            placeVecF16(tname("block%d.layer0.sin", i));
        if (i == 70) {
            placeVecF16("block70.layer0.inp_merge_sin");
            placeVecF16("block70.layer0.inp_merge_cos");
        }
    }
    for (int i = 31; i <= 38; ++i) {
        std::string n = tname("block%d.layer2.attn_scale", i);
        const Tensor &t = wf.tensors[n];
        const float *s = (const float *)(sp + poff[n]);
        VkDeviceSize o = needVec(n + ".q32");
        float *d = (float *)(sp + o);
        float sq = std::sqrt(32.0f);
        for (uint64_t j = 0; j < t.numel(); ++j) d[j] = s[j] * sq;
    }
    {
        uint16_t *d = (uint16_t *)(sp + oHeadW);
        std::memset(d, 0, 1024);
        const uint16_t *sg = (const uint16_t *)(sp + poff["block70.layer0.out_gain"]);
        const uint16_t *sc = (const uint16_t *)(sp + poff["block70.layer0.out_conv_weight"]);
        for (int r = 0; r < 16; ++r)
            for (int c = 0; c < 4; ++c) {
                d[r * 16 + c] = sg[r * 4 + c];
                d[(16 + r) * 16 + c] = sc[r * 4 + c];
            }
    }

    VkCommandBuffer up = beginCmd(vk);
    VkBufferCopy cp1{0, 0, packTotal + ar.off};
    vkCmdCopyBuffer(up, staging.buf, dev.buf, 1, &cp1);
    submitWait(vk, up);
    std::printf("weight upload: %.1f MB in %.1f ms\n", (packTotal + ar.off) / 1048576.0, msSince(tUp0));

    // ======================= 4. features input =============================
    {
        std::vector<float> x;
        if (!featPath.empty()) {
            std::ifstream f(featPath, std::ios::binary | std::ios::ate);
            if (!f) { std::fprintf(stderr, "cannot read features %s\n", featPath.c_str()); return 1; }
            std::streamsize sz = f.tellg();
            x.resize((size_t)sz / 4);
            f.seekg(0, std::ios::beg);
            if (!f.read((char *)x.data(), sz)) return 1;
            if (x.size() != (size_t)st[0].TOK * 16) {
                std::fprintf(stderr, "features size mismatch: got %zu floats, want %u\n",
                             x.size(), st[0].TOK * 16);
                return 1;
            }
        } else {
            x.resize((size_t)st[0].TOK * 16);
            const double PI = 3.14159265358979323846;
            for (uint32_t t = 0; t < st[0].TOK; ++t)
                for (uint32_t c = 0; c < 16; ++c) {
                    double v = 0.50 * std::sin(2 * PI * (0.011 * t + 0.031 * c)) +
                               0.30 * std::cos(2 * PI * (0.071 * t - 0.017 * c) + 0.7) +
                               0.15 * std::sin(2 * PI * (0.137 * t + 0.053 * c) + 1.9);
                    x[(size_t)t * 16 + c] = (float)v;
                }
        }
        char *sx = sp + oX;
        std::memcpy(sx, x.data(), x.size() * 4);
        uint16_t *sx16 = (uint16_t *)(sp + oX16);
        for (size_t i = 0; i < x.size(); ++i) sx16[i] = f32_to_f16(x[i]);
        VkCommandBuffer cbx = beginCmd(vk);
        VkBufferCopy cxx{oX, oX, (VkDeviceSize)(x.size() * 4)};
        VkBufferCopy cx16{oX16, oX16, (VkDeviceSize)(x.size() * 2)};
        vkCmdCopyBuffer(cbx, staging.buf, dev.buf, 1, &cxx);
        vkCmdCopyBuffer(cbx, staging.buf, dev.buf, 1, &cx16);
        submitWait(vk, cbx);
        std::printf("features upload ok (%zu floats)\n", x.size());
    }

    Pipeline pGemm, pGemm1, pCos, pCosW, pSmax, pEw, pPart, pTrans, pGather, pMerge, pPool, pUpM;
    makePipeline(vk, dir + "gemm.spv", pGemm);
    makePipeline(vk, dir + "gemm_rn1.spv", pGemm1);
    makePipeline(vk, dir + "cosine.spv", pCos);
    makePipeline(vk, dir + "cosine_win.spv", pCosW);
    makePipeline(vk, dir + "softmax.spv", pSmax);
    makePipeline(vk, dir + "elementwise.spv", pEw);
    makePipeline(vk, dir + "partition.spv", pPart);
    makePipeline(vk, dir + "transpose_we.spv", pTrans);
    makePipeline(vk, dir + "gather_residual.spv", pGather);
    makePipeline(vk, dir + "merge.spv", pMerge);
    makePipeline(vk, dir + "pool2.spv", pPool);
    makePipeline(vk, dir + "upmerge.spv", pUpM);
    std::printf("pipelines ok\n");

    VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, dev.buf};
    uint64_t A0 = vkGetBufferDeviceAddress(vk.dev, &bdai);
    auto A = [&](VkDeviceSize o) { return A0 + (uint64_t)o; };

    const char *famName[] = {"stem(0-4)", "enc(5-22)", "bottleneck(23-30)",
                             "global(31-38)", "decoder(39-69)", "head(70)"};
    double famFlops[6] = {0, 0, 0, 0, 0, 0};

    // dispatch helpers ------------------------------------------------------
    auto bar = [&](VkCommandBuffer &cb) { fullBarrier(cb, dev.buf); };
    auto gflags = [&](uint32_t epi, bool narrow) { return (epi << 8) | (narrow ? F_NARROW : 0u); };
    auto dGemm = [&](VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c,
                     uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                     uint32_t sa, uint32_t sb, uint32_t sc,
                     uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
        DISP_GATE("dGemm");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
        GemmPush p{a, b, c, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc};
        vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, n / 32, m / 16, batch);
        if (fam >= 0) famFlops[fam] += 2.0 * m * n * k * batch;
    };
    auto dEw = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                   uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
        DISP_GATE("dEw");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
        EWPush p{a, b, c, d, h, n, kind, ch};
        vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };
    auto dPart = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, const Stage &s, uint32_t C,
                     uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
        DISP_GATE("dPart");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
        uint32_t hp8 = (s.H + padTop + 7) / 8;
        PartPush p{a, c, s.H, s.W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
        vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
    };
    auto dGather = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c,
                       uint64_t h, const Stage &s, uint32_t C, uint32_t padTop, uint32_t padLeft,
                       uint32_t wp8, bool pub, bool b16) {
        DISP_GATE("dGather");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGather.pipe);
        GatherPush p{a, b, d, c, h, s.H, s.W, C, padTop, padLeft, wp8,
                     (pub ? 1u : 0u) | (b16 ? 2u : 0u)};
        vkCmdPushConstants(cb, pGather.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, s.TOK, 1, 1);
    };
    auto dTrans = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint32_t W, uint32_t H) {
        DISP_GATE("dTrans");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pTrans.pipe);
        TransWePush p{a, c, W, H};
        vkCmdPushConstants(cb, pTrans.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, W * 64, 1, 1);
    };
    auto dCosW = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint64_t sc,
                     uint32_t W, uint32_t H, uint32_t C, uint32_t kind) {
        DISP_GATE("dCosW");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCosW.pipe);
        CosWinPush p{a, c, sc, W * 64 * H, kind, C, H};
        vkCmdPushConstants(cb, pCosW.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (W * 64 * H + 31) / 32, 1, 1);
    };
    auto dCosG = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint64_t sc, uint32_t kind) {
        DISP_GATE("dCosG");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
        CosPush p{a, c, sc, T6P * 32, kind};
        vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (T6P * 32 + 31) / 32, 1, 1);
    };
    auto dSmaxW = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint32_t W, uint32_t H,
                      uint64_t bias) {
        DISP_GATE("dSmaxW");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c, W * H * 64, 64, 0.0f, bias, H, 0};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (W * H * 64 + 31) / 32, 1, 1);
    };
    auto dSmaxG = [&](VkCommandBuffer cb, uint64_t a, uint64_t c) {
        DISP_GATE("dSmaxG");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c, 32 * T6P, T6P, 3.0f, 0, 0, st[6].TOK};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, 32 * T6P, 1, 1);
    };
    struct PoolPush { uint64_t a, c; uint32_t Hin, Win, Wpad, C, n, flags; };
    auto dPool = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint32_t Hin, uint32_t Win,
                     uint32_t Wpad, uint32_t Hpad, uint32_t C, uint32_t flags) {
        DISP_GATE("dPool");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPool.pipe);
        uint32_t n = (Hpad / 2) * (Wpad / 2);
        PoolPush p{a, c, Hin, Win, Wpad, C, n, flags};
        vkCmdPushConstants(cb, pPool.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };
    struct UpMergePush {
        uint64_t a, b, c, d, e, h;
        uint32_t Wt, Ht, Wl, C, kind;
    };
    auto dUpMerge = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c, uint64_t sin,
                        uint64_t cos, uint64_t h, const Stage &tgt, const Stage &low,
                        uint32_t C, uint32_t kind) {
        DISP_GATE("dUpMerge");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pUpM.pipe);
        UpMergePush p{a, b, c, sin, cos, h, tgt.W, tgt.H, low.W, C, kind};
        vkCmdPushConstants(cb, pUpM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (tgt.TOK + 255) / 256, 1, 1);
    };

    auto winGeo = [](const Stage &s, int oy, int ox,
                     uint32_t &padTop, uint32_t &padLeft, uint32_t &W, uint32_t &wp8) {
        padTop = (uint32_t)(-oy);
        padLeft = (uint32_t)(-ox);
        uint32_t hp8 = (s.H + padTop + 7) / 8;
        wp8 = (s.W + padLeft + 7) / 8;
        W = hp8 * wp8;
    };

    struct Cur { VkDeviceSize off; bool is16; uint32_t C; };

    // ---- window attention shared tail (per-stage geometry) -----------------
    auto recordWindowAttn = [&](VkCommandBuffer &cb, int famIdx, int idx, const Stage &s,
                                int C, int H, int fam, int oy, int ox,
                                VkDeviceSize ffnOff, bool ffn16,
                                VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish) {
        uint32_t padTop, padLeft, W, wp8;
        winGeo(s, oy, ox, padTop, padLeft, W, wp8);
        auto lay = [&](const char *fmt) { return tname(fmt, idx); };
        std::string qkvN = lay(fam == 2 ? "block%d.layer2.qkv_weight" : "block%d.layer0.qkv_weight");
        std::string scN = lay(fam == 2 ? "block%d.layer2.attn_scale" : "block%d.layer0.attn_scale");
        std::string biasN = lay(fam == 2 ? "block%d.layer2.attn_bias" : "block%d.layer0.attn_bias");
        std::string projN = lay(fam == 2 ? "block%d.layer3.projection_weight" : "block%d.layer0.projection_weight");
        std::string acosN = lay(fam == 2 ? "block%d.layer3.attn_cos_skip" : "block%d.layer0.attn_cos_skip");

        dPart(cb, A(ffnOff), A(oWIN), s, (uint32_t)C, padTop, padLeft, wp8, ffn16);
        bar(cb);
        dGemm(cb, famIdx, A(oWIN), A(poff[qkvN]), A(oPROJW), 64, 3 * C, C, W,
              64 * C, 0, 64 * 3 * C, gflags(EPI_NONE, false), C, 3 * C, 3 * C);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oQW), A(poff[scN]), W, H, C, 0);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oKW), A(poff[scN]), W, H, C, 1);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oVW), A(poff[scN]), W, H, C, 2);
        bar(cb);
        dGemm(cb, famIdx, A(oQW), A(oKW), A(oSCW), 64, 64, 32, W * H,
              64 * 32, 64 * 32, 64 * 64, gflags(EPI_NONE, false) | F_TRANSPOSE, 32, 32, 64);
        bar(cb);
        dSmaxW(cb, A(oSCW), A(oPRW), W, H, A(poff[biasN]));
        bar(cb);
        dGemm(cb, famIdx, A(oPRW), A(oVW), A(oMGW), 64, 32, 64, W * H,
              64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
        bar(cb);
        dTrans(cb, A(oMGW), A(oATW), W, H);
        bar(cb);
        dGemm(cb, famIdx, A(oATW), A(poff[projN]), A(oABW), 64, C, C, W,
              64 * C, 0, 64 * C, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dGather(cb, A(oABW), A(ffnOff), A(vecOff[acosN]), A(rawOff), A(publish ? pubOff : rawOff),
                s, (uint32_t)C, padTop, padLeft, wp8, publish, ffn16);
        bar(cb);
    };

    // FFN + attention for one window block; writes raw (always) + pub (if publish).
    auto recordWindowBlock = [&](VkCommandBuffer &cb, int famIdx, int idx, const Stage &s,
                                 int C, int H, int fam, int oy, int ox, bool publish, Cur &cur,
                                 VkDeviceSize rawOff, VkDeviceSize pubOff) {
        uint32_t TOKS = s.TOK;
        uint32_t G = C / 32;
        VkDeviceSize x16 = cur.is16 ? cur.off : oG2;
        if (!cur.is16) { dEw(cb, A(cur.off), 0, 0, 0, A(oG2), TOKS * C, 2, 0); bar(cb); }
        if (fam == 0) {
            dGemm(cb, famIdx, A(x16), A(poff[tname("block%d.layer0.weight1", idx)]), A(oG3),
                  TOKS, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
            bar(cb);
            dGemm(cb, famIdx, A(oG3), A(poff[tname("block%d.layer0.weight2", idx)]), A(oG4),
                  TOKS, C, 4 * C, 1, 0, 0, 0, gflags(EPI_NONE, false), 4 * C, C, C);
            bar(cb);
            if (cur.is16)
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOKS * C, 4, C);
            else
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOKS * C, 1, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, oG4, false, rawOff, pubOff, publish);
        } else if (fam == 1) {
            uint64_t expBase = A(poff[tname("block%d.layer0.ffn_expand_weight", idx)]);
            uint64_t prjBase = A(poff[tname("block%d.layer0.ffn_branch_projection_weight", idx)]);
            for (uint32_t oh = 0; oh < G; ++oh) {
                dGemm(cb, famIdx, A(x16), expBase + (uint64_t)oh * C * 128 * 2, A(oG3),
                      TOKS, 128, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 128, 128);
                bar(cb);
                dGemm(cb, famIdx, A(oG3), prjBase + (uint64_t)oh * 128 * 32 * 2, A(oG4 + oh * 32 * 2),
                      TOKS, 32, 128, 1, 0, 0, 0, gflags(EPI_E4M3, true), 128, 32, C);
                bar(cb);
            }
            dGemm(cb, famIdx, A(oG4), A(poff[tname("block%d.layer0.ffn_output_projection_weight", idx)]),
                  A(oG3), TOKS, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
            bar(cb);
            dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]),
                A(oG2), TOKS * C, 5, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, oG2, true, rawOff, pubOff, publish);
        } else {
            dGemm(cb, famIdx, A(x16), A(poff[tname("block%d.layer0.first_projection_weight", idx)]),
                  A(oG2), TOKS, C, C, 1, 0, 0, 0, gflags(EPI_E4M3, true), C, C, C);
            bar(cb);
            dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer0.group_expand_weight", idx)]),
                  A(oG3), TOKS, 256, 64, 8, 64, 64 * 256, TOKS * 256,
                  gflags(EPI_GATE, true), C, 256, 256);
            bar(cb);
            dGemm(cb, famIdx, A(oG3), A(poff[tname("block%d.layer0.group_project_weight", idx)]),
                  A(oG4), TOKS, 64, 256, 8, TOKS * 256, 256 * 64, 64,
                  gflags(EPI_NONE, false), 256, 64, C);
            bar(cb);
            dEw(cb, A(oG4), 0, 0, 0, A(oG2), TOKS * C, 3, 0);
            bar(cb);
            dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer1.weight3", idx)]), A(oG3),
                  TOKS, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
            bar(cb);
            dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer1.ffn_cos_skip", idx)]),
                0, TOKS * C, 4, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, oG3, false, rawOff, pubOff, publish);
        }
    };

    // downsample transition: avgpool2 (pad-to-8 optional) -> e4m3 -> gemm w0 -> e4m3
    auto recordDs = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int Cn,
                        VkDeviceSize rawOff, VkDeviceSize outOff,
                        const Stage &inS, const Stage &outS, bool pad8) {
        uint32_t Hp = pad8 ? pad8u(inS.H) : inS.H;
        uint32_t Wp = pad8 ? pad8u(inS.W) : inS.W;
        dPool(cb, A(rawOff), A(oPOOL), inS.H, inS.W, Wp, Hp, (uint32_t)C, 2u);
        bar(cb);
        dGemm(cb, famIdx, A(oPOOL), A(poff[tname("block%d.layer0.weight0", idx)]), A(outOff),
              outS.TOK, (uint32_t)Cn, (uint32_t)C, 1, 0, 0, 0, gflags(EPI_E4M3, true),
              (uint32_t)C, (uint32_t)Cn, (uint32_t)Cn);
        bar(cb);
    };

    // upsample transition: gemm w0 -> nearest-x2 crop merge with skip*sin -> e4m3
    auto recordUp = [&](VkCommandBuffer &cb, int famIdx, int idx, int Ci, int Co,
                        Cur &cur, VkDeviceSize skipOff, VkDeviceSize out16Off,
                        const Stage &lowS, const Stage &tgtS) {
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight0", idx)]), A(oG3),
              lowS.TOK, (uint32_t)Co, (uint32_t)Ci, 1, 0, 0, 0, gflags(EPI_NONE, false),
              (uint32_t)Ci, (uint32_t)Co, (uint32_t)Co);
        bar(cb);
        dUpMerge(cb, A(oG3), A(skipOff), A(oG4), A(vecOff[tname("block%d.layer0.sin", idx)]), 0,
                 A(out16Off), tgtS, lowS, (uint32_t)Co, 0);
        bar(cb);
        cur = {out16Off, true, (uint32_t)Co};
    };

    // global block (L6 grid, input f16, padded to T6P rows)
    auto recordGlobal = [&](VkCommandBuffer &cb, int famIdx, int idx, Cur &cur,
                            VkDeviceSize rawOff, VkDeviceSize pubOff) {
        uint32_t TOKG = T6P;
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight", idx)]), A(oHG),
              TOKG, 4096, 1024, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), 1024, 4096, 4096);
        bar(cb);
        dGemm(cb, famIdx, A(oHG), A(poff[tname("block%d.layer1.weight", idx)]), A(oG3),
              TOKG, 1024, 4096, 1, 0, 0, 0, gflags(EPI_NONE, false), 4096, 1024, 1024);
        bar(cb);
        dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer1.ffn_cos_skip", idx)]),
            0, TOKG * 1024, 4, 1024);
        bar(cb);
        dEw(cb, A(oG3), 0, 0, 0, A(oG2), TOKG * 1024, 2, 0);
        bar(cb);
        dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer2.qkv_weight", idx)]), A(oPROJG),
              TOKG, 3072, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 3072, 3072);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oQG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 0);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oKG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 1);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oVG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 2);
        bar(cb);
        dGemm(cb, famIdx, A(oQG), A(oKG), A(oSCG), TOKG, TOKG, 32, 32,
              32, 32, TOKG * TOKG, gflags(EPI_NONE, false) | F_TRANSPOSE, 1024, 1024, TOKG);
        bar(cb);
        dSmaxG(cb, A(oSCG), A(oPRG));
        bar(cb);
        dGemm(cb, famIdx, A(oPRG), A(oVG), A(oMGG), TOKG, 32, TOKG, 32,
              TOKG * TOKG, 32, 32, gflags(EPI_NONE, false), TOKG, 1024, 1024);
        bar(cb);
        dEw(cb, A(oMGG), 0, 0, 0, A(oATG), TOKG * 1024, 3, 0);
        bar(cb);
        dGemm(cb, famIdx, A(oATG), A(poff[tname("block%d.layer4.projection_weight", idx)]), A(oABG),
              TOKG, 1024, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 1024, 1024);
        bar(cb);
        dEw(cb, A(oABG), A(oG3), A(oBRG), A(vecOff[tname("block%d.layer4.attn_cos_skip", idx)]),
            A(pubOff), TOKG * 1024, 0, 1024);
        bar(cb);
    };

    // shifted-window origin per block, mirroring nr_model.recovered_window_origin.
    auto originOf = [](int blockIndex) -> std::pair<int, int> {
        int phase;
        if (blockIndex == 0) phase = 0;
        else if (blockIndex >= 1 && blockIndex <= 4) phase = blockIndex - 1;
        else if (blockIndex >= 5 && blockIndex <= 8) phase = blockIndex - 5;
        else if (blockIndex >= 9 && blockIndex <= 14) phase = blockIndex - 9;
        else if (blockIndex >= 15 && blockIndex <= 22) phase = blockIndex - 15;
        else if (blockIndex >= 23 && blockIndex <= 30) phase = blockIndex - 23;
        else if (blockIndex >= 40 && blockIndex <= 55) phase = blockIndex - (blockIndex < 48 ? 40 : 48);
        else if (blockIndex >= 56 && blockIndex <= 61) phase = blockIndex - 54;
        else if (blockIndex >= 62 && blockIndex <= 69) phase = blockIndex - (blockIndex < 66 ? 62 : 66);
        else if (blockIndex == 70) phase = 1;
        else return {0, 0};
        const int dy[4] = {0, -4, 0, -4};
        const int dx[4] = {0, -4, -4, 0};
        return {dy[phase % 4], dx[phase % 4]};
    };

    // ---- mid-chain bisect copies into oDBG ---------------------------------
    struct DbgEnt { std::string name; VkDeviceSize off; VkDeviceSize bytes; bool f16; };
    std::vector<DbgEnt> dbgList;
    VkDeviceSize dbgOff = 0;
    auto dbgCp = [&](VkCommandBuffer &cb, const char *name, VkDeviceSize src, VkDeviceSize bytes,
                     bool f16) {
        VkBufferCopy c{src, oDBG + dbgOff, bytes};
        vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
        dbgList.push_back({name, oDBG + dbgOff, bytes, f16});
        dbgOff += (bytes + 255) & ~(VkDeviceSize)255;
    };

    // ---- the chain ---------------------------------------------------------
    auto recordChain = [&](VkCommandBuffer &cb, bool timed, VkQueryPool qp) {
        int tq = 0;
        auto tick = [&]() {
            if (timed && !g_noTs) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, (uint32_t)tq++);
        };
        Cur cur{oADA, false, 32};

        tick();
        // adapter GEMM at L0: x16 @ input_adapter_weight -> fp32
        dGemm(cb, 0, A(oX16), A(poff["block0.layer0.input_adapter_weight"]), A(oADA),
              st[0].TOK, 32, 16, 1, 0, 0, 0, gflags(EPI_NONE, false), 16, 32, 32);
        bar(cb);
        dbgCp(cb, "ada", oADA, (uint64_t)st[0].TOK * 32 * 4, false);
        if (g_to < 0) return;
        // b0 (plain, publish=false) at L0; frs = e4m3(raw); pool -> L1 e4m3
        {
            Cur c0 = cur;
            recordWindowBlock(cb, 0, 0, st[0], 32, 1, 0, 0, 0, false, c0, oB0RAW, 0);
            cur = {oB0RAW, false, 32};
            dbgCp(cb, "b0ffn", oG4, (uint64_t)st[0].TOK * 32 * 4, false);
            {
                uint32_t w0 = (st[0].H / 8) * (st[0].W / 8);
                dbgCp(cb, "b0abw", oABW, (uint64_t)w0 * 64 * 32 * 4, false);
            }
            dEw(cb, A(oB0RAW), 0, 0, 0, A(oFRS), st[0].TOK * 32, 3, 0);
            bar(cb);
            dPool(cb, A(oB0RAW), A(oPOOL), st[0].H, st[0].W, st[0].W, st[0].H, 32, 2u);
            bar(cb);
            cur = {oPOOL, true, 32};
        }
        if (g_to <= 0) return;
        for (int i = 1; i <= 3; ++i) {
            VkDeviceSize out = (i == 3) ? oSKIP0 : oG1;
            auto o = originOf(i);
            recordWindowBlock(cb, 0, i, st[1], 32, 1, 0, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 32};
        }
        if (g_to <= 3) return;
        {   // b4 window at L1 (publish=false) + ds to L2 C=64
            Cur c4 = cur;
            recordWindowBlock(cb, 0, 4, st[1], 32, 1, 0, -4, 0, false, c4, oRAW, 0);
            dbgCp(cb, "w4raw", oRAW, (uint64_t)st[1].TOK * 32 * 4, false);
            recordDs(cb, 0, 4, 32, 64, oRAW, oB4DS, st[1], st[2], false);
            cur = {oB4DS, true, 64};
        }
        tick();
        if (g_to <= 4) return;

        // enc L2: b5-7, b8 ds -> L3 C=128
        for (int i = 5; i <= 7; ++i) {
            VkDeviceSize out = (i == 7) ? oSKIP1 : oG1;
            auto o = originOf(i);
            recordWindowBlock(cb, 1, i, st[2], 64, 2, 1, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 64};
        }
        {
            Cur ci = cur;
            auto o = originOf(8);
            recordWindowBlock(cb, 1, 8, st[2], 64, 2, 1, o.first, o.second, false, ci, oRAW, 0);
            recordDs(cb, 1, 8, 64, 128, oRAW, oG1, st[2], st[3], false);
            dbgCp(cb, "ds8", oG1, (uint64_t)st[3].TOK * 128 * 2, true);
            cur = {oG1, true, 128};
        }
        if (g_to <= 8) return;
        // enc L3: b9-13, b14 ds -> L4 C=256
        for (int i = 9; i <= 13; ++i) {
            VkDeviceSize out = (i == 13) ? oSKIP2 : oG1;
            auto o = originOf(i);
            recordWindowBlock(cb, 1, i, st[3], 128, 4, 1, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 128};
        }
        {
            Cur ci = cur;
            auto o = originOf(14);
            recordWindowBlock(cb, 1, 14, st[3], 128, 4, 1, o.first, o.second, false, ci, oRAW, 0);
            recordDs(cb, 1, 14, 128, 256, oRAW, oG1, st[3], st[4], false);
            dbgCp(cb, "ds14", oG1, (uint64_t)st[4].TOK * 256 * 2, true);
            cur = {oG1, true, 256};
        }
        if (g_to <= 14) return;
        // enc L4: b15-21, b22 ds (pad8) -> L5 C=512
        for (int i = 15; i <= 21; ++i) {
            VkDeviceSize out = (i == 21) ? oSKIP3 : oG1;
            auto o = originOf(i);
            recordWindowBlock(cb, 1, i, st[4], 256, 8, 1, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 256};
        }
        {
            Cur ci = cur;
            auto o = originOf(22);
            recordWindowBlock(cb, 1, 22, st[4], 256, 8, 1, o.first, o.second, false, ci, oRAW, 0);
            dbgCp(cb, "w22raw", oRAW, (uint64_t)st[4].TOK * 256 * 4, false);
            recordDs(cb, 1, 22, 256, 512, oRAW, oB22DS, st[4], st[5], true);
            cur = {oB22DS, true, 512};
        }
        tick();
        if (g_to <= 22) return;

        // bottleneck L5: b23-30 split window
        for (int i = 23; i <= 30; ++i) {
            VkDeviceSize out = (i == 30) ? oSS : oG1;
            auto o = originOf(i);
            recordWindowBlock(cb, 2, i, st[5], 512, 16, 2, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 512};
        }
        {   // b30 bridge: pad8 + pool(f16) -> half -> gemm layer4.weight -> e4m3 at L6
            uint32_t Hp = pad8u(st[5].H), Wp = pad8u(st[5].W);
            dPool(cb, A(oSS), A(oPOOL), st[5].H, st[5].W, Wp, Hp, 512, 1u);
            bar(cb);
            dGemm(cb, 2, A(oPOOL), A(poff["block30.layer4.weight"]), A(oBRIDGE),
                  st[6].TOK, 1024, 512, 1, 0, 0, 0, gflags(EPI_E4M3, true), 512, 1024, 1024);
            bar(cb);
            if (T6P > st[6].TOK) {   // zero the padded global rows (fill + transfer barrier)
                vkCmdFillBuffer(cb, dev.buf, oBRIDGE + (uint64_t)st[6].TOK * 1024 * 2,
                                (uint64_t)(T6P - st[6].TOK) * 1024 * 2, 0);
                VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fb.buffer = dev.buf;
                fb.offset = 0;
                fb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb, 0, nullptr);
            }
            cur = {oBRIDGE, true, 1024};
        }
        tick();
        if (g_to <= 30) return;

        for (int i = 31; i <= 38; ++i) {
            VkDeviceSize out = (i == 38) ? oB38 : oG1;
            recordGlobal(cb, 3, i, cur, oBRG, out);
            if (i == 31) dbgCp(cb, "g31", out, T6 * 1024 * 2, true);
            cur = {out, true, 1024};
        }
        tick();
        if (g_to <= 38) return;

        {   // b39: gemm conv_weight -> nearest-x2 crop merge with split_skip*sin -> e4m3
            dGemm(cb, 4, A(cur.off), A(poff["block39.layer0.conv_weight"]), A(oG3),
                  st[6].TOK, 512, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 512, 512);
            bar(cb);
            dUpMerge(cb, A(oG3), A(oSS), A(oMRG39), A(vecOff["block39.layer0.inp_upsample_sin"]), 0,
                     A(oB39), st[5], st[6], 512, 0);
            bar(cb);
            cur = {oB39, true, 512};
        }
        if (g_to <= 39) return;
        for (int i = 40; i <= 47; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, st[5], 512, 16, 2, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 512};
        }
        {   // b48 up-transition L5->L4 + window branched C=256 H=8
            recordUp(cb, 4, 48, 512, 256, cur, oSKIP3, oG2, st[5], st[4]);
            recordWindowBlock(cb, 4, 48, st[4], 256, 8, 1, 0, 0, true, cur, oRAW, oB48);
            cur = {oB48, true, 256};
        }
        if (g_to <= 48) return;
        for (int i = 49; i <= 55; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, st[4], 256, 8, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 256};
        }
        {   // b56 up L4->L3 + window C=128 H=4
            recordUp(cb, 4, 56, 256, 128, cur, oSKIP2, oG2, st[4], st[3]);
            recordWindowBlock(cb, 4, 56, st[3], 128, 4, 1, 0, -4, true, cur, oRAW, oG1);
            cur = {oG1, true, 128};
            dbgCp(cb, "up56", oG1, (uint64_t)st[3].TOK * 128 * 2, true);
        }
        if (g_to <= 56) return;
        for (int i = 57; i <= 61; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, st[3], 128, 4, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 128};
        }
        {   // b62 up L3->L2 + window C=64 H=2
            recordUp(cb, 4, 62, 128, 64, cur, oSKIP1, oG2, st[3], st[2]);
            recordWindowBlock(cb, 4, 62, st[2], 64, 2, 1, 0, 0, true, cur, oRAW, oG1);
            cur = {oG1, true, 64};
            dbgCp(cb, "up62", oG1, (uint64_t)st[2].TOK * 64 * 2, true);
        }
        if (g_to <= 62) return;
        for (int i = 63; i <= 65; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, st[2], 64, 2, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 64};
        }
        {   // b66 up L2->L1 + window plain C=32 H=1
            recordUp(cb, 4, 66, 64, 32, cur, oSKIP0, oG2, st[2], st[1]);
            recordWindowBlock(cb, 4, 66, st[1], 32, 1, 0, 0, 0, true, cur, oRAW, oG1);
            cur = {oG1, true, 32};
            dbgCp(cb, "up66", oG1, (uint64_t)st[1].TOK * 32 * 2, true);
        }
        if (g_to <= 66) return;
        for (int i = 67; i <= 69; ++i) {
            auto o = originOf(i);
            VkDeviceSize out = (i == 69) ? oB69 : oG1;
            recordWindowBlock(cb, 4, i, st[1], 32, 1, 0, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 32};
        }
        tick();
        if (g_to <= 69) return;

        {   // b70: upsample L1->L0, merge up*sin + frs*cos (fp32) -> window plain -> head
            dUpMerge(cb, A(cur.off), A(oFRS), A(oMRG70), A(vecOff["block70.layer0.inp_merge_sin"]),
                     A(vecOff["block70.layer0.inp_merge_cos"]), 0, st[0], st[1], 32, 1);
            bar(cb);
            Cur c70{oMRG70, false, 32};
            recordWindowBlock(cb, 5, 70, st[0], 32, 1, 0, -4, -4, false, c70, oRAW, 0);
            dEw(cb, A(oRAW), 0, 0, 0, A(oG2), st[0].TOK * 32, 2, 0);
            bar(cb);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm1.pipe);
            GemmPush p{A(oG2), A(oHeadW), A(oHEAD), st[0].TOK, 16, 32, 1, 0, 0, 0,
                       gflags(EPI_NONE, false), 32, 16, 16};
            vkCmdPushConstants(cb, pGemm1.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cb, 1, st[0].TOK / 16, 1);
            bar(cb);
            famFlops[5] += 2.0 * st[0].TOK * 16 * 32;
        }
        tick();
    };

    std::printf("record + run: %.0f ms setup\n", msSince(tAll0));

    // ======================= 5. run + family timing ==========================
    VkQueryPool qpool;
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 8;
    VK_CHECK(vkCreateQueryPool(vk.dev, &qpci, nullptr, &qpool));

    {
        VkCommandBuffer cb = beginCmd(vk);
        vkCmdResetQueryPool(cb, qpool, 0, 8);
        recordChain(cb, true, qpool);
        submitWait(vk, cb);
    }
    uint64_t ts[8] = {0};
    vkGetQueryPoolResults(vk.dev, qpool, 0, 8, 8 * sizeof(uint64_t),
                          ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    double totalMs = 0;
    for (int f = 0; f < 6; ++f) {
        double famMs = (ts[f + 1] - ts[f]) * vk.tsPeriod / 1e6;
        totalMs += famMs;
        std::printf("  family %-16s %8.3f ms   (%6.2f GF -> %5.2f TFLOP/s)\n",
                    famName[f], famMs, famFlops[f] / 1e9,
                    famMs > 0 ? famFlops[f] / 1e12 / (famMs / 1e3) : 0.0);
    }
    std::printf("chain total (timestamp): %.3f ms\n", totalMs);
    std::printf("dispatches executed: %ld\n", g_dispCount);

    if (g_bench > 0 && g_to >= 70) {
        VkCommandBuffer cb = beginCmd(vk);
        for (int i = 0; i < g_bench; ++i) recordChain(cb, false, VK_NULL_HANDLE);
        auto sw0 = clk::now();
        submitWait(vk, cb);
        std::printf("steady-state chain: %.3f ms (%d-iter avg)\n", msSince(sw0) / g_bench, g_bench);
    }

    // ======================= 6. boundary dumps ==============================
    struct Dump { std::string name; VkDeviceSize off; VkDeviceSize bytes; bool f16; };
    std::vector<Dump> dumps = {
        {"w0raw", oB0RAW, (uint64_t)st[0].TOK * 32 * 4, false},
        {"frs", oFRS, (uint64_t)st[0].TOK * 32 * 2, true},
        {"ds4", oB4DS, (uint64_t)st[2].TOK * 64 * 2, true},
        {"ds22", oB22DS, (uint64_t)st[5].TOK * 512 * 2, true},
        {"ss", oSS, (uint64_t)st[5].TOK * 512 * 2, true},
        {"bridge", oBRIDGE, T6 * 1024 * 2, true},
        {"g38", oB38, T6 * 1024 * 2, true},
        {"mrg39", oMRG39, (uint64_t)st[5].TOK * 512 * 4, false},
        {"b39", oB39, (uint64_t)st[5].TOK * 512 * 2, true},
        {"b48", oB48, (uint64_t)st[4].TOK * 256 * 2, true},
        {"b69", oB69, (uint64_t)st[1].TOK * 32 * 2, true},
        {"mrg70", oMRG70, (uint64_t)st[0].TOK * 32 * 4, false},
        {"head", oHEAD, (uint64_t)st[0].TOK * 16 * 4, false},
        {"x16", oX16, (uint64_t)st[0].TOK * 16 * 2, true},
        {"xfeats", oX, (uint64_t)st[0].TOK * 16 * 4, false},
    };
    for (auto &d : dbgList) dumps.push_back({d.name, d.off, d.bytes, d.f16});
    VkDeviceSize tot = 0;
    for (auto &d : dumps) tot += d.bytes;
    Buffer rd;
    allocHost(vk, tot, rd);
    {
        VkCommandBuffer cb = beginCmd(vk);
        VkDeviceSize o = 0;
        for (auto &d : dumps) {
            VkBufferCopy c{d.off, o, d.bytes};
            vkCmdCopyBuffer(cb, dev.buf, rd.buf, 1, &c);
            o += d.bytes;
        }
        submitWait(vk, cb);
        VkDeviceSize ro = 0;
        for (auto &d : dumps) {
            std::string p = outDir + "gpu_" + d.name + (d.f16 ? ".f16" : ".bin");
            std::ofstream f(p, std::ios::binary);
            f.write((const char *)rd.mapped + ro, (std::streamsize)d.bytes);
            ro += d.bytes;
        }
    }
    std::printf("dumped %zu boundary tensors to %s in %.0f ms total\n",
                dumps.size(), outDir.c_str(), msSince(tAll0));

    vkDestroyQueryPool(vk.dev, qpool, nullptr);
    freeBuf(vk, rd);
    freeBuf(vk, staging);
    freeBuf(vk, dev);
    std::printf("M9 GPU-side complete — compare via cmp_m9.py\n");
    return 0;
}
