// ============================================================================
// M8 full-chain prototype for Intel Arc Pro B50  --  DLSS5_INTEL  (M8a)
//
// Full 71-block DLSSNR chain at a FIXED 24x12 = 288-token grid (synthetic
// input, console app — no DDA/present). Every block family of the design
// (docs/m6b-graph-design.md SS A/SS D, semantics per reference
// dlss-nr-on-intel/src/ref/nr_model.py):
//
//   stem b0-4 (plain32 window + adapter + ds)        -> C 32 -> 64
//   enc  b5-22 (branched window 64/128/256 + ds)     -> C 64 -> 128 -> 256 -> 512
//   bottleneck b23-30 (split512 window + b30 bridge)
//   global b31-38 (1024 MHA, +-3 cap, scale*sqrt32)  [M7-validated kernels]
//   decoder b39-69 (b39 merge, split512, up-transitions + branched/plain)
//   head b70 (merge + window + folded head GEMM)
//
// Constant-grid note (docs/m8-full-chain.md): the production U-Net changes
// spatial extent per level; M8a runs all blocks at 24x12 so avgpool2 / pad8 /
// nearest_up2 are neutralized (spatial-only). Every GEMM, rounding point,
// residual, publish and skip connection follows nr_model.py exactly.
// Golden: golden_full.py (nr_model.py fork, same neutralizations,
// NR_FUSE_BRANCHED=1 matching the load-time fuse-fold).
//
// Weights: all 649 logical tensors in ONE device-local VkBuffer laid out per
// docs/pack-layout.txt (256-aligned, 291,535,872 B). The staging memcpy does
// the SS B.3 preprocessing: the branched-FFN fuse-fold (same bytes, relaid);
// attn_bias is consumed AS STORED (raw) — the GPU softmax reads it linearly as
// logical [q,k] and the b0 probs validate bit-exact that way (M8a finding).
// Derived fp32 side tables
// (cos_skip/sin/merge vectors, global attn_scale*sqrt(32), folded [32,16] head
// weight) live in the arena region after the pack.
//
// Exit: 0 = GPU-side ok (compare verdicts come from golden_full.py),
//       1 = Vulkan/file/pack error, 2 = rounding unit-kernel mismatch.
// ============================================================================
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
struct SMaxPush { uint64_t a, c; uint32_t m, n; float p0; uint64_t bias; uint32_t hcount; };
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
// M8a — full 71-block chain @ 288 tokens (24x12)
// ===========================================================================
static const uint32_t TOK = 288;      // tokens = 24 rows x 12 cols
static const uint32_t IMG_H = 24, IMG_W = 12;

static int blockIndexOf(const std::string &name) {
    auto dot = name.find('.');
    return std::atoi(name.substr(5, dot - 5).c_str());
}

static int g_to = 70;   // --to N: stop chain after block N (family bisect)
static long g_dispCount = 0;    // dispatch ordinal (debug bisect)
static long g_maxDisp = 1 << 30; // --maxdisp N: no-op all dispatches after the Nth
static bool g_noBias = false;   // --nobias: window softmax w/o attn_bias read
static bool g_dispDbg = false;  // --dispdbg: stderr ordinal per attempted dispatch
static bool g_noTs = false;     // --nots: skip vkCmdWriteTimestamp (ts flake probe)
static long g_barCount = 0;     // barrier ordinal
static long g_splitN = 0;       // --split N: submit+wait every N barriers (driver probe)
static bool g_pv1 = false;      // --pv1: PV gemm batch=1 ablation (crash probe)
static int g_pvMode = 0;        // --pvmode N: PV gemm ablation (1=k32 2=n64 3=bkw 4=altout)
#define DISP_GATE(tag)                                        \
    do {                                                      \
        ++g_dispCount;                                        \
        if (g_dispDbg) std::fprintf(stderr, "[disp %ld] %s\n", g_dispCount, tag); \
        if (g_dispCount > g_maxDisp) return;                  \
    } while (0)

int main(int argc, char **argv) {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    g_hasF16C = (cpuInfo[2] & (1 << 29)) != 0;
    // --to N: record/run blocks 0..N only (family bisect + bring-up)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) g_to = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--maxdisp") == 0 && i + 1 < argc) g_maxDisp = std::atol(argv[++i]);
        if (std::strcmp(argv[i], "--nobias") == 0) g_noBias = true;
        if (std::strcmp(argv[i], "--dispdbg") == 0) g_dispDbg = true;
        if (std::strcmp(argv[i], "--nots") == 0) g_noTs = true;
        if (std::strcmp(argv[i], "--split") == 0 && i + 1 < argc) g_splitN = std::atol(argv[++i]);
        if (std::strcmp(argv[i], "--pv1") == 0) g_pv1 = true;
        if (std::strcmp(argv[i], "--pvmode") == 0 && i + 1 < argc) g_pvMode = std::atoi(argv[++i]);
    }
    std::printf("M8 full-chain prototype — Arc Pro B50\n");
    std::printf("CPU F16C: %s\n", g_hasF16C ? "yes" : "no (software RNE)");

    VkCtx vk{};
    vkInit(vk);

    std::string dir = exeDir();
    std::string repo = "C:\\Users\\AI\\Desktop\\DLSS5_INTEL";
    std::string outDir = dir + "out\\";
    CreateDirectoryA(outDir.c_str(), nullptr);

    auto tAll0 = clk::now();

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
    uint64_t packTotal = 291535872ull;   // docs/pack-layout.txt (256-aligned total)
    std::map<std::string, uint64_t> poff;
    for (auto &e : pack) poff[e.name] = e.off;
    std::printf("pack: %zu tensors (%.2f MiB)\n", pack.size(), packTotal / 1048576.0);
    bool packOk = poff["block31.layer0.weight"] == 45084928ull &&
                  poff["block39.layer0.conv_weight"] == 246448384ull &&
                  poff["block0.layer0.input_adapter_weight"] == 8960ull;  // after attn_bias/attn_cos_skip/attn_scale/ffn_cos_skip
    if (!packOk) { std::fprintf(stderr, "pack offset mismatch vs pack-layout.txt\n"); return 1; }
    std::printf("pack offsets cross-checked vs docs/pack-layout.txt: OK\n");

    auto tname = [](const char *fmt, int i) -> std::string {
        char b[160]; std::snprintf(b, sizeof b, fmt, i); return b;
    };

    // ======================= 2. arena layout =================================
    // Arena offsets are ABSOLUTE device offsets (packTotal + relative) so the
    // dispatch BDA lambda A(o) = A0 + o addresses them correctly alongside the
    // pack-relative poff[] weight offsets. (Run-4 dumps were all-zero because
    // A(o) without the pack base aliased the weights region.)
    struct Ar {
        VkDeviceSize base = 0;   // set to packTotal before first alloc
        VkDeviceSize off = 0;
        VkDeviceSize alloc(VkDeviceSize bytes) {
            VkDeviceSize a = (off + 255) & ~(VkDeviceSize)255;
            off = a + bytes;
            return base + a;
        }
    } ar;
    ar.base = packTotal;
    std::map<std::string, VkDeviceSize> vecOff;   // fp32 vector table (4 KB slots)
    auto needVec = [&](const std::string &n) {
        if (!vecOff.count(n)) { VkDeviceSize o = ar.alloc(4096); vecOff[n] = o; }
        return vecOff[n];
    };
    VkDeviceSize oHeadW = ar.alloc(1024);   // folded [32,16] f16 head weight

    auto slot = [&](VkDeviceSize bytes) { return ar.alloc(bytes); };
    VkDeviceSize oX = slot(TOK * 16 * 4);
    VkDeviceSize oX16 = slot(TOK * 16 * 2);
    VkDeviceSize oADA = slot(TOK * 32 * 4);
    VkDeviceSize oG1 = slot(TOK * 1024 * 4);
    VkDeviceSize oG2 = slot(TOK * 1024 * 4);
    VkDeviceSize oG3 = slot(TOK * 1024 * 4);
    VkDeviceSize oG4 = slot(TOK * 1024 * 4);
    VkDeviceSize oRAW = slot(TOK * 1024 * 4);
    // window attention set (Wmax=8 -> 512 rows; Cmax=512; batchmax W*H=128)
    VkDeviceSize oWIN = slot(512 * 512 * 2);
    VkDeviceSize oPROJW = slot(512 * 1536 * 4);
    VkDeviceSize oQW = slot(128 * 64 * 32 * 2), oKW = slot(128 * 64 * 32 * 2), oVW = slot(128 * 64 * 32 * 2);
    VkDeviceSize oSCW = slot(128 * 64 * 64 * 4), oPRW = slot(128 * 64 * 64 * 2);
    VkDeviceSize oMGW = slot(128 * 64 * 32 * 4), oATW = slot(512 * 512 * 2), oABW = slot(512 * 512 * 4);
    // global set (M7 sizes)
    VkDeviceSize oHG = slot(TOK * 4096 * 2);
    VkDeviceSize oPROJG = slot(TOK * 3072 * 4);
    VkDeviceSize oQG = slot(TOK * 1024 * 2), oKG = slot(TOK * 1024 * 2), oVG = slot(TOK * 1024 * 2);
    VkDeviceSize oSCG = slot((VkDeviceSize)32 * TOK * TOK * 4), oPRG = slot((VkDeviceSize)32 * TOK * TOK * 2);
    VkDeviceSize oMGG = slot(TOK * 1024 * 4), oATG = slot(TOK * 1024 * 2);
    VkDeviceSize oABG = slot(TOK * 1024 * 4), oBRG = slot(TOK * 1024 * 4);
    VkDeviceSize oBOG = slot(TOK * 1024 * 4), oB16G = slot(TOK * 1024 * 2);
    // dedicated boundary / skip slots
    VkDeviceSize oB0RAW = slot(TOK * 32 * 4), oFRS = slot(TOK * 32 * 2);
    VkDeviceSize oSKIP0 = slot(TOK * 32 * 2), oSKIP1 = slot(TOK * 64 * 2);
    VkDeviceSize oSKIP2 = slot(TOK * 128 * 2), oSKIP3 = slot(TOK * 256 * 2);
    VkDeviceSize oSS = slot(TOK * 512 * 2);
    VkDeviceSize oB4DS = slot(TOK * 64 * 2), oB22DS = slot(TOK * 512 * 2);
    VkDeviceSize oB38 = slot(TOK * 1024 * 2), oB39 = slot(TOK * 512 * 2), oB48 = slot(TOK * 256 * 2);
    VkDeviceSize oB69 = slot(TOK * 32 * 2);
    VkDeviceSize oMRG70 = slot(TOK * 32 * 4), oHEAD = slot(TOK * 16 * 4);
    VkDeviceSize oDBG = slot(12800 * 1024);   // temp b31 global probes

    // Pre-allocate every fp32 vector slot BEFORE the buffers are sized below:
    // the upload copy spans packTotal + ar.off, so ar.off must be final here
    // (the staging writes happen after, but they only fill already-fixed slots).
    auto preVec = [&](const std::string &n) { if (poff.count(n)) needVec(n); };
    for (int i = 0; i <= 70; ++i) {
        if (i == 39) { preVec("block39.layer0.inp_upsample_sin"); continue; }
        if (i >= 31 && i <= 38) {
            preVec(tname("block%d.layer1.ffn_cos_skip", i));
            preVec(tname("block%d.layer4.attn_cos_skip", i));
            needVec(tname("block%d.layer2.attn_scale.q32", i));   // derived: not in poff
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

    VkDeviceSize arenaStart = packTotal;   // arena lives after the weight pack
    VkDeviceSize totalBytes = arenaStart + ar.off;
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
            // [G,4,G,32,32] -> [G, G*32, 128]  (transpose(0,2,3,1,4), same bytes)
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
        } else {
            std::memcpy(dst, src, e.bytes);
        }
    }
    // derived fp32 vector tables
    auto placeVecF16 = [&](const std::string &n) {
        auto it = poff.find(n);
        if (it == poff.end()) { std::fprintf(stderr, "missing vec tensor %s\n", n.c_str()); return; }
        const Tensor &t = wf.tensors[n];
        VkDeviceSize o = needVec(n);
        const uint16_t *s = (const uint16_t *)(sp + it->second);
        float *d = (float *)(sp + o);   // arena offsets are absolute (packTotal base)
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
    // global per-head Q scale = attn_scale * sqrt(32), fp32 first (SS A.6.2)
    for (int i = 31; i <= 38; ++i) {
        std::string n = tname("block%d.layer2.attn_scale", i);
        const Tensor &t = wf.tensors[n];
        const float *s = (const float *)(sp + poff[n]);
        VkDeviceSize o = needVec(n + ".q32");
        float *d = (float *)(sp + o);
        float sq = std::sqrt(32.0f);
        for (uint64_t j = 0; j < t.numel(); ++j) d[j] = s[j] * sq;
    }
    // folded head weight [32,16]: rows 0-15 = out_gain, 16-31 = out_conv, cols 4-15 = 0
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
    double upMs = msSince(tUp0);
    std::printf("weight upload: %.1f MB in %.1f ms (%.2f GB/s incl. preprocessing)\n",
                (packTotal + ar.off) / 1048576.0, upMs,
                (packTotal + ar.off) / 1073741824.0 / (upMs / 1e3));

    // ======================= 4. input + pipelines ==========================
    std::vector<float> x((size_t)TOK * 16);
    const double PI = 3.14159265358979323846;
    for (uint32_t t = 0; t < TOK; ++t)
        for (uint32_t c = 0; c < 16; ++c) {
            double v = 0.50 * std::sin(2 * PI * (0.011 * t + 0.031 * c)) +
                       0.30 * std::cos(2 * PI * (0.071 * t - 0.017 * c) + 0.7) +
                       0.15 * std::sin(2 * PI * (0.137 * t + 0.053 * c) + 1.9);
            x[(size_t)t * 16 + c] = (float)v;
        }
    {
        std::string p = outDir + "x.bin";
        std::ofstream f(p, std::ios::binary);
        f.write((const char *)x.data(), (std::streamsize)(x.size() * 4));
        char *sx = sp + oX;
        std::memcpy(sx, x.data(), x.size() * 4);
        uint16_t *sx16 = (uint16_t *)(sp + oX16);
        for (size_t i = 0; i < x.size(); ++i) sx16[i] = f32_to_f16(x[i]);
        VkCommandBuffer cbx = beginCmd(vk);
        VkBufferCopy cxx{oX, oX, TOK * 16 * 4};
        VkBufferCopy cx16{oX16, oX16, TOK * 16 * 2};
        vkCmdCopyBuffer(cbx, staging.buf, dev.buf, 1, &cxx);
        vkCmdCopyBuffer(cbx, staging.buf, dev.buf, 1, &cx16);
        submitWait(vk, cbx);
        std::printf("x upload ok\n");
    }

    Pipeline pGemm, pGemm1, pCos, pCosW, pSmax, pEw, pPart, pTrans, pGather, pMerge;
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
    std::printf("pipelines ok\n");

    VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, dev.buf};
    uint64_t A0 = vkGetBufferDeviceAddress(vk.dev, &bdai);
    auto A = [&](VkDeviceSize o) { return A0 + (uint64_t)o; };

    const char *famName[] = {"stem(0-4)", "enc(5-22)", "bottleneck(23-30)",
                             "global(31-38)", "decoder(39-69)", "head(70)"};
    double famFlops[6] = {0, 0, 0, 0, 0, 0};

    // dispatch helpers ------------------------------------------------------
    auto bar = [&](VkCommandBuffer &cb) {
        fullBarrier(cb, dev.buf);
        if (g_splitN > 0 && (++g_barCount % g_splitN) == 0) {
            submitWait(vk, cb);   // split-submit probe: bound in-flight work
            cb = beginCmd(vk);
        }
    };
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
    auto dPart = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint32_t C,
                     uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
        DISP_GATE("dPart");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
        uint32_t hp8 = (IMG_H + padTop + 7) / 8;
        PartPush p{a, c, IMG_H, IMG_W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
        vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
    };
    auto dGather = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c,
                       uint64_t h, uint32_t C, uint32_t padTop, uint32_t padLeft, uint32_t wp8,
                       bool pub, bool b16) {
        DISP_GATE("dGather");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGather.pipe);
        GatherPush p{a, b, d, c, h, IMG_H, IMG_W, C, padTop, padLeft, wp8,
                     (pub ? 1u : 0u) | (b16 ? 2u : 0u)};
        vkCmdPushConstants(cb, pGather.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, TOK, 1, 1);
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
        CosPush p{a, c, sc, TOK * 32, kind};
        vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (TOK * 32 + 31) / 32, 1, 1);
    };
    auto dSmaxW = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint32_t W, uint32_t H,
                      uint64_t bias) {
        DISP_GATE("dSmaxW");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c, W * H * 64, 64, 0.0f, bias, H};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (W * H * 64 + 31) / 32, 1, 1);
    };
    auto dSmaxG = [&](VkCommandBuffer cb, uint64_t a, uint64_t c) {
        DISP_GATE("dSmaxG");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c, 32 * TOK, TOK, 3.0f, 0, 0};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, 32 * TOK, 1, 1);
    };
    auto dMerge = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                      uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind) {
        DISP_GATE("dMerge");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pMerge.pipe);
        MergePush p{a, b, c, d, e, h, n, ch, kind};
        vkCmdPushConstants(cb, pMerge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };

    auto winGeo = [](int oy, int ox, uint32_t &padTop, uint32_t &padLeft, uint32_t &W) {
        padTop = (uint32_t)(-oy);
        padLeft = (uint32_t)(-ox);
        uint32_t hp8 = (IMG_H + padTop + 7) / 8;
        uint32_t wp8 = (IMG_W + padLeft + 7) / 8;
        W = hp8 * wp8;
    };

    struct Cur { VkDeviceSize off; bool is16; uint32_t C; };

    // ---- window attention shared tail -------------------------------------
    // fam: 0 plain, 1 branched, 2 split (window families). famIdx = FLOP bucket.
    auto recordWindowAttn = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int H, int fam,
                                int oy, int ox, VkDeviceSize ffnOff, bool ffn16,
                                VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish) {
        uint32_t padTop, padLeft, W;
        winGeo(oy, ox, padTop, padLeft, W);
        uint32_t wp8 = (IMG_W + padLeft + 7) / 8;
        auto lay = [&](const char *fmt) { return tname(fmt, idx); };
        std::string qkvN = lay(fam == 2 ? "block%d.layer2.qkv_weight" : "block%d.layer0.qkv_weight");
        std::string scN = lay(fam == 2 ? "block%d.layer2.attn_scale" : "block%d.layer0.attn_scale");
        std::string biasN = lay(fam == 2 ? "block%d.layer2.attn_bias" : "block%d.layer0.attn_bias");
        std::string projN = lay(fam == 2 ? "block%d.layer3.projection_weight" : "block%d.layer0.projection_weight");
        std::string acosN = lay(fam == 2 ? "block%d.layer3.attn_cos_skip" : "block%d.layer0.attn_cos_skip");

        dPart(cb, A(ffnOff), A(oWIN), (uint32_t)C, padTop, padLeft, wp8, ffn16);
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
        dSmaxW(cb, A(oSCW), A(oPRW), W, H, g_noBias ? 0 : A(poff[biasN]));
        bar(cb);
        if (g_pvMode == 1)
            dGemm(cb, famIdx, A(oPRW), A(oVW), A(oMGW), 64, 32, 32, 1u,
                  0, 0, 0, gflags(EPI_NONE, false), 64, 32, 32);
        else if (g_pvMode == 2)
            dGemm(cb, famIdx, A(oPRW), A(oSCW), A(oMGW), 64, 64, 64, 1u,
                  0, 0, 0, gflags(EPI_NONE, false), 64, 64, 64);
        else if (g_pvMode == 3)
            dGemm(cb, famIdx, A(oPRW), A(oKW), A(oMGW), 64, 32, 64, g_pv1 ? 1u : (uint32_t)(W * H),
                  64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
        else if (g_pvMode == 4)
            dGemm(cb, famIdx, A(oPRW), A(oVW), A(oG1), 64, 32, 64, g_pv1 ? 1u : (uint32_t)(W * H),
                  64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
        else
            dGemm(cb, famIdx, A(oPRW), A(oVW), A(oMGW), 64, 32, 64, g_pv1 ? 1u : (uint32_t)(W * H),
                  64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
        bar(cb);
        dTrans(cb, A(oMGW), A(oATW), W, H);
        bar(cb);
        dGemm(cb, famIdx, A(oATW), A(poff[projN]), A(oABW), 64, C, C, W,
              64 * C, 0, 64 * C, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dGather(cb, A(oABW), A(ffnOff), A(vecOff[acosN]), A(rawOff), A(publish ? pubOff : rawOff),
                (uint32_t)C, padTop, padLeft, wp8, publish, ffn16);
        bar(cb);
    };

    // FFN + attention for one window block; writes raw (always) + pub (if publish).
    // cur: input. fam: 0 plain, 1 branched, 2 split.
    auto recordWindowBlock = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int H, int fam,
                                 int oy, int ox, bool publish, Cur &cur,
                                 VkDeviceSize rawOff, VkDeviceSize pubOff) {
        uint32_t G = C / 32;
        // ---- FFN
        VkDeviceSize x16 = cur.is16 ? cur.off : oG2;
        if (!cur.is16) { dEw(cb, A(cur.off), 0, 0, 0, A(oG2), TOK * C, 2, 0); bar(cb); }
        if (fam == 0) {
            // plain: branch = e4m3(gate(x@w1)) @ w2 ; ffn raw residual
            dGemm(cb, famIdx, A(x16), A(poff[tname("block%d.layer0.weight1", idx)]), A(oG3),
                  TOK, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
            bar(cb);
            dGemm(cb, famIdx, A(oG3), A(poff[tname("block%d.layer0.weight2", idx)]), A(oG4),
                  TOK, C, 4 * C, 1, 0, 0, 0, gflags(EPI_NONE, false), 4 * C, C, C);
            bar(cb);
            // ffn_out = branch + cur * ffn_cos   (fp32 raw)
            if (cur.is16)
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOK * C, 4, C);
            else
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOK * C, 1, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, C, H, fam, oy, ox, oG4, false, rawOff, pubOff, publish);
        } else if (fam == 1) {
            // branched: fused fold (load-time). per oh: GATE_E4M3 expand, E4M3 project
            uint64_t expBase = A(poff[tname("block%d.layer0.ffn_expand_weight", idx)]);
            uint64_t prjBase = A(poff[tname("block%d.layer0.ffn_branch_projection_weight", idx)]);
            for (uint32_t oh = 0; oh < G; ++oh) {
                dGemm(cb, famIdx, A(x16), expBase + (uint64_t)oh * C * 128 * 2, A(oG3),
                      TOK, 128, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 128, 128);
                bar(cb);
                dGemm(cb, famIdx, A(oG3), prjBase + (uint64_t)oh * 128 * 32 * 2, A(oG4 + oh * 32 * 2),
                      TOK, 32, 128, 1, 0, 0, 0, gflags(EPI_E4M3, true), 128, 32, C);
                bar(cb);
            }
            dGemm(cb, famIdx, A(oG4), A(poff[tname("block%d.layer0.ffn_output_projection_weight", idx)]),
                  A(oG3), TOK, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
            bar(cb);
            // branched FFN residual IS e4m3-published (nr_model.branched_window_block)
            dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]),
                A(oG2), TOK * C, 5, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, C, H, fam, oy, ox, oG2, true, rawOff, pubOff, publish);
        } else {
            // split: first proj E4M3; per-group GATE expand / project; core E4M3; weight3
            dGemm(cb, famIdx, A(x16), A(poff[tname("block%d.layer0.first_projection_weight", idx)]),
                  A(oG2), TOK, C, C, 1, 0, 0, 0, gflags(EPI_E4M3, true), C, C, C);
            bar(cb);
            dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer0.group_expand_weight", idx)]),
                  A(oG3), TOK, 256, 64, 8, 64, 64 * 256, 288 * 256,
                  gflags(EPI_GATE, true), C, 256, 256);
            bar(cb);
            dGemm(cb, famIdx, A(oG3), A(poff[tname("block%d.layer0.group_project_weight", idx)]),
                  A(oG4), TOK, 64, 256, 8, 288 * 256, 256 * 64, 64,
                  gflags(EPI_NONE, false), 256, 64, C);
            bar(cb);
            dEw(cb, A(oG4), 0, 0, 0, A(oG2), TOK * C, 3, 0);   // core16 = e4m3(core)
            bar(cb);
            dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer1.weight3", idx)]), A(oG3),
                  TOK, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
            bar(cb);
            dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer1.ffn_cos_skip", idx)]),
                0, TOK * C, 4, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, C, H, fam, oy, ox, oG3, false, rawOff, pubOff, publish);
        }
    };

    // downsample transition (pool neutralized): raw -> e4m3 -> gemm w0 (E4M3 out)
    auto recordDs = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int Cn,
                        VkDeviceSize rawOff, VkDeviceSize outOff) {
        dEw(cb, A(rawOff), 0, 0, 0, A(oG2), TOK * C, 3, 0);
        bar(cb);
        dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer0.weight0", idx)]), A(outOff),
              TOK, Cn, C, 1, 0, 0, 0, gflags(EPI_E4M3, true), C, Cn, Cn);
        bar(cb);
    };

    // upsample transition (neutralized): gemm w0 -> merge with skip*sin -> e4m3
    auto recordUp = [&](VkCommandBuffer &cb, int famIdx, int idx, int Ci, int Co,
                        Cur &cur, VkDeviceSize skipOff, VkDeviceSize out16Off) {
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight0", idx)]), A(oG3),
              TOK, Co, Ci, 1, 0, 0, 0, gflags(EPI_NONE, false), Ci, Co, Co);
        bar(cb);
        dMerge(cb, A(oG3), A(skipOff), A(oG4), A(vecOff[tname("block%d.layer0.sin", idx)]), 0,
               A(out16Off), TOK * Co, Co, 0);
        bar(cb);
    };

    // global block (M7 path; input f16)
    auto recordGlobal = [&](VkCommandBuffer &cb, int famIdx, int idx, Cur &cur,
                            VkDeviceSize rawOff, VkDeviceSize pubOff) {
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight", idx)]), A(oHG),
              TOK, 4096, 1024, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), 1024, 4096, 4096);
        bar(cb);
        if (idx == 31 && g_dispDbg) {   // TEMP probe: block input + gate out
            VkBufferCopy c{cur.off, oDBG + 6488064, TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
            VkBufferCopy c2{oHG, oDBG + 7077888, TOK * 4096 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c2);
        }
        dGemm(cb, famIdx, A(oHG), A(poff[tname("block%d.layer1.weight", idx)]), A(oG3),
              TOK, 1024, 4096, 1, 0, 0, 0, gflags(EPI_NONE, false), 4096, 1024, 1024);
        bar(cb);
        dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer1.ffn_cos_skip", idx)]),
            0, TOK * 1024, 4, 1024);
        bar(cb);
        dEw(cb, A(oG3), 0, 0, 0, A(oG2), TOK * 1024, 2, 0);
        bar(cb);
        if (idx == 31 && g_dispDbg) {   // TEMP probe: ffn_out fp32
            VkBufferCopy c{oG3, oDBG + 9437184, TOK * 1024 * 4};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
        }
        dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer2.qkv_weight", idx)]), A(oPROJG),
              TOK, 3072, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 3072, 3072);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oQG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 0);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oKG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 1);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oVG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 2);
        bar(cb);
        if (idx == 31 && g_dispDbg) {   // TEMP probe: q16/k16/v16
            VkBufferCopy c{oQG, oDBG + 10616832, TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
            VkBufferCopy c2{oKG, oDBG + 11206656, TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c2);
            VkBufferCopy c3{oVG, oDBG + 11796480, TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c3);
        }
        dGemm(cb, famIdx, A(oQG), A(oKG), A(oSCG), TOK, TOK, 32, 32,
              32, 32, TOK * TOK, gflags(EPI_NONE, false) | F_TRANSPOSE, 1024, 1024, TOK);
        bar(cb);
        dSmaxG(cb, A(oSCG), A(oPRG));
        bar(cb);
        dGemm(cb, famIdx, A(oPRG), A(oVG), A(oMGG), TOK, 32, TOK, 32,
              TOK * TOK, 32, 32, gflags(EPI_NONE, false), TOK, 1024, 1024);
        bar(cb);
        dEw(cb, A(oMGG), 0, 0, 0, A(oATG), TOK * 1024, 3, 0);
        bar(cb);
        if (idx == 31 && g_dispDbg) {   // TEMP probe: attended16 (kept at fixed slot)
            VkBufferCopy c{oATG, oDBG + 5308416, TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
        }
        dGemm(cb, famIdx, A(oATG), A(poff[tname("block%d.layer4.projection_weight", idx)]), A(oABG),
              TOK, 1024, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 1024, 1024);
        bar(cb);
        dEw(cb, A(oABG), A(oG3), A(oBRG), A(vecOff[tname("block%d.layer4.attn_cos_skip", idx)]),
            A(pubOff), TOK * 1024, 0, 1024);
        bar(cb);
        if (idx >= 31 && idx <= 38 && g_dispDbg) {   // TEMP probe: per-block publish
            VkBufferCopy c{pubOff, oDBG + (VkDeviceSize)(idx - 31) * (TOK * 1024 * 2), TOK * 1024 * 2};
            vkCmdCopyBuffer(cb, dev.buf, dev.buf, 1, &c);
        }
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

    // ---- the chain ---------------------------------------------------------
    auto recordChain = [&](VkCommandBuffer &cb, bool timed, VkQueryPool qp) {
        int tq = 0;
        auto tick = [&]() {
            if (timed && !g_noTs) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, (uint32_t)tq++);
        };
        Cur cur{oADA, false, 32};

        tick();                                                    // fam boundary: start
        // adapter GEMM: x16 @ input_adapter_weight -> fp32
        dGemm(cb, 0, A(oX16), A(poff["block0.layer0.input_adapter_weight"]), A(oADA),
              TOK, 32, 16, 1, 0, 0, 0, gflags(EPI_NONE, false), 16, 32, 32);
        bar(cb);
        if (g_to < 0) return;
        // b0 (plain, publish=false, raw -> dedicated; frs = e4m3(raw))
        {
            Cur c0 = cur;
            recordWindowBlock(cb, 0, 0, 32, 1, 0, 0, 0, false, c0, oB0RAW, 0);
            cur = {oB0RAW, false, 32};
            dEw(cb, A(oB0RAW), 0, 0, 0, A(oFRS), TOK * 32, 3, 0);
            bar(cb);
        }
        if (g_to <= 0) return;
        for (int i = 1; i <= 3; ++i) {
            VkDeviceSize out = (i == 3) ? oSKIP0 : oG1;
            recordWindowBlock(cb, 0, i, 32, 1, 0, originOf(i).first, originOf(i).second, true,
                              cur, oRAW, out);
            cur = {out, true, 32};
        }
        if (g_to <= 3) return;
        {   // b4 + ds
            Cur c4 = cur;
            recordWindowBlock(cb, 0, 4, 32, 1, 0, -4, 0, false, c4, oRAW, 0);
            recordDs(cb, 0, 4, 32, 64, oRAW, oB4DS);
            cur = {oB4DS, true, 64};
        }
        tick();                                                    // post-stem
        if (g_to <= 4) return;

        const int encC[] = {0, 64, 64, 64, 64, 128, 128, 128, 128, 128, 256, 256, 256, 256, 256, 256, 256, 256};
        for (int i = 5; i <= 22; ++i) {
            int C = (i <= 8) ? 64 : (i <= 14) ? 128 : 256;
            int H = C / 32;
            bool isDs = (i == 8 || i == 14 || i == 22);
            auto o = originOf(i);
            if (!isDs) {
                VkDeviceSize out = (i == 7) ? oSKIP1 : (i == 13) ? oSKIP2 : (i == 21) ? oSKIP3 : oG1;
                recordWindowBlock(cb, 1, i, C, H, 1, o.first, o.second, true, cur, oRAW, out);
                cur = {out, true, (uint32_t)C};
            } else {
                int Cn = C * 2;
                VkDeviceSize out = (i == 22) ? oB22DS : oG1;
                Cur ci = cur;
                recordWindowBlock(cb, 1, i, C, H, 1, o.first, o.second, false, ci, oRAW, 0);
                recordDs(cb, 1, i, C, Cn, oRAW, out);
                cur = {out, true, (uint32_t)Cn};
            }
        }
        (void)encC;
        tick();                                                    // post-enc
        if (g_to <= 22) return;

        for (int i = 23; i <= 30; ++i) {
            auto o = originOf(i);
            VkDeviceSize out = (i == 30) ? oSS : oG1;
            recordWindowBlock(cb, 2, i, 512, 16, 2, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 512};
        }
        {   // b30 bridge: e4m3(pad8(x)) @ layer4.weight -> e4m3 (pool neutralized)
            dGemm(cb, 2, A(oSS), A(poff["block30.layer4.weight"]), A(oG1),
                  TOK, 1024, 512, 1, 0, 0, 0, gflags(EPI_E4M3, true), 512, 1024, 1024);
            bar(cb);
            cur = {oG1, true, 1024};
        }
        tick();                                                    // post-bottleneck
        if (g_to <= 30) return;

        for (int i = 31; i <= 38; ++i) {
            VkDeviceSize out = (i == 38) ? oB38 : oG1;
            recordGlobal(cb, 3, i, cur, oBRG, out);
            cur = {out, true, 1024};
        }
        tick();                                                    // post-global

        {   // b39: gemm conv_weight -> merge with split_skip*sin -> e4m3
            dGemm(cb, 4, A(cur.off), A(poff["block39.layer0.conv_weight"]), A(oG3),
                  TOK, 512, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 512, 512);
            bar(cb);
            dMerge(cb, A(oG3), A(oSS), A(oG4), A(vecOff["block39.layer0.inp_upsample_sin"]), 0,
                   A(oB39), TOK * 512, 512, 0);
            bar(cb);
            cur = {oB39, true, 512};
        }
        for (int i = 40; i <= 47; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, 512, 16, 2, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 512};
        }
        {   // b48 up-transition + window branched C=256 H=8
            recordUp(cb, 4, 48, 512, 256, cur, oSKIP3, oG2);
            cur = {oG2, true, 256};
            recordWindowBlock(cb, 4, 48, 256, 8, 1, 0, 0, true, cur, oRAW, oB48);
            cur = {oB48, true, 256};
        }
        for (int i = 49; i <= 55; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, 256, 8, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 256};
        }
        {   // b56
            recordUp(cb, 4, 56, 256, 128, cur, oSKIP2, oG2);
            cur = {oG2, true, 128};
            recordWindowBlock(cb, 4, 57 - 1, 128, 4, 1, 0, -4, true, cur, oRAW, oG1); // originOf(56)=(0,-4)
            cur = {oG1, true, 128};
        }
        for (int i = 57; i <= 61; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, 128, 4, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 128};
        }
        {   // b62
            recordUp(cb, 4, 62, 128, 64, cur, oSKIP1, oG2);
            cur = {oG2, true, 64};
            recordWindowBlock(cb, 4, 62, 64, 2, 1, 0, 0, true, cur, oRAW, oG1);
            cur = {oG1, true, 64};
        }
        for (int i = 63; i <= 65; ++i) {
            auto o = originOf(i);
            recordWindowBlock(cb, 4, i, 64, 2, 1, o.first, o.second, true, cur, oRAW, oG1);
            cur = {oG1, true, 64};
        }
        {   // b66
            recordUp(cb, 4, 66, 64, 32, cur, oSKIP0, oG2);
            cur = {oG2, true, 32};
            recordWindowBlock(cb, 4, 66, 32, 1, 0, 0, 0, true, cur, oRAW, oG1);
            cur = {oG1, true, 32};
        }
        for (int i = 67; i <= 69; ++i) {
            auto o = originOf(i);
            VkDeviceSize out = (i == 69) ? oB69 : oG1;
            recordWindowBlock(cb, 4, i, 32, 1, 0, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 32};
        }
        tick();                                                    // post-decoder
        if (g_to <= 69) return;

        {   // b70: pre-merge -> window plain (no publish) -> head
            dMerge(cb, A(cur.off), A(oFRS), A(oMRG70), A(vecOff["block70.layer0.inp_merge_sin"]),
                   A(vecOff["block70.layer0.inp_merge_cos"]), 0, TOK * 32, 32, 1);
            bar(cb);
            Cur c70{oMRG70, false, 32};
            recordWindowBlock(cb, 5, 70, 32, 1, 0, -4, -4, false, c70, oRAW, 0);
            dEw(cb, A(oRAW), 0, 0, 0, A(oG2), TOK * 32, 2, 0);
            bar(cb);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm1.pipe);
            GemmPush p{A(oG2), A(oHeadW), A(oHEAD), TOK, 16, 32, 1, 0, 0, 0,
                       gflags(EPI_NONE, false), 32, 16, 16};
            vkCmdPushConstants(cb, pGemm1.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cb, 1, TOK / 16, 1);
            bar(cb);
            famFlops[5] += 2.0 * TOK * 16 * 32;
        }
        tick();                                                    // end
    };

    std::printf("record + run: %.0f ms setup\n", msSince(tAll0));

    // ======================= 5. correctness run + family timing ==============
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
    uint64_t ts[8];
    {
        VkResult qr = vkGetQueryPoolResults(vk.dev, qpool, 0, 8, 8 * sizeof(uint64_t),
                                            ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (qr != VK_SUCCESS) {
            std::fprintf(stderr, "warn: batched ts read -> %d; per-query\n", (int)qr);
            for (uint32_t q = 0; q < 8; ++q) {
                VkResult r = vkGetQueryPoolResults(vk.dev, qpool, q, 1, sizeof(uint64_t),
                                                   ts + q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                if (r != VK_SUCCESS) { std::fprintf(stderr, "  ts[%u] NOT-READY\n", q); ts[q] = q ? ts[q - 1] : 0; }
            }
        }
    }
    double famMs[6], totalMs = 0;
    for (int f = 0; f < 6; ++f) {
        famMs[f] = (ts[f + 1] - ts[f]) * vk.tsPeriod / 1e6;
        totalMs += famMs[f];
        std::printf("  family %-16s %8.3f ms   (%6.2f GF -> %5.2f TFLOP/s)\n",
                    famName[f], famMs[f], famFlops[f] / 1e9,
                    famMs[f] > 0 ? famFlops[f] / 1e12 / (famMs[f] / 1e3) : 0.0);
    }
    std::printf("chain total (timestamp): %.3f ms -> %.2f TFLOP/s effective @288tok\n",
                totalMs, (famFlops[0] + famFlops[1] + famFlops[2] + famFlops[3] +
                          famFlops[4] + famFlops[5]) / 1e12 / (totalMs / 1e3));
    std::printf("dispatches executed: %ld (maxdisp %ld)\n", g_dispCount, g_maxDisp);

    // steady state: 20 back-to-back chains, no timestamps (full chain only)
    if (g_to >= 70) {
        VkCommandBuffer cb = beginCmd(vk);
        for (int i = 0; i < 20; ++i) recordChain(cb, false, VK_NULL_HANDLE);
        auto sw0 = clk::now();
        submitWait(vk, cb);
        double steady = msSince(sw0) / 20.0;
        double allF = famFlops[0] + famFlops[1] + famFlops[2] + famFlops[3] + famFlops[4] + famFlops[5];
        std::printf("steady-state chain: %.3f ms (20-iter avg) -> %.2f TFLOP/s effective\n",
                    steady, allF / 1e12 / (steady / 1e3));
    }

    // ======================= 6. boundary dumps ==============================
    if (g_dispDbg) {   // run-5 debug: verify arena contents reached the device
        struct Dbg { const char *name; VkDeviceSize off; VkDeviceSize bytes; };
        std::vector<Dbg> dbgs = {
            {"dbg_x", oX, TOK * 16 * 4}, {"dbg_x16", oX16, TOK * 16 * 2},
            {"dbg_ada", oADA, TOK * 32 * 4}, {"dbg_g2", oG2, TOK * 1024 * 2},
            {"dbg_b4ds", oB4DS, TOK * 64 * 2},
            {"dbg_b0ffn", oG4, TOK * 32 * 4}, {"dbg_b0raw", oB0RAW, TOK * 32 * 4},
            {"dbg_b0proj", oPROJW, 512 * 96 * 4}, {"dbg_b0sc", oSCW, 512 * 64 * 4},
            {"dbg_b0pr", oPRW, 512 * 64 * 2}, {"dbg_b0mg", oMGW, 512 * 32 * 4},
            {"dbg_b0at", oATW, 512 * 32 * 2}, {"dbg_b0ab", oABW, 512 * 32 * 4},
            {"dbg_b1raw", oRAW, TOK * 32 * 4}, {"dbg_b1pub", oG1, TOK * 32 * 2},
            {"dbg_w1b1", poff["block1.layer0.weight1"], 32 * 128 * 2},
            {"dbg_w1b2", poff["block2.layer0.weight1"], 32 * 128 * 2},
            {"dbg_qkvb2", poff["block2.layer0.qkv_weight"], 32 * 96 * 2},
            {"dbg_biasb2", poff["block2.layer0.attn_bias"], 64 * 64 * 2},
            {"dbg_b2g3", oG3, TOK * 128 * 2},
            {"dbg_b2g4mid", oG4, TOK * 32 * 4},
            {"dbg_b2x16", oG1, TOK * 32 * 2},
            {"dbg_fc1", vecOff["block1.layer0.ffn_cos_skip"], 32 * 4},
            {"dbg_fc2", vecOff["block2.layer0.ffn_cos_skip"], 32 * 4},
            {"dbg_ac2", vecOff["block2.layer0.attn_cos_skip"], 32 * 4},
            {"dbg_bias", poff["block0.layer0.attn_bias"], 4096 * 2},
            {"dbg31_at", oDBG + 5308416, TOK * 1024 * 2},
            {"dbg31_pub", oDBG + 0 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg32_pub", oDBG + 1 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg33_pub", oDBG + 2 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg34_pub", oDBG + 3 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg35_pub", oDBG + 4 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg36_pub", oDBG + 5 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg37_pub", oDBG + 6 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg38_pub", oDBG + 7 * (TOK * 1024 * 2), TOK * 1024 * 2},
            {"dbg31_in", oDBG + 6488064, TOK * 1024 * 2},
            {"dbg31_hg", oDBG + 7077888, TOK * 4096 * 2},
            {"dbg31_ffn", oDBG + 9437184, TOK * 1024 * 4},
            {"dbg31_q", oDBG + 10616832, TOK * 1024 * 2},
            {"dbg31_k", oDBG + 11206656, TOK * 1024 * 2},
            {"dbg31_v", oDBG + 11796480, TOK * 1024 * 2},
        };
        VkDeviceSize tot2 = 0;
        for (auto &d : dbgs) tot2 += d.bytes;
        Buffer rd2;
        allocHost(vk, tot2, rd2);
        VkCommandBuffer cb2 = beginCmd(vk);
        VkDeviceSize o2 = 0;
        for (auto &d : dbgs) {
            VkBufferCopy c{d.off, o2, d.bytes};
            vkCmdCopyBuffer(cb2, dev.buf, rd2.buf, 1, &c);
            o2 += d.bytes;
        }
        submitWait(vk, cb2);
        VkDeviceSize ro2 = 0;
        for (auto &d : dbgs) {
            std::string p = outDir + d.name + ".bin";
            std::ofstream f(p, std::ios::binary);
            f.write((const char *)rd2.mapped + ro2, (std::streamsize)d.bytes);
            ro2 += d.bytes;
        }
        freeBuf(vk, rd2);
        std::printf("debug arena dumps written\n");
    }
    struct Dump { const char *name; VkDeviceSize off; VkDeviceSize bytes; bool f16; };
    std::vector<Dump> dumps = {
        {"b4ds", oB4DS, TOK * 64 * 2, true},
        {"b22ds", oB22DS, TOK * 512 * 2, true},
        {"b30", oSS, TOK * 512 * 2, true},
        {"b38", oB38, TOK * 1024 * 2, true},
        {"b39", oB39, TOK * 512 * 2, true},
        {"b48", oB48, TOK * 256 * 2, true},
        {"b69", oB69, TOK * 32 * 2, true},
        {"merged70", oMRG70, TOK * 32 * 4, false},
        {"head", oHEAD, TOK * 16 * 4, false},
    };
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
    std::printf("M8a GPU-side complete — compare via golden_full.py\n");
    return 0;
}
