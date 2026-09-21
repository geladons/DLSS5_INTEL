// ============================================================================
// M8B-LIVE — the real DLSS 5 graph live on the desktop (DLSS5_INTEL)
//
// Transport is m4-present-simple VERBATIM (commit 5fee965): DDA capture ->
// D3D11 staging -> CPU bridge (persistent-mapped upload buffer) -> decode ->
// letterbox -> rescale down -> features (16ch volume) -> [REAL DLSSNR CHAIN]
// -> rescale up -> compose (residual composite) -> encode -> blit -> present
// to the borderless topmost click-through overlay swapchain. Wiggle activity
// generator, rolling fps + window title, verify readbacks on frames 10/30/60
// with M3 content checks — all unchanged.
//
// PROCESSING CORE SWAP (m4's standin.comp is gone):
//   * network extent is FIXED at 12x24 = the M8a chain's 24x12 = 288-token
//     grid (height=24, width=12; IMG_H/IMG_W per docs/m8-full-chain.md).
//   * features.comp writes the 16ch fp32 volume into a chain-arena slot
//     (descriptor binding 3 -> the single chain device buffer).
//   * featpack (new) converts fp32 -> f16 into the chain input slot oX16.
//   * the FULL VALIDATED 71-block DLSSNR chain (dlss5/m8-full-chain @03900ab,
//     1402 dispatches, ~47 ms/frame @288tok) runs in the same command buffer
//     via buffer device addresses — no descriptor sets, no shell-out.
//   * headpack (new) extracts the chain head output channels 0..3 (the real
//     head layout: RGB residual + temporal-gate logit; nr_model.py folds
//     out_gain [16,4] and out_conv [16,4] into rows 0-15/16-31 of the [32,16]
//     folded weight, GEMM N padded 16 -> cols 4-15 are exact zeros) into the
//     binding-4 slot that rescale-up + compose consume unchanged.
//   * weights: all 649 logical tensors in ONE device-local VkBuffer per
//     docs/pack-layout.txt (256-aligned, 291,535,872 B) + fp32 side tables
//     and scratch arena after the pack. Loaded ONCE at startup (~1.3 s).
//
// CLI: --frames N (default 300), --nowiggle, --scale S (accepted, IGNORED —
// the network grid is pinned to the chain's 288 tokens), --novideo (no
// overlay window/swapchain/present: capture + full chain + verify only —
// chain-throughput sanity mode), --max-delta N (feedback delta clamp,
// default 12), --settle-thresh X (settle detection, default 0.5).
//
// OVERLAY FEEDBACK CANCELLATION (2026-09-20, the "rainbow explosion" fix):
// the fullscreen topmost overlay is itself part of the desktop DDA captures,
// so processing the raw capture re-processes our own output and the
// composite residual compounds every frame (screen-wide color garbage,
// GPU pegged, crash; readback metrics still "PASS" — that path was verified
// only via --novideo, which never showed the overlay). Fix:
//   * fbcancel.comp: corrected = clamp(capture - lastPresented, +/-4*maxDelta)
//     (safety clamp only — real content must pass; the tight divergence
//     bound is the composite residual clamp in compose) written as float RGB
//     into the decode slot; the chain processes the echo-cancelled change,
//     not the raw capture:
//         presented(n) = capture(n) + deltaNet(n)   [echo cancels exactly]
//     bufLastPresented = exact copy of the last presented frame (imgFinal ->
//     buffer copy every processed frame). The kernel runs FUSED in the main
//     frame command buffer — a separate pre-pass submission measured
//     ~168 ms/frame (cold-queue round trip); the buffers stay DEVICE-LOCAL
//     because host-visible STORAGE buffers fault the Arc 32.0.101.8805
//     driver (DEVICE_LOST within two frames, measured 2026-09-20).
//   * compose.comp: in accumulate mode only the network's composite residual
//     is clamped to +/-maxDelta/255 (default 12) — never real content, else
//     the screen can never appear from black (frame 0 corrects against a
//     ZEROED lastPresented, i.e. the "delta" IS the whole desktop).
//     `fin` = corrected + clamped residual.
//   * encode.comp (accumulate mode): out = clamp(lastPresented + fin), i.e.
//     presented = enhanced capture; per-frame screen motion contributed by
//     the net is bounded by +/-maxDelta so it cannot diverge.
//   * settle detection: when the sparse capture-sample diff (CPU, free) is
//     below --settle-thresh the visible screen is static, so the chain +
//     present are SKIPPED entirely before any GPU submission (~0% GPU when
//     settled); the overlay keeps showing the last presented frame. The
//     full-frame mean |corrected delta| comes from the fbcancel GPU counter
//     and is logged per processed frame.
//   * the overlay window stays hidden until the first present so the
//     initial captures see the real desktop, never a blank swapchain client.
// Exit codes: 0 = ran + verify-frame content checks pass, 1 = any failure.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl.h>

#include <vulkan/vulkan.h>

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

static constexpr uint32_t W = 2560, H = 1440;

// ----------------------------------------------------------- f16 <-> f32 ---
// (from m8-full-chain main.cpp — host-side widening of the f16 side tables)
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

// ---------------------------------------------------------------- BMP I/O --
static bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                         uint32_t w, uint32_t h) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t rowBytes = w * 4;
    const uint32_t imgBytes = rowBytes * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgBytes;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h; // positive -> bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    f.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    for (uint32_t y = h; y-- > 0;) {
        const uint8_t* row = &bgra[(size_t)y * rowBytes];
        f.write(reinterpret_cast<const char*>(row), rowBytes);
        if (!f) return false;
    }
    return f.good();
}

// ---------------------------------------------------------- wiggle thread --
static std::atomic<bool> g_wiggleStop{false};
static void WiggleThread() {
    bool dir = false;
    while (!g_wiggleStop.load()) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dx = dir ? 4 : -4;
        in.mi.dy = dir ? 2 : -2;
        SendInput(1, &in, sizeof(in));
        dir = !dir;
        Sleep(8);
    }
}

// ------------------------------------------- reference ports (host logic) --
struct Region { int top, bottom, left, right; };
static Region ActiveRegion(const float* rowMax, const float* colMax, int height, int width) {
    const float tolerance = 2.0f / 255.0f;
    const double limit = 0.45;
    auto run = [&](const float* v, int extent, int& lo, int& hi) {
        int cap = (int)(extent * limit);
        int lead = 0, tail = 0;
        while (lead < cap && v[lead] <= tolerance) ++lead;
        while (tail < cap && v[extent - 1 - tail] <= tolerance) ++tail;
        if (lead && tail && std::abs(lead - tail) <= 1) { lo = lead; hi = extent - tail; }
        else { lo = 0; hi = extent; }
    };
    Region r{};
    run(rowMax, height, r.top, r.bottom);
    run(colMax, width, r.left, r.right);
    if ((int64_t)(r.bottom - r.top) * (r.right - r.left) < (int64_t)height * width / 2)
        r = Region{0, height, 0, width};
    return r;
}

// ------------------------------------------------------------------- main --
#define VK_CHECK(x)                                                             \
    do {                                                                        \
        VkResult res_ = (x);                                                    \
        if (res_ != VK_SUCCESS) {                                               \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                   \
            return 1;                                                           \
        }                                                                       \
    } while (0)
#define VK_FATAL(x)                                                             \
    do {                                                                        \
        VkResult res_ = (x);                                                    \
        if (res_ != VK_SUCCESS) {                                               \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                   \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)
#define HR_CHECK(x)                                                             \
    do {                                                                        \
        HRESULT hr_ = (x);                                                      \
        if (FAILED(hr_)) {                                                      \
            std::fprintf(stderr, "[D3D-FAIL] %s -> 0x%08lx at %s:%d\n", #x,     \
                         (unsigned long)hr_, __FILE__, __LINE__);               \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const char* HrName(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_ACCESS_LOST:  return "DXGI_ERROR_ACCESS_LOST";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_SESSION_DISCONNECTED:    return "DXGI_ERROR_SESSION_DISCONNECTED";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case E_INVALIDARG:   return "E_INVALIDARG";
        case E_OUTOFMEMORY:  return "E_OUTOFMEMORY";
        default:             return "(other)";
    }
}
static std::string LuidStr(const LUID& l) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08lx:%08lx",
                  (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    return buf;
}

// -------------------------------------------------- vulkan small helpers --
struct VkCtx {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties memProps;
};

static uint32_t FindMemType(const VkCtx& c, uint32_t typeBits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        if ((c.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

static VkBuffer CreateBuf(const VkCtx& c, uint64_t bytes, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags want, VkDeviceMemory* memOut) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, buf, &req);
    uint32_t idx = FindMemType(c, req.memoryTypeBits, want);
    if (idx == UINT32_MAX) {
        idx = FindMemType(c, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (idx == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no memory type for buffer\n"); std::exit(1); }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = idx;
    VkDeviceMemory mem;
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
    VK_FATAL(vkBindBufferMemory(c.dev, buf, mem, 0));
    *memOut = mem;
    return buf;
}

static VkImage CreateImage2D(const VkCtx& c, uint32_t w, uint32_t h, VkFormat fmt,
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
    VK_FATAL(vkCreateImage(c.dev, &ici, nullptr, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.dev, img, &req);
    uint32_t idx = FindMemType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = idx;
    VkDeviceMemory mem;
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
    VK_FATAL(vkBindImageMemory(c.dev, img, mem, 0));
    *memOut = mem;
    return img;
}

static VkImageView CreateView(const VkCtx& c, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v;
    VK_FATAL(vkCreateImageView(c.dev, &vci, nullptr, &v));
    return v;
}

static VkShaderModule LoadSpv(const VkCtx& c, const std::string& file) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "[FAIL] cannot open %s\n", file.c_str()); std::exit(1); }
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> code((size_t)sz);
    if (!f.read(code.data(), sz)) { std::fprintf(stderr, "[FAIL] read spv\n"); std::exit(1); }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = (size_t)sz;
    smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_FATAL(vkCreateShaderModule(c.dev, &smci, nullptr, &m));
    return m;
}

struct Push { int32_t a[4]; int32_t b[4]; float c[4]; float d[4]; };

// ------------------------------------------------------- overlay window ----
static HWND g_hwnd = nullptr;
static std::atomic<bool> g_stop{false};
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static DWORD WINAPI MsgPumpThread(LPVOID) {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop.store(true);
    return TRUE;
}

// ===========================================================================
// M8a chain constants + safetensors parser (verbatim from m8-full-chain;
// the chain runs at a FIXED 24x12 = 288-token grid — see docs/m8-full-chain.md)
// ===========================================================================
static const uint32_t TOK = 288;      // tokens = 24 rows x 12 cols
static const uint32_t IMG_H = 24, IMG_W = 12;

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

static int readFile(const std::string &path, std::vector<char> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 1;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (!f.read(out.data(), sz)) return 1;
    return 0;
}

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

// --------------------------------------------- chain buffers + pipelines ---
struct ChainBuf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

static uint32_t chainMemType(const VkCtx &c, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i)
        if ((c.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
    std::exit(1);
}

static void chainAllocDev(const VkCtx &c, VkDeviceSize size, ChainBuf &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo mafi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &mafi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    b.size = size;
}

static void chainAllocHost(const VkCtx &c, VkDeviceSize size, ChainBuf &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    VK_FATAL(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    b.size = size;
}

struct ChainPipe {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;   // layouts reference it — keep alive
};

static void chainMakePipe(const VkCtx &c, const std::string &spv, ChainPipe &out) {
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout dsl;
    VK_FATAL(vkCreateDescriptorSetLayout(c.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_FATAL(vkCreatePipelineLayout(c.dev, &plci, nullptr, &out.layout));
    VkShaderModule mod = LoadSpv(c, spv);
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = out.layout;
    VK_FATAL(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out.pipe));
    vkDestroyShaderModule(c.dev, mod, nullptr);
    out.dsl = dsl;   // NOT destroyed: pipeline layout references it (device-lost on Arc otherwise)
}

static void chainFullBarrier(VkCommandBuffer cb, VkBuffer arena) {
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

// push blocks (scalar layout) — verbatim from m8-full-chain main.cpp
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

struct PackPush { uint64_t src, dst; uint32_t n; };
// headpack two-pass push: mode 0 accumulates per-channel DC, mode 1 writes
// the calibrated matched residual (see shaders/m8/headpack.comp).
struct HeadPush { uint64_t src, dst, dc; uint32_t ntok; float gain; uint32_t mode; };
#pragma pack(pop)

enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1 };

int main(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();
    setvbuf(stdout, nullptr, _IONBF, 0);   // live diagnostics: crash/hang forensics
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    g_hasF16C = (cpuInfo[2] & (1 << 29)) != 0;

    // ---------------- CLI
    long framesTarget = 300;
    int novideo = 0;
    float renderScale = 0.55f;   // accepted for CLI compat; grid is pinned to 288 tokens
    long wiggleIdleSec = 0;      // --wiggle-idle N: engage the gentle cursor generator
                                 // only after N seconds with no desktop updates (default OFF)
    bool wiggleForbidden = false;   // --nowiggle: hard-disable any wiggle
    int maxDelta = 12;           // --max-delta N: per-channel feedback-delta clamp (1/255 units)
    double settleThresh = 0.5;   // --settle-thresh X: skip chain+present when mean|corrected delta|
                                 // (0-255 units) stays below this (settle detection, video mode)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) framesTarget = std::atol(argv[++i]);
        else if (a == "--nowiggle") wiggleForbidden = true;
        else if (a == "--novideo") novideo = 1;
        else if (a == "--wiggle-idle" && i + 1 < argc) wiggleIdleSec = std::atol(argv[++i]);
        else if (a == "--max-delta" && i + 1 < argc) maxDelta = std::atoi(argv[++i]);
        else if (a == "--settle-thresh" && i + 1 < argc) settleThresh = std::atof(argv[++i]);
        else if (a == "--scale" && i + 1 < argc) renderScale = (float)std::atof(argv[++i]);
        else { std::fprintf(stderr, "usage: m8blive [--frames N] [--nowiggle] [--novideo] [--scale S(ignored)] [--wiggle-idle SECS] [--max-delta N] [--settle-thresh X]\n"); return 1; }
    }
    if (wiggleForbidden) wiggleIdleSec = 0;
    if (maxDelta < 1) maxDelta = 1;
    if (maxDelta > 255) maxDelta = 255;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    std::printf("=== M8B-LIVE: REAL DLSS 5 graph (71 blocks) live on the desktop, Intel Arc Pro B50 ===\n");
    std::printf("transport: m4-present-simple (CPU bridge) | core: m8-full-chain @03900ab (288 tokens, ~47 ms)\n");
    std::printf("frame target: %ux%u B8G8R8A8, frames=%ld (processed), wiggle-idle=%lds, video=%d\n",
                W, H, framesTarget, wiggleIdleSec, !novideo);
    if (!novideo)
        std::printf("feedback-cancellation: ON (echo subtract + accumulate, max-delta=%d/255, settle-thresh=%.3f)\n",
                    maxDelta, settleThresh);
    if (renderScale != 0.55f)
        std::printf("[info] --scale ignored: network grid pinned to 12x24 = the chain's 288 tokens\n");
    std::printf("\n");
    CreateDirectoryA("out", nullptr);

    // ===================================================================
    // 1. D3D11 hardware device + DDA (M4 path; staging texture only).
    // ===================================================================
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       &device, &got, &context);
        if (FAILED(hr)) { std::fprintf(stderr, "[FAIL] D3D11CreateDevice: %s\n", HrName(hr)); return 1; }
        std::printf("[D3D11] hardware device created, feature level 0x%x\n", (unsigned)got);
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    HR_CHECK(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
    ComPtr<IDXGIAdapter> adapter;
    HR_CHECK(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC adesc{};
    adapter->GetDesc(&adesc);

    ComPtr<IDXGIOutput> output;
    int outX = 0, outY = 0;
    {
        ComPtr<IDXGIOutput> o;
        UINT i = 0;
        ComPtr<IDXGIOutput> firstAttached, display5;
        while (adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            char n[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, n, sizeof(n) - 1, nullptr, nullptr);
            if (d.AttachedToDesktop && !firstAttached) firstAttached = o;
            if (d.AttachedToDesktop && std::strcmp(n, "\\\\.\\DISPLAY5") == 0) display5 = o;
            ++i; o.Reset();
        }
        output = display5 ? display5 : firstAttached;
        if (!output) { std::fprintf(stderr, "[FAIL] no attached output\n"); return 1; }
        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        outX = od.DesktopCoordinates.left;
        outY = od.DesktopCoordinates.top;
    }
    ComPtr<IDXGIOutputDuplication> dup;
    {
        ComPtr<IDXGIOutput1> out1;
        HR_CHECK(output->QueryInterface(IID_PPV_ARGS(&out1)));
        HRESULT hr = out1->DuplicateOutput(device.Get(), &dup);
        if (FAILED(hr)) { std::fprintf(stderr, "[FAIL] DuplicateOutput: %s\n", HrName(hr)); return 1; }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        std::printf("[DDA] DuplicateOutput active: %ux%u format=%d rotation=%d origin=(%d,%d)\n",
                    (unsigned)dd.ModeDesc.Width, (unsigned)dd.ModeDesc.Height,
                    (int)dd.ModeDesc.Format, (int)dd.Rotation, outX, outY);
        if (dd.ModeDesc.Width != W || dd.ModeDesc.Height != H ||
            dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            std::fprintf(stderr, "[FAIL] expected %ux%u B8G8R8A8\n", W, H); return 1;
        }
    }
    ComPtr<ID3D11Texture2D> stagingTex;
    {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        HR_CHECK(device->CreateTexture2D(&sd, nullptr, &stagingTex));
    }

    // ===================================================================
    // 2. Initial content-checked DDA frame (M3/M4 doctrine).
    //    Wiggle is OFF by default (user demand: no cursor wiggle). With
    //    --wiggle-idle N it runs for the initial acquire too and is then
    //    handed to the live loop's idle logic (stopped on the first frame).
    // ===================================================================
    std::vector<uint8_t> original(W * H * 4);   // last good native frame
    std::thread wiggler;
    if (wiggleIdleSec > 0) {
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
        std::printf("[info] cursor-wiggle generator armed (wiggle-idle=%lds)\n", wiggleIdleSec);
    }
    {
        auto tAcquireBegin = clk::now();
        auto deadline = tAcquireBegin + std::chrono::seconds(8);
        auto acceptAnyAt = tAcquireBegin + std::chrono::seconds(6);
        bool got = false;
        int tries = 0, acquired = 0, skippedBlack = 0;
        while (clk::now() < deadline && !got && !g_stop.load()) {
            ++tries;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(500, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { std::fprintf(stderr, "[error] AcquireNextFrame: %s\n", HrName(hr)); continue; }
            ComPtr<ID3D11Texture2D> frameTex;
            hr = res->QueryInterface(IID_PPV_ARGS(&frameTex));
            if (FAILED(hr)) { dup->ReleaseFrame(); continue; }
            ++acquired;
            context->CopyResource(stagingTex.Get(), frameTex.Get());
            context->Flush();
            dup->ReleaseFrame();
            bool black = true;
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                double s = 0; uint64_t cnt = 0;
                for (uint32_t y = 0; y < H; y += 24) {
                    const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
                    for (uint32_t x = 0; x < W * 4; x += 1997) { s += row[x]; ++cnt; }
                }
                context->Unmap(stagingTex.Get(), 0);
                black = (cnt == 0) || (s / (double)cnt) < 1.0;
            }
            if (black && clk::now() < acceptAnyAt) { ++skippedBlack; continue; }
            got = true;
            std::printf("[DDA] initial %s frame on try %d (acquired#%d, skippedBlack=%d)\n",
                        black ? "FALLBACK-black" : "content", tries, acquired, skippedBlack);
        }
        // The initial acquire is done — always stop the generator here; the
        // live loop re-arms it only after wiggleIdleSec of continuous idleness.
        if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
        if (!got) { std::fprintf(stderr, "[FAIL] no DDA frame within ~8s\n"); return 1; }
        D3D11_MAPPED_SUBRESOURCE ms{};
        HR_CHECK(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms));
        for (uint32_t y = 0; y < H; ++y)
            std::memcpy(&original[(size_t)y * W * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
        context->Unmap(stagingTex.Get(), 0);
        WriteBmpBGRA("out\\m8b_native0.bmp", original, W, H);
        {
            double s = 0; for (size_t i = 0; i < original.size(); i += 4003) s += original[i];
            std::printf("[cap] native sample mean=%.2f (0 => capture black)\n",
                        s / (double)(original.size() / 4003 + 1));
        }
    }

    // ===================================================================
    // 3. Vulkan: instance (with win32 surface) + LUID-matched device
    //    (+ M8a requirements: cooperative matrices + BDA + f16/memory-model).
    // ===================================================================
    VkCtx c{};
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "m8b-live";
        app.apiVersion = VK_API_VERSION_1_2;
        const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = iext;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &c.inst));
    }
    {
        uint32_t n = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(c.inst, &n, nullptr));
        std::vector<VkPhysicalDevice> pds(n);
        VK_CHECK(vkEnumeratePhysicalDevices(c.inst, &n, pds.data()));
        for (auto d : pds) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &id;
            vkGetPhysicalDeviceProperties2(d, &p2);
            LUID l{};
            if (id.deviceLUIDValid) std::memcpy(&l, id.deviceLUID, VK_LUID_SIZE);
            bool match = id.deviceLUIDValid &&
                         l.HighPart == adesc.AdapterLuid.HighPart &&
                         l.LowPart == adesc.AdapterLuid.LowPart;
            std::printf("[VK] device \"%s\" LUID=%s %s\n", p2.properties.deviceName,
                        id.deviceLUIDValid ? LuidStr(l).c_str() : "(invalid)",
                        match ? "  <== MATCHES DXGI adapter" : "");
            if (match && !c.pd) c.pd = d;
        }
        if (!c.pd) { std::fprintf(stderr, "[FAIL] no Vulkan device LUID-matches DXGI\n"); return 1; }
        // M8a requirement: cooperative matrices 8x16x16 f16xf16->f32 (chain GEMMs)
        {
            uint32_t next = 0;
            vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &next, nullptr);
            std::vector<VkExtensionProperties> exts(next);
            vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &next, exts.data());
            bool hasCoop = false;
            for (auto &e : exts)
                if (std::strcmp(e.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0) hasCoop = true;
            if (!hasCoop) { std::fprintf(stderr, "[FAIL] matched device lacks VK_KHR_cooperative_matrix\n"); return 1; }
            auto fpEnum = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
                vkGetInstanceProcAddr(c.inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
            uint32_t ncfg = 0;
            VK_CHECK(fpEnum(c.pd, &ncfg, nullptr));
            std::vector<VkCooperativeMatrixPropertiesKHR> cfgs(ncfg,
                VkCooperativeMatrixPropertiesKHR{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR, nullptr});
            VK_CHECK(fpEnum(c.pd, &ncfg, cfgs.data()));
            bool ok816 = false;
            for (auto &cm : cfgs)
                if (cm.MSize == 8 && cm.NSize == 16 && cm.KSize == 16 &&
                    cm.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && cm.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                    cm.CType == VK_COMPONENT_TYPE_FLOAT32_KHR)
                    ok816 = true;
            if (!ok816) { std::fprintf(stderr, "[FAIL] 8x16x16 f16xf16->f32 coopmat config missing\n"); return 1; }
            std::printf("[VK] coopmat 8x16x16 f16xf16->f32: OK\n");
        }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, qps.data());
        c.qf = UINT32_MAX;
        for (uint32_t i = 0; i < nq; ++i)
            if ((qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                c.qf = i; break;
            }
        if (c.qf == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no compute+graphics queue\n"); return 1; }
        vkGetPhysicalDeviceMemoryProperties(c.pd, &c.memProps);
    }

    // ===================================================================
    // 4. Borderless topmost click-through overlay window + surface
    //    (skipped entirely in --novideo sanity mode).
    // ===================================================================
    HANDLE msgThread = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!novideo) {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = L"DLSS5M8bLiveOverlay";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        if (!RegisterClassW(&wc)) { std::fprintf(stderr, "[FAIL] RegisterClassW %lu\n", GetLastError()); return 1; }
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        g_hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"DLSS5_INTEL m8b",
                                 WS_POPUP, outX, outY, W, H,
                                 nullptr, nullptr, hinst, nullptr);
        if (!g_hwnd) { std::fprintf(stderr, "[FAIL] CreateWindowExW %lu\n", GetLastError()); return 1; }
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
        // M8B feedback fix: the window stays HIDDEN until the first present.
        // Shown earlier, the never-presented swapchain client composites as a
        // black rectangle over the desktop and the first DDA captures (and the
        // frame-0 region scan) would see a black screen instead of the desktop.
        // ShowWindow/SetWindowPos happen right before the first vkQueuePresentKHR.
        std::printf("[win] overlay created (HIDDEN until first present): %ux%u at (%d,%d), WS_POPUP | TOPMOST | TRANSPARENT | LAYERED\n",
                    W, H, outX, outY);
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(c.inst, &sci, nullptr, &surface));
        DWORD mtid = 0;
        msgThread = CreateThread(nullptr, 0, MsgPumpThread, nullptr, 0, &mtid);
    }

    // ---------------- device with swapchain + cooperative-matrix support
    {
        const char* want[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = c.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = want;
        // m8 shaders declare Float16/Int64/16-bit storage/memory-model/BDA/coopmat —
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
        VK_CHECK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev));
        vkGetDeviceQueue(c.dev, c.qf, 0, &c.queue);
        std::printf("[VK] logical device up (compute+graphics family %u, swapchain + coopmat)\n", c.qf);
    }

    // ---------------- swapchain: 3 images, B8G8R8A8, MAILBOX if offered
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool swapStorage = false;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    if (!novideo) {
        VkBool32 sup = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(c.pd, c.qf, surface, &sup);
        if (!sup) { std::fprintf(stderr, "[FAIL] queue family cannot present to surface\n"); return 1; }
        VkSurfaceCapabilitiesKHR surfCaps{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.pd, surface, &surfCaps));
        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(c.pd, surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(nf);
        vkGetPhysicalDeviceSurfaceFormatsKHR(c.pd, surface, &nf, fmts.data());
        bool picked = false;
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { picked = true; break; }
        if (!picked && nf) swapFormat = fmts[0].format;
        uint32_t nm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(c.pd, surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(c.pd, surface, &nm, modes.data());
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = m; break; }
        swapStorage = (surfCaps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
        bool xferDst = (surfCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
        if (!swapStorage && !xferDst) {
            std::fprintf(stderr, "[FAIL] swapchain images support neither STORAGE nor TRANSFER_DST\n"); return 1;
        }
        uint32_t imageCount = surfCaps.minImageCount + 1;
        if (imageCount < 3) imageCount = 3;
        if (surfCaps.maxImageCount && imageCount > surfCaps.maxImageCount)
            imageCount = surfCaps.maxImageCount;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = imageCount;
        ci.imageFormat = swapFormat;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = {W, H};
        ci.imageArrayLayers = 1;
        ci.imageUsage = swapStorage ? (VkImageUsageFlags)(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                    : (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = surfCaps.currentTransform;
        ci.compositeAlpha = (surfCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        ci.presentMode = presentMode;
        ci.clipped = VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(c.dev, &ci, nullptr, &swapchain));
        uint32_t ni = 0;
        vkGetSwapchainImagesKHR(c.dev, swapchain, &ni, nullptr);
        swapImages.resize(ni);
        vkGetSwapchainImagesKHR(c.dev, swapchain, &ni, swapImages.data());
        for (auto im : swapImages) swapViews.push_back(CreateView(c, im, swapFormat));
        std::printf("[present] format=%d images=%u mode=%s path=%s\n", (int)swapFormat, ni,
                    presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO",
                    swapStorage ? "blit.comp STORAGE direct" : "vkCmdCopyImage");
    }

    // ===================================================================
    // 5. Plain Vulkan input image + persistent-mapped upload buffer (CPU bridge).
    // ===================================================================
    VkDeviceMemory memImgIn = VK_NULL_HANDLE, memUpload = VK_NULL_HANDLE;
    VkImage imgIn = CreateImage2D(c, W, H, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &memImgIn);
    VkImageView imgInView = CreateView(c, imgIn, VK_FORMAT_B8G8R8A8_UNORM);
    VkBuffer bufUpload = CreateBuf(c, (uint64_t)W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &memUpload);
    void* uploadPtr = nullptr;
    VK_CHECK(vkMapMemory(c.dev, memUpload, 0, (uint64_t)W * H * 4, 0, &uploadPtr));
    std::printf("[bridge] upload buffer %llu bytes persistently mapped (CPU bridge; zero-copy deferred to M5)\n",
                (unsigned long long)((uint64_t)W * H * 4));

    // ===================================================================
    // 6. Shared pipeline layout + 7 transport pipelines (standin REMOVED —
    //    the real 71-block chain replaces it) + 12 chain pipelines.
    // ===================================================================
    VkDescriptorSetLayout dsLayout;
    {
        VkDescriptorSetLayoutBinding binds[12]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 1; i <= 8; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};  // bufLastPresented
        binds[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};  // fbStats (mapped)
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 12; ci.pBindings = binds;
        VK_CHECK(vkCreateDescriptorSetLayout(c.dev, &ci, nullptr, &dsLayout));
    }
    VkPipelineLayout pipeLayout;
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1; ci.pSetLayouts = &dsLayout;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(c.dev, &ci, nullptr, &pipeLayout));
    }
    auto makePipe = [&](const char* name) {
        std::string path;
        {
            char buf[MAX_PATH];
            GetModuleFileNameA(nullptr, buf, MAX_PATH);
            path = buf;
            path.resize(path.find_last_of('\\') + 1);
            path += name; path += ".spv";
        }
        VkShaderModule sm = LoadSpv(c, path);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = sm; ci.stage.pName = "main";
        ci.layout = pipeLayout;
        VkPipeline p;
        VK_FATAL(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        vkDestroyShaderModule(c.dev, sm, nullptr);
        return p;
    };
    VkPipeline pipeDecode = makePipe("decode");
    VkPipeline pipeLetter = makePipe("letterbox");
    VkPipeline pipeRescale = makePipe("rescale");
    VkPipeline pipeFeatures = makePipe("features");
    VkPipeline pipeCompose = makePipe("compose");
    VkPipeline pipeEncode = makePipe("encode");
    VkPipeline pipeBlit = makePipe("blit");
    VkPipeline pipeFbcancel = makePipe("fbcancel");
    std::printf("[VK] 7 transport pipelines up (+ blit + fbcancel); standin REMOVED\n");

    VkCommandPool cmdPool;
    VkCommandBuffer cmd;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = c.qf;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(c.dev, &ci, nullptr, &cmdPool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(c.dev, &ai, &cmd));
    }
    VkFence fence;
    {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(c.dev, &fci, nullptr, &fence));
    }
    // per-stage GPU timestamp instrumentation (Task 2 perf diagnosis):
    // slots 0..5 written at front-end start/end, featpack end, chain end,
    // headpack end, encode end. Deltas * limits.timestampPeriod = ns.
    VkQueryPool tsPool = VK_NULL_HANDLE;
    float tsPeriodNs = 1.0f;
    {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 6;
        VK_CHECK(vkCreateQueryPool(c.dev, &qpci, nullptr, &tsPool));
        VkPhysicalDeviceProperties pp{};
        vkGetPhysicalDeviceProperties(c.pd, &pp);
        tsPeriodNs = pp.limits.timestampPeriod;
    }
    VkSemaphore semImage = VK_NULL_HANDLE, semRender = VK_NULL_HANDLE;
    {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(c.dev, &sci, nullptr, &semImage));
        VK_CHECK(vkCreateSemaphore(c.dev, &sci, nullptr, &semRender));
    }
    // one-shot submit helper for startup copies (own temp cmd buffer)
    auto submitOneShot = [&](VkCommandBuffer one) -> double {
        VK_FATAL(vkEndCommandBuffer(one));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &one;
        auto t0 = clk::now();
        VK_FATAL(vkQueueSubmit(c.queue, 1, &si, fence));
        VK_FATAL(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(c.dev, 1, &fence);
        vkFreeCommandBuffers(c.dev, cmdPool, 1, &one);
        return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    };
    auto beginOneShot = [&]() -> VkCommandBuffer {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        VK_FATAL(vkAllocateCommandBuffers(c.dev, &ai, &cb));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_FATAL(vkBeginCommandBuffer(cb, &bi));
        return cb;
    };
    auto submitAndWait = [&]() -> double {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        auto t0 = clk::now();
        VK_CHECK(vkQueueSubmit(c.queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(c.dev, 1, &fence);
        return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    };
    auto begin = [&]() {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    };
    auto barrierAll = [&]() {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    auto push = [&](const Push& p) {
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    };

    // ===================================================================
    // 7. Geometry: network extent PINNED to the chain's 288-token grid
    //    (12 wide x 24 tall = m8 IMG_W x IMG_H; --scale ignored).
    // ===================================================================
    const int netW = (int)IMG_W;    // 12
    const int netH = (int)IMG_H;    // 24
    std::printf("[geometry] network extent PINNED at %dx%d = %u tokens (m8 chain grid)\n",
                netW, netH, TOK);

    // ===================================================================
    // 8. DLSSNR weight residency (m8a §1-3): one device buffer, pack +
    //    arena, load-time branched-FFN fuse-fold + derived fp32 side tables.
    // ===================================================================
    std::printf("loading DLSSNR graph... (weights + chain setup; ~1-2 s, not a hang)\n");
    auto tWeights0 = clk::now();
    WeightsFile wf;
    stParse("C:\\Users\\AI\\Desktop\\DLSS5_INTEL\\work\\mlxw\\dlssnr-logical.safetensors", wf);

    struct PackEntry { std::string name; uint64_t off, bytes; int blk; };
    std::vector<PackEntry> pack;
    for (auto &kv : wf.tensors) {
        const Tensor &t = kv.second;
        uint64_t esz = (t.dtype == "F32") ? 4 : 2;
        pack.push_back({kv.first, 0, t.numel() * esz, std::atoi(kv.first.substr(5, kv.first.find('.') - 5).c_str())});
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
                  poff["block0.layer0.input_adapter_weight"] == 8960ull;
    if (!packOk) { std::fprintf(stderr, "pack offset mismatch vs pack-layout.txt\n"); return 1; }
    std::printf("pack offsets cross-checked vs docs/pack-layout.txt: OK\n");

    auto tname = [](const char *fmt, int i) -> std::string {
        char b[160]; std::snprintf(b, sizeof b, fmt, i); return b;
    };

    // ---- arena layout (verbatim m8a + 2 m8b bridge slots) ----------------
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
    VkDeviceSize oWIN = slot(512 * 512 * 2);
    VkDeviceSize oPROJW = slot(512 * 1536 * 4);
    VkDeviceSize oQW = slot(128 * 64 * 32 * 2), oKW = slot(128 * 64 * 32 * 2), oVW = slot(128 * 64 * 32 * 2);
    VkDeviceSize oSCW = slot(128 * 64 * 64 * 4), oPRW = slot(128 * 64 * 64 * 2);
    VkDeviceSize oMGW = slot(128 * 64 * 32 * 4), oATW = slot(512 * 512 * 2), oABW = slot(512 * 512 * 4);
    VkDeviceSize oHG = slot(TOK * 4096 * 2);
    VkDeviceSize oPROJG = slot(TOK * 3072 * 4);
    VkDeviceSize oQG = slot(TOK * 1024 * 2), oKG = slot(TOK * 1024 * 2), oVG = slot(TOK * 1024 * 2);
    VkDeviceSize oSCG = slot((VkDeviceSize)32 * TOK * TOK * 4), oPRG = slot((VkDeviceSize)32 * TOK * TOK * 2);
    VkDeviceSize oMGG = slot(TOK * 1024 * 4), oATG = slot(TOK * 1024 * 2);
    VkDeviceSize oABG = slot(TOK * 1024 * 4), oBRG = slot(TOK * 1024 * 4);
    VkDeviceSize oBOG = slot(TOK * 1024 * 4), oB16G = slot(TOK * 1024 * 2);
    VkDeviceSize oB0RAW = slot(TOK * 32 * 4), oFRS = slot(TOK * 32 * 2);
    VkDeviceSize oSKIP0 = slot(TOK * 32 * 2), oSKIP1 = slot(TOK * 64 * 2);
    VkDeviceSize oSKIP2 = slot(TOK * 128 * 2), oSKIP3 = slot(TOK * 256 * 2);
    VkDeviceSize oSS = slot(TOK * 512 * 2);
    VkDeviceSize oB4DS = slot(TOK * 64 * 2), oB22DS = slot(TOK * 512 * 2);
    VkDeviceSize oB38 = slot(TOK * 1024 * 2), oB39 = slot(TOK * 512 * 2), oB48 = slot(TOK * 256 * 2);
    VkDeviceSize oB69 = slot(TOK * 32 * 2);
    VkDeviceSize oMRG70 = slot(TOK * 32 * 4), oHEAD = slot(TOK * 16 * 4);
    // m8b bridge slots: features output (fp32 [288,16]) + packed head ([288,4])
    VkDeviceSize oFeatV = slot(TOK * 16 * 4);
    VkDeviceSize oHead4 = slot(TOK * 4 * 4);
    VkDeviceSize oHeadDC = slot(16);   // headpack per-channel DC accumulator

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

    ChainBuf dev;
    chainAllocDev(c, totalBytes, dev);

    // ---- staging + preprocessing ----------------------------------------
    ChainBuf cbuf;
    chainAllocHost(c, packTotal + ar.off, cbuf);
    char *sp = (char *)cbuf.mapped;

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
                            for (uint64_t c2 = 0; c2 < 32; ++c2)
                                d[((oh * G + ih) * 32 + r) * 128 + br * 32 + c2] =
                                    s[((oh * 4 + br) * G + ih) * 1024 + r * 32 + c2];
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
            for (int cc = 0; cc < 4; ++cc) {
                d[r * 16 + cc] = sg[r * 4 + cc];
                d[(16 + r) * 16 + cc] = sc[r * 4 + cc];
            }
    }
    {
        VkCommandBuffer up = beginOneShot();
        VkBufferCopy cp1{0, 0, packTotal + ar.off};
        vkCmdCopyBuffer(up, cbuf.buf, dev.buf, 1, &cp1);
        double upMs = submitOneShot(up);
        std::printf("weight upload: %.1f MB in %.1f ms (%.2f GB/s incl. preprocessing)\n",
                    (packTotal + ar.off) / 1048576.0, upMs,
                    (packTotal + ar.off) / 1073741824.0 / (upMs / 1e3));
    }
    std::printf("[chain] weights resident, setup %.0f ms\n",
                std::chrono::duration<double, std::milli>(clk::now() - tWeights0).count());

    // ---- chain pipelines --------------------------------------------------
    std::string exeDirStr;
    {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        exeDirStr = buf;
        exeDirStr.resize(exeDirStr.find_last_of('\\') + 1);
    }
    ChainPipe pGemm, pGemm1, pCos, pCosW, pSmax, pEw, pPart, pTrans, pGather, pMerge, pFeatpack, pHeadpack;
    chainMakePipe(c, exeDirStr + "gemm.spv", pGemm);
    chainMakePipe(c, exeDirStr + "gemm_rn1.spv", pGemm1);
    chainMakePipe(c, exeDirStr + "cosine.spv", pCos);
    chainMakePipe(c, exeDirStr + "cosine_win.spv", pCosW);
    chainMakePipe(c, exeDirStr + "softmax.spv", pSmax);
    chainMakePipe(c, exeDirStr + "elementwise.spv", pEw);
    chainMakePipe(c, exeDirStr + "partition.spv", pPart);
    chainMakePipe(c, exeDirStr + "transpose_we.spv", pTrans);
    chainMakePipe(c, exeDirStr + "gather_residual.spv", pGather);
    chainMakePipe(c, exeDirStr + "merge.spv", pMerge);
    chainMakePipe(c, exeDirStr + "featpack.spv", pFeatpack);
    chainMakePipe(c, exeDirStr + "headpack.spv", pHeadpack);
    VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, dev.buf};
    uint64_t A0 = vkGetBufferDeviceAddress(c.dev, &bdai);
    auto A = [&](VkDeviceSize o) { return A0 + (uint64_t)o; };
    std::printf("[chain] 12 pipelines up (10 kernels + featpack/headpack), BDA base 0x%llx\n",
                (unsigned long long)A0);

    // ===================================================================
    // 9. Wave-1 buffers + frame-0 decode + letterbox region scan (verbatim M3/M4).
    // ===================================================================
    VkDeviceMemory memRgb = VK_NULL_HANDLE, memMax = VK_NULL_HANDLE;
    VkBuffer bufRgb = CreateBuf(c, (uint64_t)W * H * 3 * 4,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memRgb);
    VkBuffer bufMax = CreateBuf(c, 4096 * 4,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                &memMax);
    void* maxPtr = nullptr;
    VK_CHECK(vkMapMemory(c.dev, memMax, 0, 4096 * 4, 0, &maxPtr));

    // ---- M8B feedback-cancellation state (video mode only; DEVICE-LOCAL:
    // host-visible STORAGE buffers fault the Arc 32.0.101.8805 driver —
    // measured 2026-09-20 as DEVICE_LOST within two frames):
    //   bufLastPresented: exact copy of the last frame we presented (BGRA u8,
    //     imgFinal -> buffer copy after every processed frame). Zeroed at
    //     startup so the first frame's corrected delta == the raw capture.
    //   bufFbStats: 4-byte mapped GPU counter; fbcancel adds sum(|delta|) per
    //     frame -> full-frame mean |corrected delta| in the summary/logs.
    VkDeviceMemory memLastP = VK_NULL_HANDLE, memFbStats = VK_NULL_HANDLE;
    VkBuffer bufLastPresented = CreateBuf(c, (uint64_t)W * H * 4,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memLastP);
    VkBuffer bufFbStats = CreateBuf(c, 8,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                &memFbStats);
    uint32_t* fbStatsPtr = nullptr;
    VK_CHECK(vkMapMemory(c.dev, memFbStats, 0, 8, 0, (void**)&fbStatsPtr));
    {
        VkCommandBuffer one = beginOneShot();
        vkCmdFillBuffer(one, bufLastPresented, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(one, bufFbStats, 0, VK_WHOLE_SIZE, 0);
        submitOneShot(one);
        std::printf("[fb] cancellation state up: lastPresented %llu B (zeroed, device-local), stats mapped\n",
                    (unsigned long long)((uint64_t)W * H * 4));
    }

    VkDescriptorPool dpool;
    VkDescriptorSet setFinal = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> setBlit(swapImages.size(), VK_NULL_HANDLE);
    {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 16;
        dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(c.dev, &dpci, nullptr, &dpool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        std::vector<VkDescriptorSetLayout> ls(1 + setBlit.size(), dsLayout);
        ai.descriptorPool = dpool; ai.descriptorSetCount = (uint32_t)ls.size();
        ai.pSetLayouts = ls.data();
        std::vector<VkDescriptorSet> allSets(ls.size(), VK_NULL_HANDLE);
        VK_CHECK(vkAllocateDescriptorSets(c.dev, &ai, allSets.data()));
        setFinal = allSets[0];
        for (size_t i = 0; i < setBlit.size(); ++i) setBlit[i] = allSets[1 + i];
    }
    auto writeBuf = [&](VkDescriptorSet s, uint32_t bind, VkBuffer buf, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, 0, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeBufRange = [&](VkDescriptorSet s, uint32_t bind, VkBuffer buf, VkDeviceSize off, VkDeviceSize sz) {
        VkDescriptorBufferInfo bi{buf, off, sz};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    auto writeImg = [&](VkDescriptorSet s, uint32_t bind, VkImageView v) {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = bind; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
    };
    writeImg(setFinal, 0, imgInView);
    writeBuf(setFinal, 1, bufRgb, (uint64_t)W * H * 3 * 4);
    writeBuf(setFinal, 8, bufMax, 4096 * 4);

    // ---- record-once upload + decode + letterbox for the initial frame
    Region region{};
    {
        begin();
        {
            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = bufUpload;
            bb.size = VK_WHOLE_SIZE;
            bb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 1, &bb, 0, nullptr);
            VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.image = imgIn;
            imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.srcAccessMask = 0; imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {W, H, 1};
            vkCmdCopyBufferToImage(cmd, bufUpload, imgIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        vkCmdDispatch(cmd, W / 16, H / 16, 1);
        barrierAll();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLetter);
        Push p{};
        p.a[0] = (int32_t)W; p.a[1] = (int32_t)H;
        p.b[0] = 0; push(p);
        vkCmdDispatch(cmd, (H + 255) / 256, 1, 1);
        barrierAll();
        p.b[0] = 1; push(p);
        vkCmdDispatch(cmd, (W + 255) / 256, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        double ms = submitAndWait();
        std::printf("[stage] frame0 upload+decode+letterbox: %.3f ms\n", ms);
        const float* m = (const float*)maxPtr;
        region = ActiveRegion(m, m + 2048, (int)H, (int)W);
        std::printf("[letterbox] region rows [%d,%d) cols [%d,%d) of %ux%u (host decision FIXED for the loop)\n",
                    region.top, region.bottom, region.left, region.right, W, H);
    }

    const int regionW = region.right - region.left;
    const int regionH = region.bottom - region.top;
    std::printf("[geometry] region %dx%d -> network extent %dx%d (chain 288-token grid)\n",
                regionW, regionH, netW, netH);

    // ===================================================================
    // 10. Wave-2 buffers + output image + full descriptor writes (M4;
    //     bufFeat/bufHead are now RANGES of the chain device buffer:
    //     binding 3 = oFeatV [288,16] fp32, binding 4 = oHead4 [288,4] fp32).
    // ===================================================================
    VkDeviceMemory memT = VK_NULL_HANDLE, memHeadUp = VK_NULL_HANDLE,
                   memFin = VK_NULL_HANDLE, memNr = VK_NULL_HANDLE,
                   memRbFinal = VK_NULL_HANDLE, memRbNr = VK_NULL_HANDLE,
                   memImgFinal = VK_NULL_HANDLE, memImgNr = VK_NULL_HANDLE;
    const uint64_t Tfloats = std::max<uint64_t>((uint64_t)netH * regionW * 3,
                                                (uint64_t)regionH * netW * 4);
    VkBuffer bufT = CreateBuf(c, Tfloats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memT);
    VkBuffer bufHeadUp = CreateBuf(c, (uint64_t)regionW * regionH * 4 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHeadUp);
    VkBuffer bufFin = CreateBuf(c, (uint64_t)regionW * regionH * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFin);
    VkBuffer bufNr = CreateBuf(c, (uint64_t)regionW * regionH * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memNr);
    VkBuffer bufRbFinal = CreateBuf(c, (uint64_t)W * H * 4,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memRbFinal);
    VkBuffer bufRbNr = CreateBuf(c, (uint64_t)regionW * regionH * 4,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memRbNr);
    // one-shot debug staging (frame-1 chain-output statistics)
    const uint64_t dbgSzFeat = (TOK * 16 * 4 + 255) & ~255ull;
    const uint64_t dbgSzHead = (TOK * 16 * 4 + 255) & ~255ull;
    const uint64_t dbgSzH4 = (TOK * 4 * 4 + 255) & ~255ull;
    const uint64_t dbgSzUp = ((uint64_t)regionW * regionH * 4 * 4 + 255) & ~255ull;
    const uint64_t dbgSzX16 = (TOK * 16 * 2 + 255) & ~255ull;   // f16 chain input
    const uint64_t dbgSzADA = (TOK * 32 * 4 + 255) & ~255ull;   // block0 output (fp32)
    const uint64_t dbgSzMRG = (TOK * 32 * 4 + 255) & ~255ull;   // pre-block70 merge (fp32)
    const uint64_t dbgSzG2 = (TOK * 32 * 4 + 255) & ~255ull;    // head-GEMM input (fp32 32ch)
    const uint64_t dbgSzHW = (1024 + 255) & ~255ull;            // folded head weight (f16)
    const uint64_t dbgSzB4 = (TOK * 64 * 2 + 255) & ~255ull;    // block4-downsample out (f16)
    const uint64_t dbgSzB22 = (TOK * 512 * 2 + 255) & ~255ull;  // block22-downsample out (f16)
    const uint64_t dbgSzB38 = (TOK * 1024 * 2 + 255) & ~255ull; // block38 out (f16)
    const uint64_t dbgSzB48 = (TOK * 256 * 2 + 255) & ~255ull;  // block48 out (f16)
    const uint64_t dbgSzB69 = (TOK * 32 * 2 + 255) & ~255ull;   // block69 out (f16)
    const uint64_t dbgSzSin = (32 * 4 + 255) & ~255ull;         // inp_merge_sin (fp32)
    const uint64_t dbgSzCos = (32 * 4 + 255) & ~255ull;         // inp_merge_cos (fp32)
    const uint64_t dbgSzFRS = (TOK * 32 * 2 + 255) & ~255ull;   // full-res skip (f16)
    const uint64_t dbgOffFeat = 0, dbgOffHead = dbgSzFeat,
                   dbgOffH4 = dbgSzFeat + dbgSzHead,
                   dbgOffUp = dbgSzFeat + dbgSzHead + dbgSzH4,
                   dbgOffX16 = dbgOffUp + dbgSzUp,
                   dbgOffADA = dbgOffX16 + dbgSzX16,
                   dbgOffMRG = dbgOffADA + dbgSzADA,
                   dbgOffG2 = dbgOffMRG + dbgSzMRG,
                   dbgOffHW = dbgOffG2 + dbgSzG2,
                   dbgOffB4 = dbgOffHW + dbgSzHW,
                   dbgOffB22 = dbgOffB4 + dbgSzB4,
                   dbgOffB38 = dbgOffB22 + dbgSzB22,
                   dbgOffB48 = dbgOffB38 + dbgSzB38,
                   dbgOffB69 = dbgOffB48 + dbgSzB48,
                   dbgOffSin = dbgOffB69 + dbgSzB69,
                   dbgOffCos = dbgOffSin + dbgSzSin,
                   dbgOffFRS = dbgOffCos + dbgSzCos;
    VkDeviceMemory memDbg = VK_NULL_HANDLE;
    VkBuffer bufDbg = CreateBuf(c, dbgOffFRS + dbgSzFRS,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memDbg);

    VkImage imgFinal = CreateImage2D(c, W, H, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgFinal);
    VkImage imgNr = CreateImage2D(c, regionW, regionH, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgNr);
    VkImageView viewFinal = CreateView(c, imgFinal, VK_FORMAT_B8G8R8A8_UNORM);
    VkImageView viewNr = CreateView(c, imgNr, VK_FORMAT_B8G8R8A8_UNORM);
    {
        writeBuf(setFinal, 2, bufT, Tfloats * 4);
        writeBufRange(setFinal, 3, dev.buf, oFeatV, (uint64_t)TOK * 16 * 4);
        writeBufRange(setFinal, 4, dev.buf, oHead4, (uint64_t)TOK * 4 * 4);
        writeBuf(setFinal, 5, bufHeadUp, (uint64_t)regionW * regionH * 4 * 4);
        writeBuf(setFinal, 6, bufFin, (uint64_t)regionW * regionH * 3 * 4);
        writeBuf(setFinal, 7, bufNr, (uint64_t)regionW * regionH * 3 * 4);
        writeBuf(setFinal, 10, bufLastPresented, (uint64_t)W * H * 4);  // feedback cancellation
        writeBuf(setFinal, 11, bufFbStats, 8);                          // settle stats
    }
    writeImg(setFinal, 9, viewFinal);
    for (size_t i = 0; i < setBlit.size(); ++i) {
        writeImg(setBlit[i], 0, viewFinal);
        writeBuf(setBlit[i], 1, bufRgb, (uint64_t)W * H * 3 * 4);
        writeBuf(setBlit[i], 8, bufMax, 4096 * 4);
        writeImg(setBlit[i], 9, swapViews[i]);
    }
    std::printf("[mem] intermediates device-local; readback host-coherent; blit sets %zu\n", setBlit.size());

    // ===================================================================
    // 11. Chain dispatch machinery (verbatim m8a — the validated full chain).
    // ===================================================================
    auto bar = [&](VkCommandBuffer cb) { chainFullBarrier(cb, dev.buf); };
    auto gflags = [&](uint32_t epi, bool narrow) { return (epi << 8) | (narrow ? F_NARROW : 0u); };
    auto dGemm = [&](VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
                     uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                     uint32_t sa, uint32_t sb, uint32_t sc,
                     uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
        (void)fam;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
        GemmPush p{a, b, c2, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc};
        vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, n / 32, m / 16, batch);
    };
    auto dEw = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                   uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
        EWPush p{a, b, c2, d, h, n, kind, ch};
        vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };
    auto dPart = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t C,
                     uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
        uint32_t hp8 = (IMG_H + padTop + 7) / 8;
        PartPush p{a, c2, IMG_H, IMG_W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
        vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
    };
    auto dGather = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c2,
                       uint64_t h, uint32_t C, uint32_t padTop, uint32_t padLeft, uint32_t wp8,
                       bool pub, bool b16) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGather.pipe);
        GatherPush p{a, b, d, c2, h, IMG_H, IMG_W, C, padTop, padLeft, wp8,
                     (pub ? 1u : 0u) | (b16 ? 2u : 0u)};
        vkCmdPushConstants(cb, pGather.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, TOK, 1, 1);
    };
    auto dTrans = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pTrans.pipe);
        TransWePush p{a, c2, Ww, H};
        vkCmdPushConstants(cb, pTrans.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, Ww * 64, 1, 1);
    };
    auto dCosW = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc,
                     uint32_t Ww, uint32_t H, uint32_t C, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCosW.pipe);
        CosWinPush p{a, c2, sc, Ww * 64 * H, kind, C, H};
        vkCmdPushConstants(cb, pCosW.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (Ww * 64 * H + 31) / 32, 1, 1);
    };
    auto dCosG = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
        CosPush p{a, c2, sc, TOK * 32, kind};
        vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (TOK * 32 + 31) / 32, 1, 1);
    };
    auto dSmaxW = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H,
                      uint64_t bias) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c2, Ww * H * 64, 64, 0.0f, bias, H};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (Ww * H * 64 + 31) / 32, 1, 1);
    };
    auto dSmaxG = [&](VkCommandBuffer cb, uint64_t a, uint64_t c2) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c2, 32 * TOK, TOK, 3.0f, 0, 0};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, 32 * TOK, 1, 1);
    };
    auto dMerge = [&](VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                      uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pMerge.pipe);
        MergePush p{a, b, c2, d, e, h, n, ch, kind};
        vkCmdPushConstants(cb, pMerge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    };

    auto winGeo = [](int oy, int ox, uint32_t &padTop, uint32_t &padLeft, uint32_t &Ww) {
        padTop = (uint32_t)(-oy);
        padLeft = (uint32_t)(-ox);
        uint32_t hp8 = (IMG_H + padTop + 7) / 8;
        uint32_t wp8 = (IMG_W + padLeft + 7) / 8;
        Ww = hp8 * wp8;
    };

    struct Cur { VkDeviceSize off; bool is16; uint32_t C; };

    auto recordWindowAttn = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int H, int fam,
                                int oy, int ox, VkDeviceSize ffnOff, bool ffn16,
                                VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish) {
        uint32_t padTop, padLeft, Ww;
        winGeo(oy, ox, padTop, padLeft, Ww);
        uint32_t wp8 = (IMG_W + padLeft + 7) / 8;
        auto lay = [&](const char *fmt) { return tname(fmt, idx); };
        std::string qkvN = lay(fam == 2 ? "block%d.layer2.qkv_weight" : "block%d.layer0.qkv_weight");
        std::string scN = lay(fam == 2 ? "block%d.layer2.attn_scale" : "block%d.layer0.attn_scale");
        std::string biasN = lay(fam == 2 ? "block%d.layer2.attn_bias" : "block%d.layer0.attn_bias");
        std::string projN = lay(fam == 2 ? "block%d.layer3.projection_weight" : "block%d.layer0.projection_weight");
        std::string acosN = lay(fam == 2 ? "block%d.layer3.attn_cos_skip" : "block%d.layer0.attn_cos_skip");

        dPart(cb, A(ffnOff), A(oWIN), (uint32_t)C, padTop, padLeft, wp8, ffn16);
        bar(cb);
        dGemm(cb, famIdx, A(oWIN), A(poff[qkvN]), A(oPROJW), 64, 3 * C, C, Ww,
              64 * C, 0, 64 * 3 * C, gflags(EPI_NONE, false), C, 3 * C, 3 * C);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oQW), A(poff[scN]), Ww, H, C, 0);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oKW), A(poff[scN]), Ww, H, C, 1);
        bar(cb);
        dCosW(cb, A(oPROJW), A(oVW), A(poff[scN]), Ww, H, C, 2);
        bar(cb);
        dGemm(cb, famIdx, A(oQW), A(oKW), A(oSCW), 64, 64, 32, Ww * H,
              64 * 32, 64 * 32, 64 * 64, gflags(EPI_NONE, false) | F_TRANSPOSE, 32, 32, 64);
        bar(cb);
        dSmaxW(cb, A(oSCW), A(oPRW), Ww, H, A(poff[biasN]));
        bar(cb);
        dGemm(cb, famIdx, A(oPRW), A(oVW), A(oMGW), 64, 32, 64, Ww * H,
              64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
        bar(cb);
        dTrans(cb, A(oMGW), A(oATW), Ww, H);
        bar(cb);
        dGemm(cb, famIdx, A(oATW), A(poff[projN]), A(oABW), 64, C, C, Ww,
              64 * C, 0, 64 * C, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dGather(cb, A(oABW), A(ffnOff), A(vecOff[acosN]), A(rawOff), A(publish ? pubOff : rawOff),
                (uint32_t)C, padTop, padLeft, wp8, publish, ffn16);
        bar(cb);
    };

    auto recordWindowBlock = [&](VkCommandBuffer &cb, int famIdx, int idx, int C, int H, int fam,
                                 int oy, int ox, bool publish, Cur &cur,
                                 VkDeviceSize rawOff, VkDeviceSize pubOff) {
        uint32_t G = C / 32;
        VkDeviceSize x16 = cur.is16 ? cur.off : oG2;
        if (!cur.is16) { dEw(cb, A(cur.off), 0, 0, 0, A(oG2), TOK * C, 2, 0); bar(cb); }
        if (fam == 0) {
            dGemm(cb, famIdx, A(x16), A(poff[tname("block%d.layer0.weight1", idx)]), A(oG3),
                  TOK, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
            bar(cb);
            dGemm(cb, famIdx, A(oG3), A(poff[tname("block%d.layer0.weight2", idx)]), A(oG4),
                  TOK, C, 4 * C, 1, 0, 0, 0, gflags(EPI_NONE, false), 4 * C, C, C);
            bar(cb);
            if (cur.is16)
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOK * C, 4, C);
            else
                dEw(cb, A(oG4), A(cur.off), A(oG4), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]), 0,
                    TOK * C, 1, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, C, H, fam, oy, ox, oG4, false, rawOff, pubOff, publish);
        } else if (fam == 1) {
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
            dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer0.ffn_cos_skip", idx)]),
                A(oG2), TOK * C, 5, C);
            bar(cb);
            recordWindowAttn(cb, famIdx, idx, C, H, fam, oy, ox, oG2, true, rawOff, pubOff, publish);
        } else {
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
            dEw(cb, A(oG4), 0, 0, 0, A(oG2), TOK * C, 3, 0);
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

    auto recordDs = [&](VkCommandBuffer cb, int famIdx, int idx, int C, int Cn,
                        VkDeviceSize rawOff, VkDeviceSize outOff) {
        dEw(cb, A(rawOff), 0, 0, 0, A(oG2), TOK * C, 3, 0);
        bar(cb);
        dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer0.weight0", idx)]), A(outOff),
              TOK, Cn, C, 1, 0, 0, 0, gflags(EPI_E4M3, true), C, Cn, Cn);
        bar(cb);
    };

    auto recordUp = [&](VkCommandBuffer cb, int famIdx, int idx, int Ci, int Co,
                        Cur &cur, VkDeviceSize skipOff, VkDeviceSize out16Off) {
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight0", idx)]), A(oG3),
              TOK, Co, Ci, 1, 0, 0, 0, gflags(EPI_NONE, false), Ci, Co, Co);
        bar(cb);
        dMerge(cb, A(oG3), A(skipOff), A(oG4), A(vecOff[tname("block%d.layer0.sin", idx)]), 0,
               A(out16Off), TOK * Co, Co, 0);
        bar(cb);
    };

    auto recordGlobal = [&](VkCommandBuffer cb, int famIdx, int idx, Cur &cur,
                            VkDeviceSize rawOff, VkDeviceSize pubOff) {
        dGemm(cb, famIdx, A(cur.off), A(poff[tname("block%d.layer0.weight", idx)]), A(oHG),
              TOK, 4096, 1024, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), 1024, 4096, 4096);
        bar(cb);
        dGemm(cb, famIdx, A(oHG), A(poff[tname("block%d.layer1.weight", idx)]), A(oG3),
              TOK, 1024, 4096, 1, 0, 0, 0, gflags(EPI_NONE, false), 4096, 1024, 1024);
        bar(cb);
        dEw(cb, A(oG3), A(cur.off), A(oG3), A(vecOff[tname("block%d.layer1.ffn_cos_skip", idx)]),
            0, TOK * 1024, 4, 1024);
        bar(cb);
        dEw(cb, A(oG3), 0, 0, 0, A(oG2), TOK * 1024, 2, 0);
        bar(cb);
        dGemm(cb, famIdx, A(oG2), A(poff[tname("block%d.layer2.qkv_weight", idx)]), A(oPROJG),
              TOK, 3072, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 3072, 3072);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oQG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 0);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oKG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 1);
        bar(cb);
        dCosG(cb, A(oPROJG), A(oVG), A(vecOff[tname("block%d.layer2.attn_scale.q32", idx)]), 2);
        bar(cb);
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
        dGemm(cb, famIdx, A(oATG), A(poff[tname("block%d.layer4.projection_weight", idx)]), A(oABG),
              TOK, 1024, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 1024, 1024);
        bar(cb);
        dEw(cb, A(oABG), A(oG3), A(oBRG), A(vecOff[tname("block%d.layer4.attn_cos_skip", idx)]),
            A(pubOff), TOK * 1024, 0, 1024);
        bar(cb);
    };

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

    // ---- the 71-block chain (verbatim m8a walk, probes/timing stripped) ----
    auto recordChain = [&](VkCommandBuffer cb) {
        Cur cur{oADA, false, 32};
        dGemm(cb, 0, A(oX16), A(poff["block0.layer0.input_adapter_weight"]), A(oADA),
              TOK, 32, 16, 1, 0, 0, 0, gflags(EPI_NONE, false), 16, 32, 32);
        bar(cb);
        {
            Cur c0 = cur;
            recordWindowBlock(cb, 0, 0, 32, 1, 0, 0, 0, false, c0, oB0RAW, 0);
            cur = {oB0RAW, false, 32};
            dEw(cb, A(oB0RAW), 0, 0, 0, A(oFRS), TOK * 32, 3, 0);
            bar(cb);
        }
        for (int i = 1; i <= 3; ++i) {
            VkDeviceSize out = (i == 3) ? oSKIP0 : oG1;
            recordWindowBlock(cb, 0, i, 32, 1, 0, originOf(i).first, originOf(i).second, true,
                              cur, oRAW, out);
            cur = {out, true, 32};
        }
        {
            Cur c4 = cur;
            recordWindowBlock(cb, 0, 4, 32, 1, 0, -4, 0, false, c4, oRAW, 0);
            recordDs(cb, 0, 4, 32, 64, oRAW, oB4DS);
            cur = {oB4DS, true, 64};
        }

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

        for (int i = 23; i <= 30; ++i) {
            auto o = originOf(i);
            VkDeviceSize out = (i == 30) ? oSS : oG1;
            recordWindowBlock(cb, 2, i, 512, 16, 2, o.first, o.second, true, cur, oRAW, out);
            cur = {out, true, 512};
        }
        {
            dGemm(cb, 2, A(oSS), A(poff["block30.layer4.weight"]), A(oG1),
                  TOK, 1024, 512, 1, 0, 0, 0, gflags(EPI_E4M3, true), 512, 1024, 1024);
            bar(cb);
            cur = {oG1, true, 1024};
        }

        for (int i = 31; i <= 38; ++i) {
            VkDeviceSize out = (i == 38) ? oB38 : oG1;
            recordGlobal(cb, 3, i, cur, oBRG, out);
            cur = {out, true, 1024};
        }

        {
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
        {
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
        {
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
        {
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
        {
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

        {
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
        }
    };

    // ===================================================================
    // 12. LIVE LOOP (m4 verbatim, standin block -> real chain).
    // ===================================================================
    const uint32_t regionOff = (uint32_t)(region.top * (int)W + region.left);
    const long verifyFrames[3] = {10, 30, 60};
    int verifyIdx = 0;
    long frame = 0, dropped = 0, stale = 0;
    long settled = 0;                 // feedback settle skips (video mode)
    long fbFrames = 0;                // processed frames with fb stats
    double fbMeanLast = 0.0;          // last full-frame mean |corrected delta| (0-255)
    bool shownOnce = false;           // overlay ShowWindow deferred to first present
    double fpsFinal = 0.0;
    bool passMetrics = true;
    bool anyVerify = false;
    bool savedPair = false;
    std::deque<double> frameTimes;
    std::vector<uint8_t> nativeRef;
    std::printf("[m8b] entering live loop (%ld frames, event-driven; --frames counts PROCESSED frames)\n",
                framesTarget);
    const auto tLoopStart = clk::now();
    auto tLastAcquired = tLoopStart;   // wall-clock of last acquired frame (idle reference)
    double lastIdleLogSec = 0.0;       // idle heartbeat logger
    bool wiggleActive = false;    // wiggle-idle generator currently running
    uint8_t prevSamples[512]{};   // last frame's sparse samples (wiggle discriminator)
    uint64_t prevSampleCnt = 0;

    while (!g_stop.load() && frame < framesTarget) {
        const auto tLoopTop = clk::now();
        // ---- (a) EVENT-DRIVEN DDA capture: block up to 1000 ms for a real
        // desktop update; process+present ONLY when one arrives. On idle the
        // loop presents nothing and does no GPU work (~0% GPU on a static
        // screen). Optional --wiggle-idle N re-arms the gentle cursor
        // generator after N seconds of no updates (default OFF).
        bool fresh = false;
        double estMeanDelta = 0.0;   // sparse settle estimate (video mode)
        {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(1000, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                ++dropped;
                const double idleSec =
                    std::chrono::duration<double>(clk::now() - tLastAcquired).count();
                if (idleSec - lastIdleLogSec >= 5.0) {
                    lastIdleLogSec = idleSec;
                    std::printf("[m8b] idle, waiting for updates (%.0f s; %ld processed; %.2f fps)\n",
                                idleSec, frame, fpsFinal);
                }
                // --wiggle-idle N: after N wall-clock seconds without ANY
                // acquired frame, engage the gentle generator (default OFF).
                if (wiggleIdleSec > 0 && !wiggleActive && idleSec >= (double)wiggleIdleSec) {
                    g_wiggleStop.store(false);
                    wiggler = std::thread(WiggleThread);
                    wiggleActive = true;
                    std::printf("[info] wiggle-idle: no updates for %.0f s — engaging gentle generator\n",
                                idleSec);
                }
                continue;
            } else if (FAILED(hr)) {
                tLastAcquired = clk::now();
                if (hr == DXGI_ERROR_ACCESS_LOST) {
                    std::fprintf(stderr, "[warn] ACCESS_LOST - attempting one DuplicateOutput recovery\n");
                    dup.Reset();
                    ComPtr<IDXGIOutput1> out1;
                    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out1))) &&
                        SUCCEEDED(out1->DuplicateOutput(device.Get(), &dup))) {
                        ++dropped;
                    } else {
                        std::fprintf(stderr, "[FAIL] duplication lost for good\n");
                        g_stop.store(true);
                    }
                } else {
                    std::fprintf(stderr, "[error] AcquireNextFrame: %s\n", HrName(hr));
                    ++dropped;
                }
                continue;
            } else {
                ComPtr<ID3D11Texture2D> frameTex;
                hr = res->QueryInterface(IID_PPV_ARGS(&frameTex));
                if (FAILED(hr)) { dup->ReleaseFrame(); ++dropped; continue; }
                context->CopyResource(stagingTex.Get(), frameTex.Get());
                context->Flush();
                dup->ReleaseFrame();
                bool black = true;
                D3D11_MAPPED_SUBRESOURCE ms{};
                if (SUCCEEDED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                    double s = 0; uint64_t cnt = 0;
                    // sparse samples ALSO feed the wiggle discriminator: the
                    // generator only moves the cursor (~few samples), real
                    // content changes many — disengage wiggle on real change.
                    // The same samples feed the feedback SETTLE estimate
                    // (mean |capture - previous capture|): when the visible
                    // screen stops changing, the corrected delta would be ~0,
                    // so the frame is skipped before any GPU work.
                    uint8_t cur[512]; const uint32_t curCap = 512;
                    for (uint32_t y = 0; y < H; y += 24) {
                        const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
                        for (uint32_t x = 0; x < W * 4; x += 1997) {
                            s += row[x];
                            if (cnt < curCap) cur[cnt] = row[x];
                            ++cnt;
                        }
                    }
                    black = (cnt == 0) || (s / (double)cnt) < 1.0;
                    if (!black) {
                        uint64_t diffCnt = 0;
                        double absSum = 0.0;
                        if (prevSampleCnt && prevSampleCnt == cnt) {
                            for (uint64_t i = 0; i < cnt && i < curCap; ++i) {
                                const unsigned ad = (unsigned)std::abs((int)cur[i] - (int)prevSamples[i]);
                                absSum += (double)ad;
                                if (ad > 16) ++diffCnt;
                            }
                            estMeanDelta = absSum / (double)(cnt < curCap ? cnt : curCap);
                        } else {
                            estMeanDelta = 1e9;   // first frame: always process
                        }
                        if (wiggleActive && prevSampleCnt == cnt && diffCnt > 32) {
                            g_wiggleStop.store(true);
                            wiggler.join();
                            wiggleActive = false;
                            std::printf("[info] real screen activity resumed (%llu changed samples) — wiggle generator off\n",
                                        (unsigned long long)diffCnt);
                        }
                        prevSampleCnt = cnt < curCap ? cnt : curCap;
                        for (uint64_t i = 0; i < prevSampleCnt; ++i) prevSamples[i] = cur[i];
                        bool isVerify = (verifyIdx < 3 && verifyFrames[verifyIdx] == frame);
                        for (uint32_t y = 0; y < H; ++y)
                            std::memcpy(&original[(size_t)y * W * 4],
                                        (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
                        if (isVerify) nativeRef = original;
                    }
                    context->Unmap(stagingTex.Get(), 0);
                }
                if (black) { ++dropped; continue; }
                fresh = true;
                tLastAcquired = clk::now();
            }
            if (fresh) stale = 0;
        }
        if (g_stop.load()) break;
        const double acqMs = std::chrono::duration<double, std::milli>(clk::now() - tLoopTop).count();

        // ---- (b) SETTLE SKIP (video mode): the visible screen (which under
        // the opaque overlay is our own last presented frame) barely changed
        // vs the previous capture — the frame's corrected delta would be ~0,
        // so skip the chain + present entirely. Costs one sparse-sample diff,
        // no GPU submission at all (~0% GPU when settled). The full-frame
        // mean |corrected delta| from the fbcancel GPU counter is still
        // logged for every processed frame (see fbMeanLast).
        if (!novideo && shownOnce && estMeanDelta < settleThresh) {
            ++settled;
            if (settled <= 3 || (settled % 100) == 0)
                std::printf("[m8b] settled (est mean|d|=%.3f < %.3f) - skip chain+present, keep last frame (#%ld)\n",
                            estMeanDelta, settleThresh, settled);
            continue;
        }

        // ---- (b2) CPU bridge: staging -> upload buffer (raw capture, for
        // imgIn: letterbox content scan + encode alpha)
        const auto tBridge0 = clk::now();
        std::memcpy(uploadPtr, original.data(), (size_t)W * H * 4);
        const double bridgeMs =
            std::chrono::duration<double, std::milli>(clk::now() - tBridge0).count();

        // ---- (c) acquire swapchain image (video mode only)
        uint32_t imageIndex = 0;
        if (!novideo) {
            VkResult ar;
            for (;;) {
                ar = vkAcquireNextImageKHR(c.dev, swapchain, UINT64_MAX, semImage, VK_NULL_HANDLE, &imageIndex);
                if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) break;
                if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
                    std::fprintf(stderr, "[warn] swapchain OUT_OF_DATE - ending run\n");
                    g_stop.store(true);
                    break;
                }
                break;
            }
            if (g_stop.load() && ar != VK_SUCCESS) break;
        }

        // ---- (d) record frame: upload + feedback cancellation + M4 front-end
        // + REAL CHAIN + compose + encode. In video mode fbcancel (GPU kernel,
        // fused here — a separate pre-pass submission measured ~168 ms/frame)
        // replaces decode: it writes the corrected delta into the decode
        // output slot; imgIn still gets the raw capture for the letterbox
        // content scan and encode alpha.
        const bool verify = (verifyIdx < 3 && verifyFrames[verifyIdx] == frame);
        const auto tFrameStart = clk::now();
        begin();
        vkCmdResetQueryPool(cmd, tsPool, 0, 6);
        if (!novideo) {
            vkCmdFillBuffer(cmd, bufFbStats, 0, 8, 0);
            VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fb.buffer = bufFbStats;
            fb.size = 8;
            fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb, 0, nullptr);
        }
        {   // upload -> imgIn
            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = bufUpload;
            bb.size = VK_WHOLE_SIZE;
            bb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 1, &bb, 0, nullptr);
            VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.image = imgIn;
            imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.srcAccessMask = 0; imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {W, H, 1};
            vkCmdCopyBufferToImage(cmd, bufUpload, imgIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
        }
        if (!novideo) {   // fbcancel: corrected = clamp(capture - lastPresented, +/-4*maxDelta)
                          // (safety clamp only — real content must pass; the tight
                          // divergence bound is the composite residual clamp in
                          // compose) as float RGB into the decode slot + stats
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFbcancel);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                    &setFinal, 0, nullptr);
            Push p{};
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.b[0] = maxDelta * 4;
            push(p);
            vkCmdDispatch(cmd, W / 16, H / 16, 1);
            barrierAll();
        }
        {   // M4 front-end (verbatim dispatch params; region fixed)
            Push p{};
            if (novideo) {   // decode: BGRA -> float RGB. Video mode: fbcancel
                             // already wrote the corrected delta into bufRgb.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                        &setFinal, 0, nullptr);
                vkCmdDispatch(cmd, W / 16, H / 16, 1);
                barrierAll();
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLetter);
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.b[0] = 0; push(p);
            vkCmdDispatch(cmd, (H + 255) / 256, 1, 1);
            barrierAll();
            p.b[0] = 1; push(p);
            vkCmdDispatch(cmd, (W + 255) / 256, 1, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale);
            p.a[0] = (int32_t)W; p.a[1] = regionOff; p.a[2] = regionH; p.a[3] = regionW;
            p.b[0] = netH; p.b[1] = regionW; p.b[2] = 0; p.b[3] = 3; p.d[0] = 1; p.d[1] = 2; push(p);
            vkCmdDispatch(cmd, (regionW + 15) / 16, (netH + 15) / 16, 1);
            barrierAll();
            p.a[0] = regionW; p.a[1] = 0; p.a[2] = regionW; p.a[3] = netH;
            p.b[0] = netW; p.b[1] = netW; p.b[2] = 1; p.b[3] = 3; p.d[0] = 2; p.d[1] = 4; push(p);
            vkCmdDispatch(cmd, (netW + 15) / 16, (netH + 15) / 16, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFeatures);
            p.a[0] = netW; p.a[1] = netH; p.b[0] = (int32_t)(frame & 0x7fffffff);
            p.c[0] = 0.0f; p.c[1] = 1.0f; p.c[2] = 1.0f; push(p);
            vkCmdDispatch(cmd, ((uint64_t)netW * netH + 255) / 256, 1, 1);
            barrierAll();
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 1);
        }
        {   // ==== REAL DLSS 5 CHAIN (replaces the m4 stand-in block) ====
            // featpack: features fp32 [288,16] -> chain input f16 (oX16)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pFeatpack.pipe);
            PackPush fp{A(oFeatV), A(oX16), TOK * 16};
            vkCmdPushConstants(cmd, pFeatpack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fp), &fp);
            vkCmdDispatch(cmd, (TOK * 16 + 255) / 256, 1, 1);
            barrierAll();
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 2);
            // full 71-block DLSSNR chain (~1402 dispatches)
            recordChain(cmd);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 3);
            // headpack (two passes): per-channel DC of the raw head, then the
            // calibrated matched residual into the m4 head layout [288,4]
            // (binding 4 slot). See shaders/m8/headpack.comp.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pHeadpack.pipe);
            vkCmdFillBuffer(cmd, dev.buf, oHeadDC, 16, 0);
            barrierAll();
            HeadPush hd{A(oHEAD), A(oHead4), A(oHeadDC), TOK, 0.0f, 0};
            vkCmdPushConstants(cmd, pHeadpack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hd), &hd);
            vkCmdDispatch(cmd, (TOK + 255) / 256, 1, 1);
            barrierAll();
            HeadPush hp{A(oHEAD), A(oHead4), A(oHeadDC), TOK, 0.2f, 1};
            vkCmdPushConstants(cmd, pHeadpack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hp), &hp);
            vkCmdDispatch(cmd, (TOK + 255) / 256, 1, 1);
            barrierAll();
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 4);
        }
        {   // rescale up (4ch head -> region) + compose + encode (verbatim M4)
            Push p{};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale);
            p.a[0] = netW; p.a[1] = 0; p.a[2] = netH; p.a[3] = netW;
            p.b[0] = regionH; p.b[1] = netW; p.b[2] = 0; p.b[3] = 4; p.d[0] = 4; p.d[1] = 2; push(p);
            vkCmdDispatch(cmd, (netW + 15) / 16, (regionH + 15) / 16, 1);
            barrierAll();
            p.a[0] = netW; p.a[1] = 0; p.a[2] = netW; p.a[3] = regionH;
            p.b[0] = regionW; p.b[1] = regionW; p.b[2] = 1; p.b[3] = 4; p.d[0] = 2; p.d[1] = 5; push(p);
            vkCmdDispatch(cmd, (regionW + 15) / 16, (regionH + 15) / 16, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeCompose);
            p.a[0] = (int32_t)W; p.a[1] = region.top; p.a[2] = region.left;
            p.b[0] = regionW; p.b[1] = regionH;
            p.c[0] = (float)maxDelta / 255.0f;             // composite delta clamp
            p.c[1] = novideo ? 0.0f : 1.0f;                // accumulate mode (video)
            push(p);
            vkCmdDispatch(cmd, ((uint64_t)regionW * regionH + 255) / 256, 1, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode);
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.a[2] = region.left; p.a[3] = region.top;
            p.b[0] = regionW; p.b[1] = regionH; p.b[2] = 0;
            p.b[3] = novideo ? 0 : 1;                      // accumulate: add delta onto lastPresented
            push(p);
            vkCmdDispatch(cmd, (W + 15) / 16, (H + 15) / 16, 1);
            barrierAll();
            if (!novideo) {   // keep the cancellation buffer == exactly what we present
                VkImageMemoryBarrier lb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                lb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                lb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                lb.image = imgFinal;
                lb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                lb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                lb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                lb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                lb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lb);
                VkBufferImageCopy lr{};
                lr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                lr.imageExtent = {W, H, 1};
                vkCmdCopyImageToBuffer(cmd, imgFinal, VK_IMAGE_LAYOUT_GENERAL, bufLastPresented, 1, &lr);
                VkBufferMemoryBarrier lbb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                lbb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                lbb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                lbb.buffer = bufLastPresented;
                lbb.size = VK_WHOLE_SIZE;
                lbb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                lbb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &lbb, 0, nullptr);
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 5);
        }
        if (frame == 1) {   // one-shot debug: copy chain-path buffers for stats
            VkBufferCopy cp[16]{};
            cp[0] = {oFeatV, dbgOffFeat, TOK * 16 * 4};
            cp[1] = {oHEAD, dbgOffHead, TOK * 16 * 4};
            cp[2] = {oHead4, dbgOffH4, TOK * 4 * 4};
            cp[3] = {oX16, dbgOffX16, TOK * 16 * 2};
            cp[4] = {oADA, dbgOffADA, TOK * 32 * 4};
            cp[5] = {oMRG70, dbgOffMRG, TOK * 32 * 4};
            cp[6] = {oG2, dbgOffG2, TOK * 32 * 4};
            cp[7] = {oHeadW, dbgOffHW, 1024};
            cp[8] = {oB4DS, dbgOffB4, TOK * 64 * 2};
            cp[9] = {oB22DS, dbgOffB22, TOK * 512 * 2};
            cp[10] = {oB38, dbgOffB38, TOK * 1024 * 2};
            cp[11] = {oB48, dbgOffB48, TOK * 256 * 2};
            cp[12] = {oB69, dbgOffB69, TOK * 32 * 2};
            cp[13] = {vecOff["block70.layer0.inp_merge_sin"], dbgOffSin, 32 * 4};
            cp[14] = {vecOff["block70.layer0.inp_merge_cos"], dbgOffCos, 32 * 4};
            cp[15] = {oFRS, dbgOffFRS, TOK * 32 * 2};
            vkCmdCopyBuffer(cmd, dev.buf, bufDbg, 16, cp);
            VkBufferCopy cp2{0, dbgOffUp, (uint64_t)regionW * regionH * 4 * 4};
            vkCmdCopyBuffer(cmd, bufHeadUp, bufDbg, 1, &cp2);
            VkMemoryBarrier db{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; db.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &db, 0, nullptr, 0, nullptr);
        }
        if (verify) {
            VkImageMemoryBarrier vb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vb.image = imgFinal;
            vb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vb.oldLayout = VK_IMAGE_LAYOUT_GENERAL; vb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            vb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; vb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &vb);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, imgFinal, VK_IMAGE_LAYOUT_GENERAL, bufRbFinal, 1, &rgn);
            VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
        }
        if (!novideo) {   // blit composite -> swapchain image
            VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.image = swapImages[imageIndex];
            imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            imb.srcAccessMask = 0;
            imb.dstAccessMask = swapStorage ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 swapStorage ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                             : VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &imb);
            if (swapStorage) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeBlit);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                        &setBlit[imageIndex], 0, nullptr);
                Push p{};
                p.a[0] = (int32_t)W; p.a[1] = (int32_t)H;
                push(p);
                vkCmdDispatch(cmd, W / 16, H / 16, 1);
            } else {
                VkImageCopy cp{};
                cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                cp.extent = {W, H, 1};
                vkCmdCopyImage(cmd, imgFinal, VK_IMAGE_LAYOUT_GENERAL,
                               swapImages[imageIndex], VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            }
            VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            pb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pb.image = swapImages[imageIndex];
            pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            pb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pb.srcAccessMask = swapStorage ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
            pb.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd,
                                 swapStorage ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                             : VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &pb);
        }
        VK_CHECK(vkEndCommandBuffer(cmd));
        const double recMs =
            std::chrono::duration<double, std::milli>(clk::now() - tFrameStart).count();
        double gpuMs = 0;

        // ---- (e) submit (wait acquire, signal render) + present + FULL wait.
        {
            const auto tSubmit0 = clk::now();
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            if (!novideo) {
                si.waitSemaphoreCount = 1; si.pWaitSemaphores = &semImage; si.pWaitDstStageMask = &st;
                si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semRender;
            }
            VK_CHECK(vkQueueSubmit(c.queue, 1, &si, fence));
            if (!novideo) {
                if (!shownOnce) {
                    // deferred show: the very first presented image is already
                    // on the swapchain, so the overlay appears with content —
                    // never a black rectangle over the captured desktop.
                    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
                    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    shownOnce = true;
                    std::printf("[win] overlay shown (first present)\n");
                }
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semRender;
                pi.swapchainCount = 1; pi.pSwapchains = &swapchain;
                pi.pImageIndices = &imageIndex;
                vkQueuePresentKHR(c.queue, &pi);
            }
            VK_CHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkResetFences(c.dev, 1, &fence);
            gpuMs = std::chrono::duration<double, std::milli>(clk::now() - tSubmit0).count();
        }
        if (!novideo) {   // full-frame mean |corrected delta| from the fbcancel counter
            const uint32_t sumAbs = *fbStatsPtr;   // host-coherent, fence-passed
            fbMeanLast = (double)sumAbs / ((double)W * H * 3.0);
            ++fbFrames;
        }
        {   // per-stage GPU timestamp readback + timing line
            uint64_t ts[6]{};
            VkResult qr = vkGetQueryPoolResults(c.dev, tsPool, 0, 6, sizeof(ts), ts,
                                                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            double feMs = 0, fpMs = 0, chMs = 0, hpMs = 0, tailMs = 0, gpuTotMs = 0;
            if (qr == VK_SUCCESS) {
                auto d = [&](int i, int j) {
                    return (double)(ts[j] - ts[i]) * (double)tsPeriodNs / 1e6;
                };
                feMs = d(0, 1); fpMs = d(1, 2); chMs = d(2, 3);
                hpMs = d(3, 4); tailMs = d(4, 5); gpuTotMs = d(0, 5);
            }
            std::printf("[frame] %ld processed in %.1f ms (acq %.1f fbmean %.2f bridge %.1f rec %.1f gpu %.1f "
                        "| fe %.1f fp %.1f chain %.1f hp %.1f tail %.1f)\n",
                        frame, gpuTotMs, acqMs, fbMeanLast, bridgeMs, recMs, gpuMs,
                        feMs, fpMs, chMs, hpMs, tailMs);
        }
        if (frame == 1) {   // one-shot chain-path statistics (min/max/mean/nonzero%)
            auto dumpF = [](const char* nm, const float* p, size_t n) {
                double mn = 1e300, mx = -1e300, mean = 0.0; size_t nz = 0;
                for (size_t i = 0; i < n; ++i) {
                    double v = p[i];
                    if (v < mn) mn = v; if (v > mx) mx = v; mean += v;
                    if (v != 0.0) ++nz;
                }
                mean /= (double)(n ? n : 1);
                std::printf("[dbg] %-7s n=%zu min=%.6g max=%.6g mean=%.6g nonzero=%.1f%%\n",
                            nm, n, mn, mx, mean, 100.0 * (double)nz / (double)(n ? n : 1));
            };
            void* pd = nullptr;
            VK_CHECK(vkMapMemory(c.dev, memDbg, 0, VK_WHOLE_SIZE, 0, &pd));
            const char* db = (const char*)pd;
            dumpF("featV", (const float*)(db + dbgOffFeat), TOK * 16);
            dumpF("HEAD", (const float*)(db + dbgOffHead), TOK * 16);
            dumpF("head4", (const float*)(db + dbgOffH4), TOK * 4);
            {   // per-channel DC vs spatial detail of the head residual
                const float* h4 = (const float*)(db + dbgOffH4);
                for (uint32_t c = 0; c < 4; ++c) {
                    double mean = 0, var = 0;
                    for (uint32_t t = 0; t < TOK; ++t) mean += h4[t * 4 + c];
                    mean /= TOK;
                    for (uint32_t t = 0; t < TOK; ++t) { double d = h4[t * 4 + c] - mean; var += d * d; }
                    var /= TOK;
                    std::printf("[dbg] head4 ch%u: mean=%.4f std=%.4f (DC-removed delta 0.25xstd=%.4f)\n",
                                c, mean, std::sqrt(var), 0.25 * std::sqrt(var));
                }
            }
            auto dumpU16 = [](const char* nm, const uint16_t* p, size_t n) {
                uint32_t mn = 0xffff, mx = 0; size_t nz = 0;
                for (size_t i = 0; i < n; ++i) {
                    uint32_t v = p[i];
                    if (v < mn) mn = v; if (v > mx) mx = v;
                    if (v != 0 && v != 0x8000) ++nz;
                }
                std::printf("[dbg] %-7s n=%zu min=0x%04x max=0x%04x nonzero=%.1f%%\n",
                            nm, n, mn, mx, 100.0 * (double)nz / (double)(n ? n : 1));
            };
            dumpU16("x16", (const uint16_t*)(db + dbgOffX16), TOK * 16);
            dumpF("ADA", (const float*)(db + dbgOffADA), TOK * 32);
            dumpF("MRG70", (const float*)(db + dbgOffMRG), TOK * 32);
            dumpF("G2", (const float*)(db + dbgOffG2), TOK * 32);
            dumpU16("headW", (const uint16_t*)(db + dbgOffHW), 512);
            auto h2f = [](uint16_t h) -> float {
                uint32_t s = (uint32_t)(h & 0x8000) << 16;
                uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
                uint32_t f;
                if (e == 0) {
                    if (m == 0) f = s;
                    else {
                        int sh = 0; while (!(m & 0x400)) { m <<= 1; ++sh; }
                        m &= 0x3ff;
                        e = (uint32_t)(127 - 15 - 10 + sh) << 23;
                        f = s | e | (m << 13);
                    }
                } else if (e == 31) f = s | 0x7f800000u | (m << 13);
                else f = s | ((e + 112) << 23) | (m << 13);
                float r; std::memcpy(&r, &f, 4); return r;
            };
            auto dumpH = [&](const char* nm, const uint16_t* p, size_t n) {
                double mn = 1e300, mx = -1e300, mean = 0.0; size_t nz = 0, ninf = 0;
                for (size_t i = 0; i < n; ++i) {
                    double v = h2f(p[i]);
                    if (std::isinf(v) || std::isnan(v)) { ++ninf; continue; }
                    if (v < mn) mn = v; if (v > mx) mx = v; mean += v;
                    if (v != 0.0) ++nz;
                }
                mean /= (double)(n ? n : 1);
                std::printf("[dbg] %-7s n=%zu min=%.6g max=%.6g mean=%.6g nonzero=%.1f%% nan/inf=%zu | first8:",
                            nm, n, mn, mx, mean, 100.0 * (double)nz / (double)(n ? n : 1), ninf);
                for (size_t i = 0; i < 8 && i < n; ++i) std::printf(" %.4g", h2f(p[i]));
                std::printf("\n");
            };
            dumpH("B4DS", (const uint16_t*)(db + dbgOffB4), TOK * 64);
            dumpH("B22DS", (const uint16_t*)(db + dbgOffB22), TOK * 512);
            dumpH("B38", (const uint16_t*)(db + dbgOffB38), TOK * 1024);
            dumpH("B48", (const uint16_t*)(db + dbgOffB48), TOK * 256);
            dumpH("B69", (const uint16_t*)(db + dbgOffB69), TOK * 32);
            dumpF("sinT", (const float*)(db + dbgOffSin), 32);
            dumpF("cosT", (const float*)(db + dbgOffCos), 32);
            dumpH("FRS", (const uint16_t*)(db + dbgOffFRS), TOK * 32);
            dumpF("headUp", (const float*)(db + dbgOffUp), (size_t)regionW * regionH * 4);
            vkUnmapMemory(c.dev, memDbg);
        }

        // ---- (f) verification: BMP + M3 content checks vs native capture.
        if (verify) {
            std::vector<uint8_t> finalPx((size_t)W * H * 4);
            void* p1 = nullptr;
            VK_CHECK(vkMapMemory(c.dev, memRbFinal, 0, (uint64_t)W * H * 4, 0, &p1));
            std::memcpy(finalPx.data(), p1, (uint64_t)W * H * 4);
            vkUnmapMemory(c.dev, memRbFinal);
            char path[64];
            std::snprintf(path, sizeof(path), "out\\m8b_frame_%ld.bmp", frame);
            if (!WriteBmpBGRA(path, finalPx, W, H))
                std::fprintf(stderr, "[FAIL] write %s\n", path);
            if (nativeRef.empty()) nativeRef = original;
            if (!savedPair && frame == 30) {   // one full-res native vs processed pair for the user
                WriteBmpBGRA("out\\m8b_native.bmp", nativeRef, W, H);
                WriteBmpBGRA("out\\m8b_processed.bmp", finalPx, W, H);
                savedPair = true;
            }
            double meanAbs[3] = {0, 0, 0}, gradD[3] = {0, 0, 0};
            uint64_t changed = 0, total = (uint64_t)regionW * regionH;
            for (int y = 0; y < regionH; ++y) {
                for (int x = 0; x < regionW; ++x) {
                    size_t fullP = (size_t)(y + region.top) * W + (x + region.left);
                    int d[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        d[ch] = (int)finalPx[fullP * 4 + ch] - (int)nativeRef[fullP * 4 + ch];
                        meanAbs[ch] += std::abs(d[ch]);
                    }
                    if (d[0] || d[1] || d[2]) ++changed;
                    int x2 = x < regionW - 1 ? x + 1 : x;
                    int y2 = y < regionH - 1 ? y + 1 : y;
                    size_t fullX = (size_t)(y + region.top) * W + (x2 + region.left);
                    size_t fullY = (size_t)(y2 + region.top) * W + (x + region.left);
                    for (int ch = 0; ch < 3; ++ch) {
                        double gx = (double)((int)finalPx[fullX * 4 + ch] - (int)nativeRef[fullX * 4 + ch]) - d[ch];
                        double gy = (double)((int)finalPx[fullY * 4 + ch] - (int)nativeRef[fullY * 4 + ch]) - d[ch];
                        gradD[ch] += std::sqrt(gx * gx + gy * gy);
                    }
                }
            }
            for (int ch = 0; ch < 3; ++ch) { meanAbs[ch] /= (double)total; gradD[ch] /= (double)total; }
            double meanGrad = (gradD[0] + gradD[1] + gradD[2]) / 3.0;
            bool ok = (meanAbs[0] > 0 && meanAbs[1] > 0 && meanAbs[2] > 0 &&
                       meanGrad > 0 && changed > 0);
            passMetrics = passMetrics && ok;
            anyVerify = true;
            std::printf("[metrics] frame %ld region %dx%d: mean|final-native| B=%.5f G=%.5f R=%.5f "
                        "(must be > 0)\n", frame, regionW, regionH, meanAbs[0], meanAbs[1], meanAbs[2]);
            std::printf("[metrics] structure: mean |grad(delta)| B=%.5f G=%.5f R=%.5f (> 0); changed %.2f%% "
                        "=> %s\n", gradD[0], gradD[1], gradD[2],
                        100.0 * (double)changed / (double)total, ok ? "PASS" : "FAIL");
            ++verifyIdx;
        }

        // ---- (g) rolling fps + title + console
        frameTimes.push_back(std::chrono::duration<double>(clk::now().time_since_epoch()).count());
        if (frameTimes.size() > 60) frameTimes.pop_front();
        double fps = 0.0;
        if (frameTimes.size() > 1)
            fps = (frameTimes.size() - 1) / (frameTimes.back() - frameTimes.front());
        fpsFinal = fps;
        if (g_hwnd) {
            wchar_t title[96];
            swprintf(title, 96, L"DLSS5_INTEL m8b | %.1f fps", fps);
            SetWindowTextW(g_hwnd, title);
        }
        if ((frame % 60) == 0 || frame == framesTarget - 1) {
            std::printf("[m8b] frame %ld | rolling %.1f fps | dropped %ld | %s\n",
                        frame, fps, dropped, fresh ? "fresh" : "stale-reuse");
        }
        ++frame;
    }
    const auto tLoopEnd = clk::now();

    // ===================================================================
    // 13. Summary + teardown.
    // ===================================================================
    const double totalSec = std::chrono::duration<double>(tLoopEnd - tLoopStart).count();
    const double avgFps = frame > 0 ? (double)frame / totalSec : 0.0;
    std::printf("\n=== M8B-LIVE summary ===\n");
    std::printf("frames: %ld, wall: %.2f s, avg fps: %.2f, last rolling fps: %.1f\n",
                frame, totalSec, avgFps, fpsFinal);
    std::printf("dropped (DDA timeout/black/access-lost): %ld\n", dropped);
    if (!novideo)
        std::printf("feedback-cancellation: %ld processed frames, %ld settled skips, "
                    "last mean|corrected delta| = %.3f/255 (clamp +/-%d)\n",
                    fbFrames, settled, fbMeanLast, maxDelta);
    std::printf("present: %s, format %d, path %s\n",
                presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO", (int)swapFormat,
                novideo ? "none (--novideo)" : (swapStorage ? "blit.comp STORAGE" : "vkCmdCopyImage"));
    std::printf("chain: full 71-block DLSSNR graph @ 288 tokens (real weights, resident)\n");
    std::printf("verify frames {10,30,60}: %s\n", anyVerify ? (passMetrics ? "ALL PASS" : "FAIL") : "none run");
    bool pass = anyVerify && passMetrics && frame >= framesTarget;
    std::printf("=== M8B-LIVE result: %s ===\n", pass ? "PASS" : "FAIL");

    vkDeviceWaitIdle(c.dev);
    vkUnmapMemory(c.dev, memUpload);
    vkUnmapMemory(c.dev, memMax);
    if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
    vkDestroyFence(c.dev, fence, nullptr);
    vkDestroyQueryPool(c.dev, tsPool, nullptr);
    vkDestroySemaphore(c.dev, semImage, nullptr);
    vkDestroySemaphore(c.dev, semRender, nullptr);
    vkDestroyCommandPool(c.dev, cmdPool, nullptr);
    for (VkPipeline p : {pipeDecode, pipeLetter, pipeRescale, pipeFeatures, pipeCompose,
                         pipeEncode, pipeBlit})
        vkDestroyPipeline(c.dev, p, nullptr);
    vkDestroyPipelineLayout(c.dev, pipeLayout, nullptr);
    for (ChainPipe* cp : {&pGemm, &pGemm1, &pCos, &pCosW, &pSmax, &pEw, &pPart, &pTrans,
                          &pGather, &pMerge, &pFeatpack, &pHeadpack}) {
        vkDestroyPipeline(c.dev, cp->pipe, nullptr);
        vkDestroyPipelineLayout(c.dev, cp->layout, nullptr);
        vkDestroyDescriptorSetLayout(c.dev, cp->dsl, nullptr);
    }
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsLayout, nullptr);
    for (VkBuffer b : {bufRgb, bufMax, bufT, bufHeadUp, bufFin, bufNr, bufRbFinal,
                       bufRbNr, bufUpload, bufDbg, bufLastPresented})
        vkDestroyBuffer(c.dev, b, nullptr);
    for (VkDeviceMemory m : {memRgb, memMax, memT, memHeadUp, memFin, memNr,
                             memRbFinal, memRbNr, memDbg, memImgFinal, memImgNr, memImgIn, memUpload,
                             memLastP})
        vkFreeMemory(c.dev, m, nullptr);
    if (cbuf.mapped) vkUnmapMemory(c.dev, cbuf.mem);
    if (cbuf.buf) vkDestroyBuffer(c.dev, cbuf.buf, nullptr);
    if (cbuf.mem) vkFreeMemory(c.dev, cbuf.mem, nullptr);
    if (dev.buf) vkDestroyBuffer(c.dev, dev.buf, nullptr);
    if (dev.mem) vkFreeMemory(c.dev, dev.mem, nullptr);
    vkDestroyImageView(c.dev, imgInView, nullptr);
    vkDestroyImageView(c.dev, viewFinal, nullptr);
    vkDestroyImageView(c.dev, viewNr, nullptr);
    for (auto v : swapViews) vkDestroyImageView(c.dev, v, nullptr);
    vkDestroyImage(c.dev, imgIn, nullptr);
    vkDestroyImage(c.dev, imgFinal, nullptr);
    vkDestroyImage(c.dev, imgNr, nullptr);
    if (swapchain) vkDestroySwapchainKHR(c.dev, swapchain, nullptr);
    vkDestroyDevice(c.dev, nullptr);
    if (surface) vkDestroySurfaceKHR(c.inst, surface, nullptr);
    vkDestroyInstance(c.inst, nullptr);
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        if (msgThread) {
            WaitForSingleObject(msgThread, 2000);
            CloseHandle(msgThread);
        }
        if (g_hwnd) DestroyWindow(g_hwnd);
    }

    return pass ? 0 : 1;
}
