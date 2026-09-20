// ============================================================================
// M0 cooperative-matrix (XMX) probe for Intel Arc Pro B50  --  DLSS5_INTEL
//
// Enumerates VK_KHR_cooperative_matrix configs at runtime, prints them all,
// picks the first f16 x f16 -> {f16|f32} config, runs one subgroup-scoped
// coopMatMulAdd against a CPU f32 reference and reports PASS/FAIL + timing.
//
// Exit codes: 0 = PASS, 1 = FAIL (vulkan error / numeric mismatch),
//             2 = compiled-in config (CMake defines) != runtime-chosen config
// ============================================================================
#include <vulkan/vulkan.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---- compile-time shape (must match probe.spv; see CMakeLists.txt) --------
#ifndef CFG_M
#define CFG_M 16
#endif
#ifndef CFG_N
#define CFG_N 16
#endif
#ifndef CFG_K
#define CFG_K 16
#endif
#ifndef CFG_SUBGROUP
#define CFG_SUBGROUP 32
#endif
#ifndef CFG_CF32
#define CFG_CF32 1
#endif

#define VK_CHECK(x)                                                            \
    do {                                                                       \
        VkResult res_ = (x);                                                   \
        if (res_ != VK_SUCCESS) {                                              \
            std::fprintf(stderr, "VK error %d at line %d: %s\n", (int)res_,    \
                         __LINE__, #x);                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// ---- f16 <-> f32 helpers (inputs chosen to be normal f16 values) ----------
static uint16_t f32_to_f16(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3FFu;
    if (e <= 0) return (uint16_t)sign;              // flush tiny to zero
    if (e >= 31) return (uint16_t)(sign | 0x7C00u); // inf
    return (uint16_t)(sign | ((uint32_t)e << 10) | mant);
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t f;
    if (e == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            int exp = -1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            f = sign | ((uint32_t)(114 + exp) << 23) | (mant << 13);
        }
    } else if (e == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((e - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

static const char *compTypeName(VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "f64";
        case VK_COMPONENT_TYPE_SINT8_KHR:   return "s8";
        case VK_COMPONENT_TYPE_SINT16_KHR:  return "s16";
        case VK_COMPONENT_TYPE_SINT32_KHR:  return "s32";
        case VK_COMPONENT_TYPE_UINT8_KHR:   return "u8";
        case VK_COMPONENT_TYPE_UINT16_KHR:  return "u16";
        case VK_COMPONENT_TYPE_UINT32_KHR:  return "u32";
        default:                            return "?";
    }
}

static const char *scopeName(VkScopeKHR s) {
    switch (s) {
        case VK_SCOPE_DEVICE_KHR:       return "DEVICE";
        case VK_SCOPE_WORKGROUP_KHR:    return "WORKGROUP";
        case VK_SCOPE_SUBGROUP_KHR:     return "SUBGROUP";
        case VK_SCOPE_QUEUE_FAMILY_KHR: return "QUEUE_FAMILY";
        default:                        return "?";
    }
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

int main() {
    using clk = std::chrono::steady_clock;
    std::printf("=== M0 cooperative-matrix probe (Arc Pro B50) ===\n");

    // ---- instance ---------------------------------------------------------
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "m0-coopmat-probe";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t npd = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, nullptr));
    std::printf("physical devices: %u\n", npd);
    if (npd == 0) { std::fprintf(stderr, "no vulkan devices\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(npd);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds.data()));

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qfCompute = 0;
    for (auto d : pds) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        uint32_t next = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &next, nullptr);
        std::vector<VkExtensionProperties> exts(next);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &next, exts.data());
        bool hasCoop = false;
        for (auto &e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0)
                hasCoop = true;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qps.data());
        uint32_t q = 0;
        for (; q < nq; ++q)
            if (qps[q].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
        std::printf("  candidate: %s (api %u.%u.%u.%u) coopmat=%s computeQ=%s\n",
                    props.deviceName,
                    VK_API_VERSION_VARIANT(props.apiVersion),
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion),
                    VK_API_VERSION_PATCH(props.apiVersion),
                    hasCoop ? "YES" : "no",
                    q < nq ? "yes" : "NO");
        if (hasCoop && q < nq) { pd = d; qfCompute = q; break; }
    }
    if (pd == VK_NULL_HANDLE) {
        std::fprintf(stderr, "FAIL: no physical device supports VK_KHR_cooperative_matrix\n");
        return 1;
    }

    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    drv.pNext = &sub;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    std::printf("device: %s\n", p2.properties.deviceName);
    std::printf("apiVersion: %u.%u.%u.%u  driverVersion: %u (%s %s)\n",
                VK_API_VERSION_MAJOR(p2.properties.apiVersion),
                VK_API_VERSION_MINOR(p2.properties.apiVersion),
                VK_API_VERSION_PATCH(p2.properties.apiVersion),
                VK_API_VERSION_VARIANT(p2.properties.apiVersion),
                p2.properties.driverVersion, drv.driverName, drv.driverInfo);
    std::printf("subgroupSize: %u\n", sub.subgroupSize);

    // ---- enumerate cooperative matrix configs -----------------------------
    auto fpEnum = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    if (!fpEnum) { std::fprintf(stderr, "FAIL: vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR not found\n"); return 1; }

    uint32_t ncfg = 0;
    VK_CHECK(fpEnum(pd, &ncfg, nullptr));
    std::vector<VkCooperativeMatrixPropertiesKHR> cfgs(ncfg,
        VkCooperativeMatrixPropertiesKHR{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, nullptr});
    VK_CHECK(fpEnum(pd, &ncfg, cfgs.data()));
    std::printf("cooperative matrix configs: %u\n", ncfg);
    for (uint32_t i = 0; i < ncfg; ++i) {
        auto &c = cfgs[i];
        std::printf("  [%u] M=%u N=%u K=%u  A=%s B=%s C=%s R=%s  sat=%d scope=%s\n",
                    i, c.MSize, c.NSize, c.KSize,
                    compTypeName(c.AType), compTypeName(c.BType),
                    compTypeName(c.CType), compTypeName(c.ResultType),
                    c.saturatingAccumulation, scopeName(c.scope));
    }

    // pick first f16 x f16 -> f16|f32, prefer f32 result
    const VkCooperativeMatrixPropertiesKHR *pick = nullptr;
    for (auto &c : cfgs) {
        if (c.AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            c.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            (c.CType == VK_COMPONENT_TYPE_FLOAT16_KHR || c.CType == VK_COMPONENT_TYPE_FLOAT32_KHR) &&
            (c.ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR || c.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR)) {
            if (!pick) pick = &c;
            else if (pick->ResultType != VK_COMPONENT_TYPE_FLOAT32_KHR &&
                     c.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR) pick = &c;
        }
    }
    if (!pick) {
        std::fprintf(stderr, "FAIL: no f16 cooperative matrix config found\n");
        return 1;
    }
    const int chosenCF32 = (pick->CType == VK_COMPONENT_TYPE_FLOAT32_KHR ||
                            pick->ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR) ? 1 : 0;
    std::printf("CHOSEN M=%u N=%u K=%u SUBGROUP=%u CF32=%d\n",
                pick->MSize, pick->NSize, pick->KSize, sub.subgroupSize, chosenCF32);
    std::printf("COMPILED M=%d N=%d K=%d SUBGROUP=%d CF32=%d\n",
                CFG_M, CFG_N, CFG_K, CFG_SUBGROUP, CFG_CF32);
    {
        // write chosen shape for the build orchestrator (build.cmd recompiles
        // the shader with these values when they differ from the compiled-in)
        std::string cp = exeDir() + "chosen.txt";
        std::FILE *f = std::fopen(cp.c_str(), "w");
        if (f) {
            std::fprintf(f, "M=%u\nN=%u\nK=%u\nSG=%u\nCF32=%d\n",
                         pick->MSize, pick->NSize, pick->KSize, sub.subgroupSize, chosenCF32);
            std::fclose(f);
        }
    }
    if (pick->MSize != CFG_M || pick->NSize != CFG_N || pick->KSize != CFG_K ||
        sub.subgroupSize != (uint32_t)CFG_SUBGROUP || chosenCF32 != CFG_CF32) {
        std::fprintf(stderr, "CONFIG-MISMATCH: rebuild shader with the CHOSEN defines (exit 2)\n");
        return 2;
    }
    const uint32_t M = pick->MSize, N = pick->NSize, K = pick->KSize;

    // ---- device + queue ---------------------------------------------------
    const char *exts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfCompute;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VkDevice dev;
    VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfCompute, 0, &queue);

    // ---- buffers ----------------------------------------------------------
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == UINT32_MAX) { std::fprintf(stderr, "no host-visible+coherent memory type\n"); return 1; }

    auto makeBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buf, VkDeviceMemory &dmem) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mt;
        VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &dmem));
        VK_CHECK(vkBindBufferMemory(dev, buf, dmem, 0));
        return 0;
    };

    VkBuffer bA, bB, bC;
    VkDeviceMemory mA, mB, mC;
    if (makeBuf((VkDeviceSize)M * K * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bA, mA)) return 1;
    if (makeBuf((VkDeviceSize)K * N * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bB, mB)) return 1;
    if (makeBuf((VkDeviceSize)M * N * (chosenCF32 ? 4 : 2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bC, mC)) return 1;

    // fill A,B (host-coherent): small values exactly representable in f16
    auto valA = [](int i, int k) { return (float)(((i * 3 + k * 5) % 11) - 5) * 0.25f; };
    auto valB = [](int k, int j) { return (float)(((k * 7 + j * 2) % 13) - 6) * 0.125f; };

    uint16_t *pA; uint16_t *pB;
    VK_CHECK(vkMapMemory(dev, mA, 0, VK_WHOLE_SIZE, 0, (void **)&pA));
    VK_CHECK(vkMapMemory(dev, mB, 0, VK_WHOLE_SIZE, 0, (void **)&pB));
    for (uint32_t i = 0; i < M; ++i)
        for (uint32_t k = 0; k < K; ++k)
            pA[i * K + k] = f32_to_f16(valA((int)i, (int)k));
    for (uint32_t k = 0; k < K; ++k)
        for (uint32_t j = 0; j < N; ++j)
            pB[k * N + j] = f32_to_f16(valB((int)k, (int)j));
    vkUnmapMemory(dev, mA);
    vkUnmapMemory(dev, mB);

    // ---- descriptors ------------------------------------------------------
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 3;
    dlci.pBindings = bindings;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &dsl));

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    dpci.pPoolSizes = &psz;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkDescriptorBufferInfo biA{bA, 0, VK_WHOLE_SIZE}, biB{bB, 0, VK_WHOLE_SIZE}, biC{bC, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]{};
    VkDescriptorBufferInfo *bis[3] = {&biA, &biB, &biC};
    for (int i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = dset;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = bis[i];
    }
    vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);

    // ---- pipeline ---------------------------------------------------------
    std::vector<char> spv;
    std::string spvPath = exeDir() + "probe.spv";
    if (readFile(spvPath, spv)) {
        // fallback: cwd
        if (readFile("probe.spv", spv)) {
            std::fprintf(stderr, "cannot read probe.spv (looked at %s)\n", spvPath.c_str());
            return 1;
        }
    }
    std::printf("loaded %zu bytes of SPIR-V from %s\n", spv.size(), spvPath.c_str());
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = (const uint32_t *)spv.data();
    VkShaderModule sm;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout playout;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &playout));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm;
    cpci.stage.pName = "main";
    cpci.layout = playout;
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));

    // ---- command buffer ---------------------------------------------------
    VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci2.queueFamilyIndex = qfCompute;
    VkCommandPool cpool;
    VK_CHECK(vkCreateCommandPool(dev, &cpci2, nullptr, &cpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    auto recordOne = [&]() {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        vkEndCommandBuffer(cmd);
    };
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    // warmup
    recordOne();
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    // single-dispatch latency
    recordOne();
    auto t0 = clk::now();
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    auto t1 = clk::now();
    double singleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // batch of 100 dispatches in one command buffer
    vkResetCommandBuffer(cmd, 0);
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);
        for (int it = 0; it < 100; ++it) vkCmdDispatch(cmd, 1, 1, 1);
        vkEndCommandBuffer(cmd);
    }
    auto t2 = clk::now();
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    auto t3 = clk::now();
    double batchMs = std::chrono::duration<double, std::milli>(t3 - t2).count() / 100.0;

    // ---- verify -----------------------------------------------------------
    std::vector<float> got(M * N, 0.0f);
    if (chosenCF32) {
        float *pC;
        VK_CHECK(vkMapMemory(dev, mC, 0, VK_WHOLE_SIZE, 0, (void **)&pC));
        for (uint32_t i = 0; i < M; ++i)
            for (uint32_t j = 0; j < N; ++j) got[i * N + j] = pC[i * N + j];
        vkUnmapMemory(dev, mC);
    } else {
        uint16_t *pC;
        VK_CHECK(vkMapMemory(dev, mC, 0, VK_WHOLE_SIZE, 0, (void **)&pC));
        for (uint32_t i = 0; i < M; ++i)
            for (uint32_t j = 0; j < N; ++j) got[i * N + j] = f16_to_f32(pC[i * N + j]);
        vkUnmapMemory(dev, mC);
    }

    int bad = 0;
    double maxRel = 0.0;
    for (uint32_t i = 0; i < M; ++i) {
        for (uint32_t j = 0; j < N; ++j) {
            float expv = 0.0f;
            for (uint32_t k = 0; k < K; ++k)
                expv += valA((int)i, (int)k) * valB((int)k, (int)j);
            float g = got[i * N + j];
            float err = std::fabs(g - expv);
            float tol = std::max(0.01f * std::fabs(expv), 1e-3f);
            double rel = (std::fabs(expv) > 1e-6f) ? (double)err / std::fabs(expv) : (double)err;
            if (rel > maxRel) maxRel = rel;
            if (err > tol) {
                if (bad < 8)
                    std::printf("  mismatch [%u,%u]: got %f expected %f (err %f)\n",
                                i, j, g, expv, err);
                ++bad;
            }
        }
    }
    std::printf("checked %u x %u = %u results, max rel err %.4f%%\n",
                M, N, M * N, maxRel * 100.0);
    std::printf("timing: single dispatch+sync %.3f ms; batch avg %.4f ms/dispatch\n",
                singleMs, batchMs);

    int rc = 0;
    if (bad == 0) {
        std::printf("RESULT: PASS (config %ux%ux%u f16, C=%s)\n", M, N, K,
                    chosenCF32 ? "f32" : "f16");
    } else {
        std::printf("RESULT: FAIL (%d mismatches)\n", bad);
        rc = 1;
    }

    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyPipelineLayout(dev, playout, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);
    vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vkDestroyCommandPool(dev, cpool, nullptr);
    for (auto b : {bA, bB, bC}) vkDestroyBuffer(dev, b, nullptr);
    for (auto m : {mA, mB, mC}) vkFreeMemory(dev, m, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return rc;
}
