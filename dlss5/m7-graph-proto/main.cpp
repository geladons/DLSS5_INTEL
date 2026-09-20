// ============================================================================
// M7 graph prototype for Intel Arc Pro B50  --  DLSS5_INTEL
//
// M7a: rounding unit kernel — publish.glsl chain (half_round / e4m3 /
//      gate_activation / e4m3 o gate) on a value sweep, bit-compared against a
//      C++ CPU implementation of the same design-doc SS A.1 formulas.
// M7b: global transformer block 31 end-to-end @ 288 tokens (24x12 grid,
//      C=1024, H=32 heads x head_dim 32) — RMSNorm-free plain global FFN
//      (gate+e4m3) -> residual -> full MHA (cosine publish Q/K, e4m3 V,
//      bit-affine softmax, +-3 cap, scale*sqrt(32)) -> residual + e4m3 publish.
//      Golden: golden.py (NumPy + exact rounding contract).
//
// Deps: vulkan-1 only. Console app, no DDA/present.
// Exit: 0 = all PASS, 1 = Vulkan/file error, 2 = M7a mismatch, 3 = M7b GPU-side
//       stage error (compare verdicts come from golden.py).
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
// CPU half_round = hardware F16C vcvtps2ph, round-to-nearest-even, exactly what
// GLSL packHalf2x16 does (validated over every f16 value by the reference's
// half_probe). Software RNE fallback if the CPU lacks F16C.
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
    int hexp = e - 112;  // rebased exponent (127-15)
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
            if (m == 0x400u) return (uint16_t)(sign | 0x7C00u); // rounds up to 2^-14 min normal
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
        __m128i h = _mm_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT);  // imm bits[1:0]=RN;
        return (uint16_t)_mm_extract_epi16(h, 0);                // MXCSR masks flags
    }
    return f32_to_f16_soft(f);
}

static float half_round_cpu(float f) {
    uint16_t h = f32_to_f16(f);
    // decode back (exact)
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t b;
    if (e == 0) {
        if (mant == 0) { b = sign; }
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

// ------------------------------------------------- CPU e4m3 / gate (doc) ---
static float round_even_cpu(float v) {  // v >= 0, finite; exact halves possible
    float fl = std::floor(v);
    float diff = v - fl;
    if (diff > 0.5f) return fl + 1.0f;
    if (diff < 0.5f) return fl;
    return (std::fmod(fl, 2.0f) == 0.0f) ? fl : fl + 1.0f;
}

static float e4m3_cpu(float x) {  // design doc SS A.1 (= publish.glsl)
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

static float gate_cpu(float x) {  // fp32 arithmetic + half_round at doc points
    float wide = half_round_cpu(x);
    float clamped = std::clamp(wide, -4.0f, 4.0f);
    float linear = std::fabs(clamped) * -0.055908203125f;
    linear += 0.447265625f;
    linear = half_round_cpu(linear);
    linear *= clamped;
    linear += 0.89453125f;
    linear = half_round_cpu(linear);
    return half_round_cpu(wide * linear);
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
    // minimal JSON cursor for {"name": {"dtype":s,"shape":[i],"data_offsets":[i,i]}}
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
                } else { // skip value
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
    app.pApplicationName = "m7-graph-proto";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &vk.inst));

    uint32_t npd = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(vk.inst, &npd, nullptr));
    std::vector<VkPhysicalDevice> pds(npd);
    VK_CHECK(vkEnumeratePhysicalDevices(vk.inst, &npd, pds.data()));

    // pick the coopmat-capable device with the largest device-local heap (the
    // real B50; the root-enumerated virtual display has no heap/co-op matrix)
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
    void *mapped = nullptr;  // host buffers only
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
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
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

// buffer-ref pipelines: one empty descriptor set layout + 128B push range
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
    vkDestroyDescriptorSetLayout(vk.dev, dsl, nullptr);
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
    vkFreeCommandBuffers(vk.dev, vk.cmdPool, 1, &cb);
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

// push blocks matching the GLSL (scalar layout)
struct GemmPush {
    uint64_t a, b, c;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
};
static_assert(sizeof(GemmPush) >= 68, "gemm push layout");

struct CosPush {
    uint64_t a, c, d;
    uint32_t m, flags;
};
static_assert(sizeof(CosPush) == 32, "cos push layout");

struct SMaxPush {
    uint64_t a, c;
    uint32_t m, n;
    float p0;
};
static_assert(sizeof(SMaxPush) >= 28, "softmax push layout");

struct EWPush {
    uint64_t a, b, c, d, h;
    uint32_t n, kind, ch;
};
static_assert(sizeof(EWPush) >= 52, "elementwise push layout");

// epilogue + flag constants (design doc SS C.1, xmxres.EPI_*)
enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1 };

// ===========================================================================
// M7a — rounding unit kernel
// ===========================================================================
static int runM7a(const VkCtx &vk, const std::string &dir) {
    std::printf("\n=== M7a: rounding unit kernel ===\n");
    std::vector<float> xs;
    // 1) doc boundary / contract vectors
    const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f,
                             4.5f, -4.5f, 8.0f, -8.0f, 100.0f, -100.0f,
                             448.0f, -448.0f, 449.0f, -449.0f, 500.0f, -500.0f,
                             65504.0f, 65520.0f, -65504.0f, 1e30f, -1e30f,
                             INFINITY, -INFINITY,
                             0.015625f,                 // 2^-6 e4m3 normal floor
                             0.0146484375f,             // just below
                             0.001953125f,              // 2^-9 subnormal step
                             0.00006198883056640625f,   // 2^-14 cosine norm floor
                             0.000030517578125f,        // 2^-15
                             1e-8f, -1e-8f, 5.9604645e-08f, 1.1754944e-38f, -1.1754944e-38f,
                             0.055908203125f, 0.447265625f, 0.89453125f};
    for (float v : special) xs.push_back(v);
    // 2) every finite f16 bit pattern widened to f32 (NaNs excluded: the e4m3
    //    contract is only defined over finite values; GLSL min(NaN,448) is
    //    implementation-defined). Inf patterns are kept (both sides saturate).
    for (uint32_t b = 0; b < 65536u; ++b) {
        if ((b & 0x7FFFu) > 0x7C00u) continue;  // NaN
        xs.push_back(f16_bits_to_f32((uint16_t)b));
    }
    // 3) random sweep across magnitudes (sign random, exponent uniform)
    {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
        for (int i = 0; i < 4 * 1000 * 1000; ++i) {
            uint64_t r = rnd();
            float m = (float)((r >> 40) & 0xFFFFFF) / 16777216.0f + 0.5f; // [0.5,1.5)
            int e = (int)(r % 61) - 30;                                    // 2^-30..2^30
            float v = std::ldexp(m, e);
            xs.push_back((r & 1) ? -v : v);
        }
        // uniform dense in [-1024, 1024]
        for (int i = 0; i < 1 * 1000 * 1000; ++i) {
            uint64_t r = rnd();
            xs.push_back(((double)(r % 2000001) / 1000000.0 - 1.0) * 1024.0);
        }
    }
    uint32_t n = (uint32_t)xs.size();
    std::printf("sweep: %u values (%zu special + 65536 f16 patterns + random)\n",
                n, sizeof(special) / sizeof(special[0]));

    Buffer inB, outB;
    allocHost(vk, (VkDeviceSize)n * 4, inB);
    allocHost(vk, (VkDeviceSize)n * 16, outB);
    std::memcpy(inB.mapped, xs.data(), (size_t)n * 4);

    // CPU expected
    std::vector<float> exp((size_t)n * 4);
    for (uint32_t i = 0; i < n; ++i) {
        float x = xs[i];
        exp[(size_t)i * 4 + 0] = half_round_cpu(x);
        exp[(size_t)i * 4 + 1] = e4m3_cpu(x);
        exp[(size_t)i * 4 + 2] = gate_cpu(x);
        float g = gate_cpu(x);
        exp[(size_t)i * 4 + 3] = e4m3_cpu(g);
    }

    // pipeline (descriptor-based round_test)
    VkDescriptorSetLayoutBinding bd[2]{};
    for (int i = 0; i < 2; ++i) {
        bd[i].binding = (uint32_t)i;
        bd[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bd[i].descriptorCount = 1;
        bd[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bd;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(vk.dev, &plci, nullptr, &layout));
    VkShaderModule mod = loadShader(vk, dir + "round_test.spv");
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(vk.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
    vkDestroyShaderModule(vk.dev, mod, nullptr);

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(vk.dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VK_CHECK(vkAllocateDescriptorSets(vk.dev, &dsai, &dset));
    VkDescriptorBufferInfo bi[2]{{inB.buf, 0, VK_WHOLE_SIZE}, {outB.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = dset;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vk.dev, 2, w, 0, nullptr);

    VkCommandBuffer cb = beginCmd(vk);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, nullptr);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    submitWait(vk, cb);

    uint32_t match[4] = {0, 0, 0, 0};
    uint32_t shown[4] = {0, 0, 0, 0};
    const char *names[4] = {"half_round", "e4m3", "gate_activation", "e4m3(gate)"};
    const float *got = (const float *)outB.mapped;
    for (uint32_t i = 0; i < n; ++i) {
        for (int k = 0; k < 4; ++k) {
            uint32_t gb, eb;
            float g = got[(size_t)i * 4 + k], e = exp[(size_t)i * 4 + k];
            std::memcpy(&gb, &g, 4);
            std::memcpy(&eb, &e, 4);
            if (gb == eb) { match[k]++; }
            else if (shown[k] < 5) {
                shown[k]++;
                std::printf("  MISMATCH %s: x=%.9g (bits %08x) gpu=%.9g cpu=%.9g\n",
                            names[k], xs[i], ((uint32_t *)xs.data())[i], g, e);
            }
        }
    }
    int rc = 0;
    for (int k = 0; k < 4; ++k) {
        double pct = 100.0 * match[k] / n;
        std::printf("  %-16s match %u/%u = %.4f%%\n", names[k], match[k], n, pct);
        if (match[k] != n) rc = 2;
    }
    std::printf("M7a: %s\n", rc == 0 ? "PASS (100%% bit-exact)" : "FAIL");
    vkDestroyPipeline(vk.dev, pipe, nullptr);
    vkDestroyPipelineLayout(vk.dev, layout, nullptr);
    vkDestroyDescriptorSetLayout(vk.dev, dsl, nullptr);
    vkDestroyDescriptorPool(vk.dev, pool, nullptr);
    freeBuf(vk, inB);
    freeBuf(vk, outB);
    return rc;
}

// ===========================================================================
// M7b — global block 31
// ===========================================================================
static const uint32_t TOK = 288, CH = 1024, HEADS = 32, HDIM = 32;

struct Arena {
    Buffer dev;
    VkDeviceSize off = 0;
    VkDeviceSize alloc(VkDeviceSize bytes) {
        VkDeviceSize a = (off + 255) & ~(VkDeviceSize)255;
        off = a + bytes;
        return a;
    }
};

struct GpuDump { const char *name; VkDeviceSize off; VkDeviceSize bytes; bool f16; };

static int runM7b(const VkCtx &vk, const std::string &dir,
                  const std::string &weightsPath, const std::string &outDir) {
    std::printf("\n=== M7b: global block 31 @ %ux%u (C=%u, H=%u) ===\n", TOK, TOK, CH, HEADS);
    auto t0 = clk::now();

    // ---- weights ----------------------------------------------------------
    WeightsFile wf;
    stParse(weightsPath, wf);
    auto need = [&](const char *name) -> const Tensor & {
        auto it = wf.tensors.find(name);
        if (it == wf.tensors.end()) { std::fprintf(stderr, "missing tensor %s\n", name); std::exit(1); }
        return it->second;
    };
    const char *W0 = "block31.layer0.weight";            // [1024,4096] F16
    const char *W1 = "block31.layer1.weight";            // [4096,1024] F16
    const char *WC = "block31.layer1.ffn_cos_skip";      // [1024] F16
    const char *QK = "block31.layer2.qkv_weight";        // [1024,3072] F16
    const char *AS = "block31.layer2.attn_scale";        // [32] F32
    const char *PW = "block31.layer4.projection_weight"; // [1024,1024] F16
    const char *AC = "block31.layer4.attn_cos_skip";     // [1024] F16
    struct WD { const char *name; uint64_t bytes; };
    WD wds[] = {{W0, 0}, {W1, 0}, {QK, 0}, {PW, 0}, {WC, 0}, {AC, 0}, {AS, 0}};
    for (auto &w : wds) w.bytes = need(w.name).numel() * (need(w.name).dtype == "F32" ? 4 : 2);

    // ---- synthetic input (documented generator) ---------------------------
    // x[t, c] = 0.60*sin(2*pi*(0.007*t + 0.013*c))
    //         + 0.25*sin(2*pi*(0.031*t - 0.017*c) + 1.7)
    //         + 0.10*cos(2*pi*(0.113*t + 0.041*c) + 0.3)
    std::vector<float> x((size_t)TOK * CH);
    const double PI = 3.14159265358979323846;
    for (uint32_t t = 0; t < TOK; ++t)
        for (uint32_t c = 0; c < CH; ++c) {
            double v = 0.60 * std::sin(2 * PI * (0.007 * t + 0.013 * c)) +
                       0.25 * std::sin(2 * PI * (0.031 * t - 0.017 * c) + 1.7) +
                       0.10 * std::cos(2 * PI * (0.113 * t + 0.041 * c) + 0.3);
            x[(size_t)t * CH + c] = (float)v;
        }
    std::vector<uint16_t> x16((size_t)TOK * CH);
    for (size_t i = 0; i < x.size(); ++i) x16[i] = f32_to_f16(x[i]);

    // per-head Q scale = attn_scale * sqrt(32), fp32 first (doc SS A.6.2)
    std::vector<float> qscale(HEADS);
    {
        const Tensor &as = need(AS);
        const float *av = (const float *)(wf.bytes.data() + wf.dataBase + as.off0);
        float s32 = std::sqrt(32.0f);
        for (uint32_t h = 0; h < HEADS; ++h) qscale[h] = av[h] * s32;
    }

    // ---- arena ------------------------------------------------------------
    Arena ar;
    VkDeviceSize oW0 = ar.alloc(need(W0).numel() * 2);
    VkDeviceSize oW1 = ar.alloc(need(W1).numel() * 2);
    VkDeviceSize oQK = ar.alloc(need(QK).numel() * 2);
    VkDeviceSize oPW = ar.alloc(need(PW).numel() * 2);
    VkDeviceSize oWC = ar.alloc(need(WC).numel() * 4);   // f16 in file, fp32 in arena
    VkDeviceSize oAC = ar.alloc(need(AC).numel() * 4);   // (residual kernels read FloatBuf)
    VkDeviceSize oQS = ar.alloc(HEADS * 4);
    VkDeviceSize oX = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oX16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oH = ar.alloc((VkDeviceSize)TOK * 4096 * 2);
    VkDeviceSize oBR = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oFF = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oF16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oPJ = ar.alloc((VkDeviceSize)TOK * 3072 * 4);
    VkDeviceSize oQ16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oK16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oV16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oSC = ar.alloc((VkDeviceSize)HEADS * TOK * TOK * 4);
    VkDeviceSize oPR = ar.alloc((VkDeviceSize)HEADS * TOK * TOK * 2);
    VkDeviceSize oMG = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oAT = ar.alloc((VkDeviceSize)TOK * CH * 2);
    VkDeviceSize oAB = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oBO = ar.alloc((VkDeviceSize)TOK * CH * 4);
    VkDeviceSize oB16 = ar.alloc((VkDeviceSize)TOK * CH * 2);
    allocDev(vk, ar.off, ar.dev);
    std::printf("arena: %.2f MB device-local\n", ar.off / 1048576.0);

    // ---- staging + upload -------------------------------------------------
    // One contiguous host staging image of the device arena prefix
    // [weights | qscale | x | x16]; a single vkCmdCopyBuffer uploads it all.
    // (BUGFIX: the old sizing used max(weightsEnd, inBytes) and then wrote x at
    // staging+oX — 1.18 MB past the allocation — and the copy length stopped at
    // oX, never delivering x/x16. Heap overflow -> 0xC0000005 after arena setup.)
    VkDeviceSize upBytes = oX16 + (VkDeviceSize)TOK * CH * 2;
    Buffer staging;
    allocHost(vk, upBytes, staging);
    char *sp = (char *)staging.mapped;
    auto place = [&](const Tensor &t, VkDeviceSize dst) {
        std::memcpy(sp + dst, wf.bytes.data() + wf.dataBase + t.off0, t.off1 - t.off0);
    };
    // cos_skip vectors are F16 in the safetensors but consumed as fp32 by the
    // residual kernels (FloatBuf) — convert on the host, upload fp32.
    auto placeF16asF32 = [&](const Tensor &t, VkDeviceSize dst) {
        const uint16_t *src = (const uint16_t *)(wf.bytes.data() + wf.dataBase + t.off0);
        float *outp = (float *)(sp + dst);
        for (uint64_t i = 0; i < t.numel(); ++i) outp[i] = f16_bits_to_f32(src[i]);
    };
    place(need(W0), oW0); place(need(W1), oW1); place(need(QK), oQK);
    place(need(PW), oPW);
    placeF16asF32(need(WC), oWC);
    placeF16asF32(need(AC), oAC);
    std::memcpy(sp + oQS, qscale.data(), HEADS * 4);
    std::memcpy(sp + oX, x.data(), (size_t)TOK * CH * 4);
    std::memcpy(sp + oX16, x16.data(), (size_t)TOK * CH * 2);

    VkCommandBuffer up = beginCmd(vk);
    VkBufferCopy cp1{0, 0, upBytes};
    vkCmdCopyBuffer(up, staging.buf, ar.dev.buf, 1, &cp1);
    submitWait(vk, up);

    // ---- pipelines --------------------------------------------------------
    Pipeline pGemm, pCos, pSmax, pEw;
    makePipeline(vk, dir + "gemm.spv", pGemm);
    makePipeline(vk, dir + "cosine.spv", pCos);
    makePipeline(vk, dir + "softmax.spv", pSmax);
    makePipeline(vk, dir + "elementwise.spv", pEw);

    // ---- record block with timestamps -------------------------------------
    const char *stageNames[] = {
        "ffn_expand_gemm", "ffn_contract_gemm", "ffn_residual", "to_half",
        "qkv_gemm", "cosine_q", "cosine_k", "cosine_v",
        "scores_gemm", "softmax", "ctx_gemm", "attended_e4m3",
        "attnproj_gemm", "final_residual"};
    const int NS = 14;
    const bool isGemm[] = {true, true, false, false, true, false, false, false,
                           true, false, true, false, true, false};
    const double flops[] = {
        2.0 * TOK * CH * 4096, 2.0 * TOK * 4096 * CH, 0, 0,
        2.0 * TOK * CH * 3072, 0, 0, 0,
        HEADS * 2.0 * TOK * TOK * HDIM, 0, HEADS * 2.0 * TOK * TOK * HDIM, 0,
        2.0 * TOK * CH * CH, 0};

    VkQueryPool qpool;
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2 * NS;
    VK_CHECK(vkCreateQueryPool(vk.dev, &qpci, nullptr, &qpool));

    auto dispatchGemm = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c,
                            uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                            uint32_t sa, uint32_t sb, uint32_t sc,
                            uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
        GemmPush p{a, b, c, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc};
        vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, n / 32, m / 16, batch);
    };
    auto dispatchCos = [&](VkCommandBuffer cb, uint64_t a, uint64_t c, uint64_t d,
                           uint32_t m, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
        CosPush p{a, c, d, m, kind};
        vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (m + 31) / 32, 1, 1);
    };
    auto dispatchSMax = [&](VkCommandBuffer cb, uint64_t a, uint64_t c,
                            uint32_t m, uint32_t n, float p0) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c, m, n, p0};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (m + 31) / 32, 1, 1);
    };
    auto dispatchEw = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
        EWPush p{a, b, c, d, h, n, kind, ch};
        vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };

    VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, ar.dev.buf};
    uint64_t A0 = vkGetBufferDeviceAddress(vk.dev, &bdai);
    auto A = [&](VkDeviceSize o) { return A0 + (uint64_t)o; };

    auto recordBlock = [&](VkCommandBuffer cb, bool timed, int tlo, int thi) {
        int ts = 0;
        auto tick = [&](VkCommandBuffer c, bool top) {
            int s = ts / 2;
            if (timed && s >= tlo && s < thi)
                vkCmdWriteTimestamp(c, top ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    qpool, (uint32_t)(ts++));
            else if (timed)
                ts++;  // keep stage<->query mapping stable without writing
        };
        auto bar = [&]() { fullBarrier(cb, ar.dev.buf); };

        // 0 FFN expand: x16 @ W0 -> h f16 (GATE_E4M3)
        tick(cb, true);
        dispatchGemm(cb, A(oX16), A(oW0), A(oH), TOK, 4096, CH, 1, 0, 0, 0,
                     (EPI_GATE_E4M3 << 8) | F_NARROW, CH, 4096, 4096);
        tick(cb, false); bar();
        // 1 FFN contract: h @ W1 -> branch fp32
        tick(cb, true);
        dispatchGemm(cb, A(oH), A(oW1), A(oBR), TOK, CH, 4096, 1, 0, 0, 0,
                     EPI_NONE, 4096, CH, CH);
        tick(cb, false); bar();
        // 2 ffn residual: ffn_out = branch + x * ffn_cos
        tick(cb, true);
        dispatchEw(cb, A(oBR), A(oX), A(oFF), A(oWC), 0, TOK * CH, 1, CH);
        tick(cb, false); bar();
        // 3 to_half ffn16
        tick(cb, true);
        dispatchEw(cb, A(oFF), 0, 0, 0, A(oF16), TOK * CH, 2, 0);
        tick(cb, false); bar();
        // 4 qkv: ffn16 @ qkv_w -> proj fp32
        tick(cb, true);
        dispatchGemm(cb, A(oF16), A(oQK), A(oPJ), TOK, 3072, CH, 1, 0, 0, 0,
                     EPI_NONE, CH, 3072, 3072);
        tick(cb, false); bar();
        // 5/6/7 cosine publishes
        tick(cb, true);
        dispatchCos(cb, A(oPJ), A(oQ16), A(oQS), TOK * HEADS, 0);
        bar();
        dispatchCos(cb, A(oPJ), A(oK16), A(oQS), TOK * HEADS, 1);
        bar();
        dispatchCos(cb, A(oPJ), A(oV16), A(oQS), TOK * HEADS, 2);
        tick(cb, false); bar();
        // 8 scores: per head q16 @ k16^T -> scores fp32
        tick(cb, true);
        dispatchGemm(cb, A(oQ16), A(oK16), A(oSC), TOK, TOK, HDIM, HEADS,
                     HDIM, HDIM, TOK * TOK,
                     F_TRANSPOSE, CH, CH, TOK);
        tick(cb, false); bar();
        // 9 softmax
        tick(cb, true);
        dispatchSMax(cb, A(oSC), A(oPR), HEADS * TOK, TOK, 3.0f);
        tick(cb, false); bar();
        // 10 ctx: per head probs @ v16 -> merged fp32 (strided store = merge_heads)
        tick(cb, true);
        dispatchGemm(cb, A(oPR), A(oV16), A(oMG), TOK, HDIM, TOK, HEADS,
                     TOK * TOK, HDIM, HDIM,
                     EPI_NONE, TOK, CH, CH);
        tick(cb, false); bar();
        // 11 attended = e4m3(merged) f16
        tick(cb, true);
        dispatchEw(cb, A(oMG), 0, 0, 0, A(oAT), TOK * CH, 3, 0);
        tick(cb, false); bar();
        // 12 attn projection: attended16 @ proj_w -> attn_branch fp32
        tick(cb, true);
        dispatchGemm(cb, A(oAT), A(oPW), A(oAB), TOK, CH, CH, 1, 0, 0, 0,
                     EPI_NONE, CH, CH, CH);
        tick(cb, false); bar();
        // 13 final residual + e4m3 publish
        tick(cb, true);
        dispatchEw(cb, A(oAB), A(oFF), A(oBO), A(oAC), A(oB16), TOK * CH, 0, CH);
        tick(cb, false); bar();
    };

    // timed correctness run — in TWO passes: the Arc driver only surfaces the
    // first ~24 timestamp writes of a submission as available (queries 24..27
    // went NOT-READY with 28 writes), so time stages 0-11 and 12-13 separately.
    auto readTs = [&](uint64_t *tsOut) {
        VkResult qr = vkGetQueryPoolResults(vk.dev, qpool, 0, 2 * NS, 2 * NS * sizeof(uint64_t),
                                            tsOut, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (qr != VK_SUCCESS) {
            std::fprintf(stderr, "warn: batched vkGetQueryPoolResults -> %d; reading per-query\n", (int)qr);
            for (uint32_t q = 0; q < (uint32_t)(2 * NS); ++q) {
                VkResult r = vkGetQueryPoolResults(vk.dev, qpool, q, 1, sizeof(uint64_t),
                                                   tsOut + q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                if (r != VK_SUCCESS) {
                    std::fprintf(stderr, "  ts[%u] NOT-READY\n", q);
                    tsOut[q] = (q > 0) ? tsOut[q - 1] : 0;  // monotonic fallback
                }
            }
        }
    };
    uint64_t tsA[2 * NS], tsB[2 * NS], ts[2 * NS];
    {
        VkCommandBuffer cb = beginCmd(vk);
        vkCmdResetQueryPool(cb, qpool, 0, 2 * NS);
        recordBlock(cb, true, 0, 12);
        submitWait(vk, cb);
        readTs(tsA);
        cb = beginCmd(vk);
        vkCmdResetQueryPool(cb, qpool, 0, 2 * NS);
        recordBlock(cb, true, 12, NS);
        submitWait(vk, cb);
        readTs(tsB);
    }
    for (int i = 0; i < 2 * NS; ++i) ts[i] = (i < 24) ? tsA[i] : tsB[i];
    double stageMs[NS], gemmMs = 0, totalMs = 0, gemmFlops = 0;
    for (int s = 0; s < NS; ++s) {
        stageMs[s] = (ts[2 * s + 1] - ts[2 * s]) * vk.tsPeriod / 1e6;
        totalMs += stageMs[s];
        if (isGemm[s]) { gemmMs += stageMs[s]; gemmFlops += flops[s]; }
        std::printf("  stage %2d  %-18s %8.3f ms\n", s, stageNames[s], stageMs[s]);
    }
    // NB: gemmFlops is in FLOP; /1e12 -> TFLOP.
    std::printf("GEMM total: %.3f ms  (%.2f GFLOP -> %.2f TFLOP/s GEMM-only)\n",
                gemmMs, gemmFlops / 1e9, gemmFlops / 1e12 / (gemmMs / 1e3));
    std::printf("block total (dispatch-only): %.3f ms -> %.2f TFLOP/s effective\n",
                totalMs, gemmFlops / 1e12 / (totalMs / 1e3));

    // steady-state: 20 back-to-back blocks
    VkCommandBuffer cb = beginCmd(vk);
    for (int i = 0; i < 20; ++i) recordBlock(cb, false, 0, NS);
    auto sw0 = clk::now();
    submitWait(vk, cb);
    double steady = msSince(sw0) / 20.0;
    std::printf("steady-state block: %.3f ms (20-iter avg, incl. barriers) -> %.2f TFLOP/s\n",
                steady, gemmFlops / 1e12 / (steady / 1e3));

    // ---- readback + dump --------------------------------------------------
    std::vector<GpuDump> dumps = {
        {"h", oH, (VkDeviceSize)TOK * 4096 * 2, true},
        {"branch", oBR, (VkDeviceSize)TOK * CH * 4, false},
        {"ffn_out", oFF, (VkDeviceSize)TOK * CH * 4, false},
        {"proj", oPJ, (VkDeviceSize)TOK * 3072 * 4, false},
        {"q16", oQ16, (VkDeviceSize)TOK * CH * 2, true},
        {"k16", oK16, (VkDeviceSize)TOK * CH * 2, true},
        {"v16", oV16, (VkDeviceSize)TOK * CH * 2, true},
        {"scores", oSC, (VkDeviceSize)HEADS * TOK * TOK * 4, false},
        {"probs", oPR, (VkDeviceSize)HEADS * TOK * TOK * 2, true},
        {"merged", oMG, (VkDeviceSize)TOK * CH * 4, false},
        {"attended16", oAT, (VkDeviceSize)TOK * CH * 2, true},
        {"attn_branch", oAB, (VkDeviceSize)TOK * CH * 4, false},
        {"block_raw", oBO, (VkDeviceSize)TOK * CH * 4, false},
        {"block16", oB16, (VkDeviceSize)TOK * CH * 2, true},
    };
    VkDeviceSize totalDump = 0;
    for (auto &d : dumps) totalDump += d.bytes;
    Buffer rdst;
    allocHost(vk, totalDump, rdst);
    cb = beginCmd(vk);
    VkDeviceSize dstOff = 0;
    std::vector<VkDeviceSize> dumpOffs;
    for (auto &d : dumps) {
        VkBufferCopy c{d.off, dstOff, d.bytes};
        vkCmdCopyBuffer(cb, ar.dev.buf, rdst.buf, 1, &c);
        dumpOffs.push_back(dstOff);
        dstOff += d.bytes;
    }
    submitWait(vk, cb);

    // x.bin for the golden
    {
        std::string p = outDir + "x.bin";
        std::ofstream f(p, std::ios::binary);
        f.write((const char *)x.data(), (std::streamsize)(x.size() * 4));
    }
    for (size_t i = 0; i < dumps.size(); ++i) {
        std::string p = outDir + "gpu_" + dumps[i].name + (dumps[i].f16 ? ".f16" : ".bin");
        std::ofstream f(p, std::ios::binary);
        f.write((const char *)rdst.mapped + dumpOffs[i], (std::streamsize)dumps[i].bytes);
    }
    std::printf("dumped %zu tensors to %s (%.1f MB) in %.0f ms\n",
                dumps.size() + 1, outDir.c_str(), (totalDump + x.size() * 4) / 1048576.0,
                msSince(t0));

    vkDestroyQueryPool(vk.dev, qpool, nullptr);
    freeBuf(vk, staging);
    freeBuf(vk, rdst);
    freeBuf(vk, ar.dev);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // F16C probe for the CPU half_round
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    g_hasF16C = (cpuInfo[2] & (1 << 29)) != 0;
    std::printf("M7 graph prototype — Arc Pro B50\n");
    std::printf("CPU F16C: %s\n", g_hasF16C ? "yes" : "no (software RNE)");

    VkCtx vk{};
    vkInit(vk);

    std::string dir = exeDir();
    std::string repo = "C:\\Users\\AI\\Desktop\\DLSS5_INTEL";
    std::string outDir = dir + "out\\";
    CreateDirectoryA(outDir.c_str(), nullptr);

    int rc = runM7a(vk, dir);
    int rcB = runM7b(vk, dir, repo + "\\work\\mlxw\\dlssnr-logical.safetensors", outDir);
    if (rcB != 0) rc = rcB;

    std::printf("\nM7 GPU-side complete: M7a %s, M7b GPU run ok (compare via golden.py)\n",
                (rc == 0 || rc == 3) ? "PASS" : "FAIL");
    return rc;
}
