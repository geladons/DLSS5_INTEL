// ============================================================================
// M6a safetensors weights loader for Intel Arc Pro B50  --  DLSS5_INTEL
//
// Reads work/mlxw/dlssnr-logical.safetensors (649 tensors, F16/F32, 71
// transformer blocks), prints inventory + block/dims signature, uploads ALL
// tensors into ONE device-local VkBuffer (host-coherent staging -> device
// local, per-block submits for timing), optionally verifies GPU residency
// by reading back 3 sample tensors and comparing bytes with the file, and
// computes host-side sanity statistics (mean/std/min/max) for selected
// tensors by decoding F16 manually.
//
// No external deps beyond vulkan-1.lib / dxgi.lib / Windows SDK.
//
// Exit codes: 0 = success (verify failures -> 1)
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// ------------------------------------------------------------ f16 -> f32 ---
// Bit-manipulation decode (handles subnormals, inf, nan) -- same routine as
// the known-good M0 probe.
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

// ------------------------------------------------------- safetensors ------
struct Tensor {
    std::string name;
    std::string dtype;
    std::vector<int64_t> shape;
    uint64_t off0 = 0;   // data offsets, relative to start of tensor data blob
    uint64_t off1 = 0;
    uint64_t vulkanOffset = 0; // offset inside the big device buffer
    static uint64_t eltSize(const std::string &d) {
        if (d == "F64" || d == "I64" || d == "U64") return 8;
        if (d == "F32" || d == "I32" || d == "U32") return 4;
        if (d == "F16" || d == "BF16" || d == "I16" || d == "U16") return 2;
        if (d == "I8" || d == "U8" || d == "BOOL") return 1;
        return 0;
    }
    uint64_t numel() const {
        uint64_t n = 1;
        for (auto s : shape) n *= (uint64_t)s;
        return n;
    }
    uint64_t nbytes() const { return off1 - off0; }
};

// Minimal hand-rolled JSON cursor for the safetensors header schema:
//   { "__metadata__": {...}, "name": {"dtype":s,"shape":[i..],"data_offsets":[i,i]}, ... }
struct JCur {
    const char *p;
    const char *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    [[noreturn]] void fail(const char *what) {
        std::fprintf(stderr, "JSON parse error: %s near byte %lld\n", what,
                     (long long)(p - end));
        std::exit(1);
    }
    char peek() { ws(); return p < end ? *p : '\0'; }
    char take() { ws(); return p < end ? *p++ : '\0'; }
    void expect(char c) { if (take() != c) fail("unexpected character"); }
    std::string string() {
        if (take() != '"') fail("expected string");
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        unsigned v = 0;
                        for (int i = 0; i < 4 && p + 1 < end; ++i) {
                            ++p;
                            v <<= 4;
                            char c = *p;
                            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                        }
                        s += (char)v; // enough for ascii-ish headers
                        break;
                    }
                    default: s += *p; break;
                }
                ++p;
            } else {
                s += *p++;
            }
        }
        if (p >= end) fail("unterminated string");
        ++p;
        return s;
    }
    long long integer() {
        ws();
        bool neg = false;
        if (p < end && *p == '-') { neg = true; ++p; }
        if (p >= end || *p < '0' || *p > '9') fail("expected integer");
        long long v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    }
    void skipValue() {
        char c = peek();
        if (c == '{') {
            take();
            if (peek() == '}') { take(); return; }
            while (true) {
                string();
                expect(':');
                skipValue();
                if (peek() == ',') { take(); continue; }
                expect('}');
                return;
            }
        } else if (c == '[') {
            take();
            if (peek() == ']') { take(); return; }
            while (true) {
                skipValue();
                if (peek() == ',') { take(); continue; }
                expect(']');
                return;
            }
        } else if (c == '"') {
            (void)string();
        } else {
            // number / true / false / null: consume token chars
            while (p < end && (*p == '-' || *p == '+' || *p == '.' ||
                               (*p >= '0' && *p <= '9') ||
                               (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                ++p;
        }
    }
};

struct SafeTensorsFile {
    std::vector<uint8_t> bytes;      // whole file (8B len + JSON header + data)
    std::map<std::string, Tensor> tensors;
    uint64_t headerLen = 0;
    std::map<std::string, std::string> metadata;

    const uint8_t *dataBlob() const { return bytes.data() + 8 + headerLen; }
};

static SafeTensorsFile loadSafetensors(const std::string &path) {
    SafeTensorsFile st;
    auto t0 = clk::now();
    {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "cannot open %s (err %lu)\n", path.c_str(), GetLastError());
            std::exit(1);
        }
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        st.bytes.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (got < st.bytes.size()) {
            DWORD rd = 0;
            if (!ReadFile(h, st.bytes.data() + got, (DWORD)std::min<size_t>(1u << 30, st.bytes.size() - got), &rd, nullptr) || rd == 0) {
                std::fprintf(stderr, "ReadFile failed at %zu\n", got);
                CloseHandle(h);
                std::exit(1);
            }
            got += rd;
        }
        CloseHandle(h);
    }
    std::printf("file load: %zu bytes read in %.2f ms\n", st.bytes.size(), msSince(t0));

    if (st.bytes.size() < 8 + 2) {
        std::fprintf(stderr, "file too small to be safetensors\n");
        std::exit(1);
    }
    // 8-byte LE header length
    uint64_t hl = 0;
    std::memcpy(&hl, st.bytes.data(), 8);
    st.headerLen = hl;
    if (8 + hl > st.bytes.size()) {
        std::fprintf(stderr, "header length %llu exceeds file size\n", (unsigned long long)hl);
        std::exit(1);
    }
    const char *hp = (const char *)st.bytes.data() + 8;
    JCur j{hp, hp + hl};
    j.expect('{');
    uint64_t maxEnd = 0;
    while (true) {
        char c = j.peek();
        if (c == '}') { j.take(); break; }
        std::string key = j.string();
        j.expect(':');
        if (key == "__metadata__") {
            j.expect('{');
            if (j.peek() != '}') {
                while (true) {
                    std::string mk = j.string();
                    j.expect(':');
                    std::string mv = j.peek() == '"' ? j.string() : "";
                    if (mv.empty()) j.skipValue();
                    st.metadata[mk] = mv;
                    if (j.peek() == ',') { j.take(); continue; }
                    break;
                }
            }
            j.expect('}');
        } else {
            Tensor t;
            t.name = key;
            j.expect('{');
            bool haveDtype = false, haveShape = false, haveOff = false;
            while (true) {
                std::string f = j.string();
                j.expect(':');
                if (f == "dtype") {
                    t.dtype = j.string();
                    haveDtype = true;
                } else if (f == "shape") {
                    j.expect('[');
                    if (j.peek() != ']') {
                        while (true) {
                            t.shape.push_back(j.integer());
                            if (j.peek() == ',') { j.take(); continue; }
                            break;
                        }
                    }
                    j.expect(']');
                    haveShape = true;
                } else if (f == "data_offsets") {
                    j.expect('[');
                    t.off0 = (uint64_t)j.integer();
                    j.expect(',');
                    t.off1 = (uint64_t)j.integer();
                    j.expect(']');
                    haveOff = true;
                } else {
                    j.skipValue();
                }
                if (j.peek() == ',') { j.take(); continue; }
                break;
            }
            j.expect('}');
            if (!haveDtype || !haveShape || !haveOff) {
                std::fprintf(stderr, "tensor %s missing fields\n", t.name.c_str());
                std::exit(1);
            }
            if (Tensor::eltSize(t.dtype) == 0) {
                std::fprintf(stderr, "tensor %s has unsupported dtype %s\n",
                             t.name.c_str(), t.dtype.c_str());
                std::exit(1);
            }
            if (t.off1 < t.off0) {
                std::fprintf(stderr, "tensor %s has inverted offsets\n", t.name.c_str());
                std::exit(1);
            }
            uint64_t shapeBytes = t.numel() * Tensor::eltSize(t.dtype);
            if (shapeBytes != t.nbytes()) {
                std::fprintf(stderr, "tensor %s: shape implies %llu bytes but data_offsets span %llu\n",
                             t.name.c_str(), (unsigned long long)shapeBytes,
                             (unsigned long long)t.nbytes());
                std::exit(1);
            }
            maxEnd = std::max(maxEnd, t.off1);
            st.tensors[key] = t;
        }
        if (j.peek() == ',') { j.take(); continue; }
    }
    // trailing padding inside the declared header length is allowed (spaces)
    while (j.p < j.end && (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r')) ++j.p;

    // ---- magic / metadata validation -------------------------------------
    if (st.metadata.count("format") == 0) {
        std::fprintf(stderr, "FAIL: header has no __metadata__.format (not a DLSSNR logical dump)\n");
        std::exit(1);
    }
    std::printf("metadata.format = %s\n", st.metadata["format"].c_str());
    auto it = st.metadata.find("decoded_tensor_count");
    if (it != st.metadata.end()) {
        long long declared = std::atoll(it->second.c_str());
        if (declared != (long long)st.tensors.size()) {
            std::fprintf(stderr, "FAIL: metadata decoded_tensor_count=%lld but header declares %zu tensors\n",
                         declared, st.tensors.size());
            std::exit(1);
        }
        std::printf("metadata.decoded_tensor_count = %lld (matches header)\n", declared);
    }
    uint64_t dataStart = 8 + hl;
    if (dataStart + maxEnd != st.bytes.size()) {
        std::fprintf(stderr,
                     "WARNING: file size %zu != 8 + header %llu + data %llu (=%llu)\n",
                     st.bytes.size(), (unsigned long long)hl, (unsigned long long)maxEnd,
                     (unsigned long long)(dataStart + maxEnd));
    } else {
        std::printf("size accounting: 8 + %llu header + %llu data = %llu bytes (matches file)\n",
                    (unsigned long long)hl, (unsigned long long)maxEnd,
                    (unsigned long long)(dataStart + maxEnd));
    }
    return st;
}

// ------------------------------------------------------------- grouping ----
struct LayerInfo {
    int index = -1;
    std::vector<const Tensor *> tensors;
    uint64_t bytes = 0;
};
struct BlockInfo {
    int index = -1;
    std::map<int, LayerInfo> layers;
    uint64_t bytes = 0;
    size_t tensors = 0;
};

static bool splitBlockLayer(const std::string &name, int &b, int &l) {
    // expects blockN.layerM.rest
    if (std::strncmp(name.c_str(), "block", 5) != 0) return false;
    const char *p = name.c_str() + 5;
    if (*p < '0' || *p > '9') return false;
    b = 0;
    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
    if (std::strncmp(p, ".layer", 6) != 0) return false;
    p += 6;
    if (*p < '0' || *p > '9') return false;
    l = 0;
    while (*p >= '0' && *p <= '9') l = l * 10 + (*p++ - '0');
    return *p == '.';
}

static std::string shortName(const std::string &name) {
    auto pos = name.find_last_of('.');
    return pos == std::string::npos ? name : name.substr(pos + 1);
}

static std::string shapeStr(const Tensor &t) {
    std::string s = "[";
    for (size_t i = 0; i < t.shape.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(t.shape[i]);
    }
    s += "]";
    return s;
}

int main(int argc, char **argv) {
    std::string file = R"(C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors)";
    bool doVerify = true; // default on; flag accepted for explicitness
    bool doStats = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) file = argv[++i];
        else if (a == "--verify") doVerify = true;
        else if (a == "--no-verify") doVerify = false;
        else if (a == "--stats") doStats = true;
        else if (a == "--no-stats") doStats = false;
        else {
            std::fprintf(stderr, "usage: m6loader [--file path] [--verify|--no-verify] [--stats|--no-stats]\n");
            return 1;
        }
    }

    std::printf("=== M6a safetensors weights loader (Arc Pro B50) ===\n");
    std::printf("file: %s\n", file.c_str());

    // ============================ 1. parse =================================
    SafeTensorsFile st = loadSafetensors(file);
    auto &T = st.tensors;

    uint64_t totalBytes = 0;
    std::map<std::string, size_t> dtypeHist;
    for (auto &[n, t] : T) {
        totalBytes += t.nbytes();
        dtypeHist[t.dtype]++;
    }
    std::printf("\n--- inventory ---\n");
    std::printf("tensor count: %zu\n", T.size());
    std::printf("total tensor bytes: %llu (%.2f MB)\n",
                (unsigned long long)totalBytes, (double)totalBytes / 1048576.0);
    std::printf("dtype histogram:");
    for (auto &[d, c] : dtypeHist) std::printf("  %s:%zu", d.c_str(), c);
    std::printf("\n");

    // ---- block grouping ---------------------------------------------------
    std::map<int, BlockInfo> blocks;
    size_t nonBlock = 0;
    for (auto &[n, t] : T) {
        int b, l;
        if (!splitBlockLayer(n, b, l)) { ++nonBlock; continue; }
        auto &bi = blocks[b];
        bi.index = b;
        auto &li = bi.layers[l];
        li.index = l;
        li.tensors.push_back(&t);
        li.bytes += t.nbytes();
        bi.bytes += t.nbytes();
        bi.tensors++;
    }
    std::printf("block groups: %zu (blocks %d..%d), non-block tensors: %zu\n",
                blocks.size(), blocks.begin()->first, blocks.rbegin()->first, nonBlock);

    std::printf("\n--- block summary (per-block bytes + dimension signature) ---\n");
    for (auto &[b, bi] : blocks) {
        std::printf("block%-2d | %zu tensors, %2zu layers, %8.2f MB\n",
                    b, bi.tensors, bi.layers.size(), (double)bi.bytes / 1048576.0);
        for (auto &[l, li] : bi.layers) {
            // sort tensors by name for a stable signature
            std::vector<const Tensor *> ts = li.tensors;
            std::sort(ts.begin(), ts.end(),
                      [](const Tensor *a, const Tensor *b) { return a->name < b->name; });
            std::string sig;
            for (auto *t : ts) {
                sig += " " + shortName(t->name) + shapeStr(*t);
            }
            std::printf("    layer%d:%s\n", l, sig.c_str());
        }
    }

    // ============================ 2. Vulkan ================================
    std::printf("\n--- vulkan init ---\n");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "m6-weights-loader";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    // DXGI adapter LUID (hardware, first non-software) for cross-API match.
    LUID dxgiLuid{};
    bool haveDxgiLuid = false;
    {
        IDXGIFactory1 *fx = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fx))) && fx) {
            IDXGIAdapter *a = nullptr;
            for (UINT i = 0; fx->EnumAdapters(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC d{};
                a->GetDesc(&d);
                char nm[128] = {0};
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof(nm) - 1, nullptr, nullptr);
                bool sw = d.VendorId == 0x1414;
                std::printf("[DXGI] adapter[%u] \"%s\" vendor=0x%04x LUID=%08lx:%08lx %s\n",
                            i, nm, d.VendorId, (unsigned long)d.AdapterLuid.HighPart,
                            (unsigned long)d.AdapterLuid.LowPart, sw ? "(software)" : "");
                if (!sw && !haveDxgiLuid) {
                    dxgiLuid = d.AdapterLuid;
                    haveDxgiLuid = true;
                }
                a->Release();
            }
            fx->Release();
        } else {
            std::printf("[DXGI] CreateDXGIFactory1 failed (err %lu); LUID match disabled\n",
                        GetLastError());
        }
    }

    uint32_t npd = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, nullptr));
    std::vector<VkPhysicalDevice> pds(npd);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds.data()));
    std::printf("vulkan physical devices: %u\n", npd);

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qfCompute = UINT32_MAX;
    {
        VkPhysicalDevice fallback = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < npd; ++i) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &id;
            vkGetPhysicalDeviceProperties2(pds[i], &p2);
            LUID l{};
            bool valid = id.deviceLUIDValid != VK_FALSE;
            if (valid) std::memcpy(&l, id.deviceLUID, VK_LUID_SIZE);
            bool match = valid && haveDxgiLuid &&
                         l.HighPart == dxgiLuid.HighPart && l.LowPart == dxgiLuid.LowPart;
            std::printf("[VK] device[%u] \"%s\" api %u.%u.%u LUID=%08lx:%08lx%s%s\n", i,
                        p2.properties.deviceName,
                        VK_API_VERSION_MAJOR(p2.properties.apiVersion),
                        VK_API_VERSION_MINOR(p2.properties.apiVersion),
                        VK_API_VERSION_PATCH(p2.properties.apiVersion),
                        (unsigned long)l.HighPart, (unsigned long)l.LowPart,
                        valid ? "" : " (invalid)",
                        match ? "  <== DXGI LUID MATCH" : "");
            if (fallback == VK_NULL_HANDLE) fallback = pds[i];
            if (match && pd == VK_NULL_HANDLE) pd = pds[i];
        }
        if (pd == VK_NULL_HANDLE) {
            std::printf("NOTE: no LUID match (or DXGI unavailable); falling back to device[0]\n");
            pd = fallback;
        }
    }
    if (pd == VK_NULL_HANDLE) {
        std::fprintf(stderr, "FAIL: no vulkan devices\n");
        return 1;
    }
    {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        id.pNext = &drv;
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        LUID l{};
        if (id.deviceLUIDValid) std::memcpy(&l, id.deviceLUID, VK_LUID_SIZE);
        std::printf("USING device: \"%s\" driver %s (%s) LUID=%08lx:%08lx\n",
                    p2.properties.deviceName, drv.driverName, drv.driverInfo,
                    (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    }
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps.data());
        for (uint32_t i = 0; i < nq; ++i)
            if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfCompute = i; break; }
        if (qfCompute == UINT32_MAX) {
            std::fprintf(stderr, "FAIL: no compute queue family\n");
            return 1;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfCompute;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfCompute, 0, &queue);

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    std::printf("memory heaps:");
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
        std::printf("  [%u] %.0f MB %s%s", i, (double)mem.memoryHeaps[i].size / 1048576.0,
                    (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "host",
                    i + 1 < mem.memoryHeapCount ? ";" : "");
    std::printf("\n");

    uint32_t mtHost = UINT32_MAX, mtDevice = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        uint32_t f = mem.memoryTypes[i].propertyFlags;
        if (mtHost == UINT32_MAX &&
            (f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            mtHost = i;
        if (mtDevice == UINT32_MAX &&
            (f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ==
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            mtDevice = i;
    }
    if (mtHost == UINT32_MAX || mtDevice == UINT32_MAX) {
        std::fprintf(stderr, "FAIL: required memory types missing (host=%u device=%u)\n",
                     mtHost, mtDevice);
        return 1;
    }
    std::printf("memory types: host-visible+coherent=%u device-local=%u\n", mtHost, mtDevice);

    // ---- layout: one device-local buffer, 256B-aligned tensor offsets ----
    const VkDeviceSize kAlign = 256;
    VkDeviceSize totalVk = 0;
    for (auto &[n, t] : T) {
        totalVk = (totalVk + kAlign - 1) & ~(VkDeviceSize)(kAlign - 1);
        t.vulkanOffset = (uint64_t)totalVk;
        totalVk += t.nbytes();
    }
    std::printf("\n--- upload plan ---\n");
    std::printf("device buffer size: %llu bytes (%.2f MB), alignment %llu\n",
                (unsigned long long)totalVk, (double)totalVk / 1048576.0,
                (unsigned long long)kAlign);

    VkBuffer staging = VK_NULL_HANDLE, deviceBuf = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory mStage = VK_NULL_HANDLE, mDev = VK_NULL_HANDLE, mRead = VK_NULL_HANDLE;
    auto makeBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage, uint32_t mt,
                       VkBuffer &buf, VkDeviceMemory &dm) {
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
        VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &dm));
        VK_CHECK(vkBindBufferMemory(dev, buf, dm, 0));
        return 0;
    };
    if (makeBuf(totalVk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, mtHost, staging, mStage)) return 1;
    if (makeBuf(totalVk,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                mtDevice, deviceBuf, mDev))
        return 1;

    VkCommandPool cpool;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = qfCompute;
        VK_CHECK(vkCreateCommandPool(dev, &ci, nullptr, &cpool));
    }
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cpool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    }
    VkFence fence;
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(dev, &fi, nullptr, &fence));
    }

    // ---- stage: memcpy file bytes into staging (timed per block) ----------
    std::printf("\n--- staging upload (host memcpy into coherent staging) ---\n");
    uint8_t *stagePtr = nullptr;
    VK_CHECK(vkMapMemory(dev, mStage, 0, VK_WHOLE_SIZE, 0, (void **)&stagePtr));
    const uint8_t *blob = st.dataBlob();

    std::map<int, double> stageMsPerBlock;
    auto tStage0 = clk::now();
    // deterministic order: block, layer, name
    {
        std::vector<const Tensor *> order;
        for (auto &[n, t] : T) order.push_back(&t);
        std::sort(order.begin(), order.end(), [](const Tensor *a, const Tensor *b) {
            int ab, al, bb, bl;
            bool abl = splitBlockLayer(a->name, ab, al);
            bool bbl = splitBlockLayer(b->name, bb, bl);
            if (abl != bbl) return abl > bbl;
            if (!abl) return a->name < b->name;
            if (ab != bb) return ab < bb;
            if (al != bl) return al < bl;
            return a->name < b->name;
        });
        int curBlock = -1;
        clk::time_point tb0;
        for (auto *t : order) {
            int b, l;
            bool isBlock = splitBlockLayer(t->name, b, l);
            int key = isBlock ? b : -1;
            if (key != curBlock) {
                if (curBlock != -1)
                    stageMsPerBlock[curBlock] += msSince(tb0);
                if (stageMsPerBlock.find(key) == stageMsPerBlock.end())
                    stageMsPerBlock[key] = 0.0;
                curBlock = key;
                tb0 = clk::now();
            }
            std::memcpy(stagePtr + t->vulkanOffset, blob + t->off0, (size_t)t->nbytes());
        }
        if (curBlock != -1) stageMsPerBlock[curBlock] += msSince(tb0);
    }
    double tStageTotal = msSince(tStage0);
    std::printf("staging memcpy total: %.2f ms (%.1f MB/s effective)\n",
                tStageTotal, (double)totalBytes / 1048576.0 / (tStageTotal / 1000.0));

    // ---- GPU copy: per-block submit + fence, timed ------------------------
    std::printf("\n--- GPU copy (staging -> device local, per-block submits) ---\n");
    std::map<int, double> gpuMsPerBlock;
    double gpuTotal = 0.0;
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;

        for (auto &[b, bi_] : blocks) {
            vkResetCommandBuffer(cmd, 0);
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            for (auto &[l, li] : bi_.layers)
                for (auto *t : li.tensors)
                    VkBufferCopy region{(VkDeviceSize)t->vulkanOffset,
                                        (VkDeviceSize)t->vulkanOffset,
                                        (VkDeviceSize)t->nbytes()};
            // (placeholder loop above replaced below; keep structure simple)
            vkEndCommandBuffer(cmd);
            break;
        }
    }
    // NOTE: the loop skeleton above is rewritten cleanly below (kept simple:
    // one submit per block with all its tensor copies).
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        auto submitBlock = [&](int key, const std::vector<const Tensor *> &ts) {
            vkResetCommandBuffer(cmd, 0);
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            for (auto *t : ts) {
                VkBufferCopy region{(VkDeviceSize)t->vulkanOffset,
                                    (VkDeviceSize)t->vulkanOffset,
                                    (VkDeviceSize)t->nbytes()};
                vkCmdCopyBuffer(cmd, staging, deviceBuf, 1, &region);
            }
            VK_CHECK(vkEndCommandBuffer(cmd));
            auto t0 = clk::now();
            VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
            VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
            VK_CHECK(vkResetFences(dev, 1, &fence));
            double m = msSince(t0);
            gpuMsPerBlock[key] = m;
            gpuTotal += m;
        };
        std::map<int, std::vector<const Tensor *>> perBlock;
        std::vector<const Tensor *> nonBlockT;
        for (auto &[n, t] : T) {
            int b, l;
            if (splitBlockLayer(n, b, l)) perBlock[b].push_back(&t);
            else nonBlockT.push_back(&t);
        }
        for (auto &[b, ts] : perBlock) {
            std::sort(ts.begin(), ts.end(),
                      [](const Tensor *a, const Tensor *c) { return a->name < c->name; });
            submitBlock(b, ts);
        }
        if (!nonBlockT.empty()) submitBlock(-1, nonBlockT);
    }
    std::printf("GPU copy total: %.2f ms (%.1f MB/s)\n", gpuTotal,
                (double)totalBytes / 1048576.0 / (gpuTotal / 1000.0));

    // per-block breakdown
    auto summarize = [](const char *what, const std::map<int, double> &m) {
        std::vector<double> v;
        for (auto &[k, d] : m)
            if (k >= 0) v.push_back(d);
        std::sort(v.begin(), v.end());
        if (v.empty()) return;
        std::printf("%s per block: min %.3f ms | median %.3f ms | max %.3f ms (n=%zu)\n",
                    what, v.front(), v[v.size() / 2], v.back(), v.size());
    };
    std::printf("\n--- per-block breakdown ---\n");
    summarize("staging", stageMsPerBlock);
    summarize("gpu copy", gpuMsPerBlock);
    std::printf("block timing detail (block: stage ms / gpu ms):\n");
    for (auto &[b, bi] : blocks) {
        std::printf("  block%-2d (%6.2f MB): %7.3f / %7.3f\n", b,
                    (double)bi.bytes / 1048576.0,
                    stageMsPerBlock.count(b) ? stageMsPerBlock[b] : -1.0,
                    gpuMsPerBlock.count(b) ? gpuMsPerBlock[b] : -1.0);
    }

    std::printf("\n--- VRAM budget ---\n");
    {
        std::printf("device-local buffer: %.2f MB\n", (double)totalVk / 1048576.0);
        std::printf("staging buffer (host heap): %.2f MB\n", (double)totalVk / 1048576.0);
        uint32_t heapDev = mem.memoryTypes[mtDevice].heapIndex;
        std::printf("device heap[%u] capacity: %.0f MB -> used by weights: %.2f MB (%.2f%%)\n",
                    heapDev, (double)mem.memoryHeaps[heapDev].size / 1048576.0,
                    (double)totalVk / 1048576.0,
                    100.0 * (double)totalVk / (double)mem.memoryHeaps[heapDev].size);
    }

    // ============================ 3. verify ================================
    if (doVerify) {
        std::printf("\n--- GPU residency verification (readback, byte-exact) ---\n");
        struct Sample {
            const char *name;
            const Tensor *t;
        };
        std::vector<Sample> samples;
        auto addSample = [&](const char *n) {
            auto it = T.find(n);
            if (it == T.end()) {
                std::fprintf(stderr, "verify: tensor %s not found\n", n);
                std::exit(1);
            }
            samples.push_back({n, &it->second});
        };
        addSample("block0.layer0.weight1");       // F16 weight matrix [32,128]
        addSample("block10.layer0.attn_scale");   // F32 [4]
        addSample("block35.layer2.attn_scale");   // F32 [32]
        VkDeviceSize rbSize = 0;
        std::vector<VkDeviceSize> rbOff;
        for (auto &s : samples) {
            rbOff.push_back(rbSize);
            rbSize += s.t->nbytes();
        }
        if (makeBuf(rbSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, mtHost, readback, mRead))
            return 1;
        {
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            for (size_t i = 0; i < samples.size(); ++i) {
                VkBufferCopy region{samples[i].t->vulkanOffset, rbOff[i],
                                    (VkDeviceSize)samples[i].t->nbytes()};
                vkCmdCopyBuffer(cmd, deviceBuf, readback, 1, &region);
            }
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
            VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        }
        uint8_t *rb = nullptr;
        VK_CHECK(vkMapMemory(dev, mRead, 0, VK_WHOLE_SIZE, 0, (void **)&rb));
        int fails = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const Tensor *t = samples[i].t;
            const uint8_t *expect = blob + t->off0;
            const uint8_t *got = rb + rbOff[i];
            bool same = std::memcmp(expect, got, (size_t)t->nbytes()) == 0;
            std::printf("  %-32s %s %5llu bytes: %s\n", samples[i].name,
                        t->dtype.c_str(), (unsigned long long)t->nbytes(),
                        same ? "IDENTICAL" : "MISMATCH");
            if (!same) ++fails;
        }
        vkUnmapMemory(dev, mRead);
        if (fails) {
            std::fprintf(stderr, "VERIFY FAIL: %d samples mismatched\n", fails);
            return 1;
        }
        std::printf("VERIFY PASS: all %zu sample tensors byte-identical after device round-trip\n",
                    samples.size());
    }

    // ============================ 4. stats =================================
    if (doStats) {
        std::printf("\n--- host-side sanity statistics (F16 decoded by bit ops) ---\n");
        struct StatReq {
            const char *name;
            const Tensor *t;
        };
        std::vector<StatReq> reqs;
        auto addStat = [&](const char *n) {
            auto it = T.find(n);
            if (it == T.end()) {
                std::printf("  NOTE: %s not in file (skipped)\n", n);
                return;
            }
            reqs.push_back({n, &it->second});
        };
        addStat("block0.layer0.weight1");
        addStat("block35.layer0.projection_weight"); // task-listed name
        if (!T.count("block35.layer0.projection_weight")) {
            // actual name in this dump is block35.layer0.weight [1024,4096]
            addStat("block35.layer0.weight");
            addStat("block35.layer4.projection_weight"); // [1024,1024]
        }
        bool allPlausible = true;
        for (auto &r : reqs) {
            const Tensor *t = r.t;
            if (t->dtype != "F16") {
                std::printf("  %s: dtype %s (stat path only decodes F16; skipped)\n",
                            r.name, t->dtype.c_str());
                continue;
            }
            const uint16_t *h = (const uint16_t *)(blob + t->off0);
            size_t n = (size_t)t->numel();
            double sum = 0, sq = 0, mn = 1e300, mx = -1e300;
            size_t nanCount = 0, infCount = 0, zeroCount = 0;
            for (size_t i = 0; i < n; ++i) {
                uint16_t bits = h[i];
                uint32_t e = (bits >> 10) & 0x1F, m = bits & 0x3FF;
                if (e == 31 && m != 0) { ++nanCount; continue; }
                float v = f16_to_f32(bits);
                if (e == 31) ++infCount;
                if (v == 0.0f) ++zeroCount;
                sum += v;
                sq += (double)v * v;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            double mean = sum / (double)n;
            double var = sq / (double)n - mean * mean;
            if (var < 0) var = 0;
            double sd = std::sqrt(var);
            const char *verdict;
            if (nanCount || infCount) verdict = "FLAG: NaN/Inf present";
            else if (zeroCount == n) verdict = "FLAG: all-zero";
            else if (std::fabs(mean) > 0.5) verdict = "FLAG: mean not near zero";
            else if (sd < 0.01 || sd > 1.0) verdict = "FLAG: std outside plausible 0.02-0.2-ish band";
            else verdict = "plausible";
            if (std::strncmp(verdict, "FLAG", 4) == 0) allPlausible = false;
            std::printf("  %-36s n=%8zu mean=%+.6f std=%.6f min=%+.6f max=%+.6f"
                        " nan=%zu inf=%zu zero=%zu  -> %s\n",
                        r.name, n, mean, sd, mn, mx, nanCount, infCount, zeroCount, verdict);
        }
        std::printf("weight stats verdict: %s\n",
                    allPlausible ? "ALL PLAUSIBLE" : "SOME FLAGS RAISED (see above)");
    }

    std::printf("\n--- timing summary ---\n");
    std::printf("file load      : see top of log\n");
    std::printf("staging upload : %.2f ms\n", tStageTotal);
    std::printf("gpu copy       : %.2f ms\n", gpuTotal);

    // ---- teardown ---------------------------------------------------------
    vkUnmapMemory(dev, mStage);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, cpool, nullptr);
    if (readback) vkDestroyBuffer(dev, readback, nullptr);
    if (mRead) vkFreeMemory(dev, mRead, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    vkDestroyBuffer(dev, deviceBuf, nullptr);
    vkFreeMemory(dev, mStage, nullptr);
    vkFreeMemory(dev, mDev, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);

    std::printf("\nRESULT: PASS\n");
    return 0;
}
