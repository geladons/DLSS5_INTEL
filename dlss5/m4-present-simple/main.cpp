// ============================================================================
// M4-SIMPLE live overlay — DLSS5_INTEL
//
// LIVE loop version of the proven M3 neural pass slice (m3-neural-passthrough,
// commit a458bbb, passes on this machine): a borderless topmost click-through
// overlay window + Vulkan swapchain shows the PROCESSED desktop in real time.
//
// Data flow per frame (CPU bridge — zero-copy is explicitly OUT OF SCOPE,
// deferred to milestone M5):
//
//   DDA AcquireNextFrame (content-checked; wiggle activity generator)
//     -> D3D11 CopyResource to staging -> CPU map
//     -> memcpy to persistent-mapped HOST_COHERENT upload buffer
//     -> vkCmdCopyBufferToImage into the plain Vulkan input image
//     -> [M3 pipeline UNCHANGED: decode -> letterbox -> rescale down ->
//        features -> standin learned block -> rescale up -> compose -> encode]
//     -> blit.comp storage copy into the acquired swapchain image
//     -> vkQueuePresentKHR
//
// M3 doctrine kept exactly: ONE command buffer, record -> submit ->
// vkWaitForFences (FULL GPU idle) -> re-record. No async pipelining, no
// timeline semaphores, no imported memory. The previous m4-live-present
// attempt (zero-copy shared-texture import + per-frame timeline-semaphore
// wait) failed at ~frame 4 — likely driver lazy-release semantics; that work
// is deferred to M5 with a rotating-buffer design doc.
//
// Metrics: rolling fps in console + window title, dropped/stale frame
// counters. On loop frames 10/30/60 the processed frame is ALSO read back to
// out\m4s_frame_{n}.bmp and M3's content checks are computed: per-channel
// mean|final-native| > 0 and mean |grad(delta)| > 0 (delta not a uniform
// shift). The wiggle cursor appears in capture frames — expected, not error.
//
// CLI: --frames N (default 300), --nowiggle, --scale S (default 0.55).
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

static constexpr uint32_t W = 2560, H = 1440;

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

static int Align64(float v) {
    int q = (int)std::lround(v / 64.0f) * 64;
    return q < 320 ? 320 : q;
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

int main(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();

    // ---------------- CLI
    long framesTarget = 300;
    int wiggle = 1;
    float renderScale = 0.55f;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) framesTarget = std::atol(argv[++i]);
        else if (a == "--nowiggle") wiggle = 0;
        else if (a == "--scale" && i + 1 < argc) renderScale = (float)std::atof(argv[++i]);
        else { std::fprintf(stderr, "usage: m4simple [--frames N] [--nowiggle] [--scale S]\n"); return 1; }
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    std::printf("=== M4-SIMPLE: live processed-desktop overlay (CPU bridge; zero-copy DEFERRED to M5) ===\n");
    std::printf("frame target: %ux%u B8G8R8A8, render_scale=%.2f, frames=%ld, wiggle=%d\n\n",
                W, H, renderScale, framesTarget, wiggle);
    CreateDirectoryA("out", nullptr);

    // ===================================================================
    // 1. D3D11 hardware device + DDA (M2/M3 path; staging texture only —
    //    NO shared texture, NO cross-API fences: CPU bridge instead).
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
        HRESULT hr = out1->DuplicateOutput(device.Get(), &dup); // M1: DuplicateOutput1 rejected by driver
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
    // 2. Initial content-checked DDA frame (M3 doctrine: first frame(s) are
    //    black warm-up on this Arc driver — wiggle ON during the acquire to
    //    guarantee DDA delivery, stop afterwards if --nowiggle).
    // ===================================================================
    std::vector<uint8_t> original(W * H * 4);   // last good native frame
    {
        auto tAcquireBegin = clk::now();
        auto deadline = tAcquireBegin + std::chrono::seconds(8);
        auto acceptAnyAt = tAcquireBegin + std::chrono::seconds(6);
        std::thread wiggler;
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
        std::printf("[info] cursor-wiggle activity generator ON for initial acquire (DDA is dirty-rect driven)\n");
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
        if (!wiggle) { g_wiggleStop.store(true); }
        if (wiggler.joinable()) wiggler.join();
        if (!got) { std::fprintf(stderr, "[FAIL] no DDA frame within ~8s\n"); return 1; }
        // full-res host copy of the native frame (diagnostic artifact)
        D3D11_MAPPED_SUBRESOURCE ms{};
        HR_CHECK(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms));
        for (uint32_t y = 0; y < H; ++y)
            std::memcpy(&original[(size_t)y * W * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
        context->Unmap(stagingTex.Get(), 0);
        WriteBmpBGRA("out\\m4s_native0.bmp", original, W, H);
        {
            double s = 0; for (size_t i = 0; i < original.size(); i += 4003) s += original[i];
            std::printf("[cap] native sample mean=%.2f (0 => capture black)\n",
                        s / (double)(original.size() / 4003 + 1));
        }
    }
    // keep wiggling for the whole live loop unless --nowiggle
    std::thread wiggler;
    if (wiggle) {
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
        std::printf("[info] wiggle ON for live loop\n");
    }

    // ===================================================================
    // 3. Vulkan: instance (with win32 surface) + LUID-matched device.
    // ===================================================================
    VkCtx c{};
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "m4-simple";
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
    // 4. Borderless topmost click-through overlay window + surface.
    // ===================================================================
    HANDLE msgThread = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = L"DLSS5M4SimpleOverlay";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        if (!RegisterClassW(&wc)) { std::fprintf(stderr, "[FAIL] RegisterClassW %lu\n", GetLastError()); return 1; }
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        g_hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"DLSS5_INTEL m4s",
                                 WS_POPUP, outX, outY, W, H,
                                 nullptr, nullptr, hinst, nullptr);
        if (!g_hwnd) { std::fprintf(stderr, "[FAIL] CreateWindowExW %lu\n", GetLastError()); return 1; }
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, outX, outY, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::printf("[win] overlay created: %ux%u at (%d,%d), WS_POPUP | TOPMOST | TRANSPARENT | LAYERED\n",
                    W, H, outX, outY);
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(c.inst, &sci, nullptr, &surface));
        DWORD mtid = 0;
        msgThread = CreateThread(nullptr, 0, MsgPumpThread, nullptr, 0, &mtid);
    }

    // ---------------- device with swapchain support
    {
        const char* want[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = c.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = want;
        VK_CHECK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev));
        vkGetDeviceQueue(c.dev, c.qf, 0, &c.queue);
        std::printf("[VK] logical device up (compute+graphics family %u, swapchain ext)\n", c.qf);
    }

    // ---------------- swapchain: 3 images, B8G8R8A8, MAILBOX if offered
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool swapStorage = false;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    {
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
    // 5. Plain Vulkan input image + persistent-mapped upload buffer
    //    (THIS replaces M3's shared-texture import: the CPU bridge).
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
    // 6. Shared pipeline layout + 8 pipelines (M3's 7 + blit).
    // ===================================================================
    VkDescriptorSetLayout dsLayout;
    {
        VkDescriptorSetLayoutBinding binds[10]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 1; i <= 8; ++i)
            binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        binds[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 10; ci.pBindings = binds;
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
    VkPipeline pipeStandin = makePipe("standin");
    VkPipeline pipeCompose = makePipe("compose");
    VkPipeline pipeEncode = makePipe("encode");
    VkPipeline pipeBlit = makePipe("blit");
    std::printf("[VK] 8 compute pipelines up (M3's 7 + blit)\n");

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
    VkSemaphore semImage = VK_NULL_HANDLE, semRender = VK_NULL_HANDLE;
    {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(c.dev, &sci, nullptr, &semImage));
        VK_CHECK(vkCreateSemaphore(c.dev, &sci, nullptr, &semRender));
    }
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
    // 7. Wave-1 buffers + frame-0 decode + letterbox region scan (verbatim M3,
    //    but input arrives via the CPU upload bridge).
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
        // host upload -> imgIn (GPU image)
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
    const int netW = Align64(regionW * renderScale);
    const int netH = Align64(regionH * renderScale);
    std::printf("[geometry] region %dx%d -> network extent %dx%d (x%.2f, nearest 64, min 320)\n",
                regionW, regionH, netW, netH, renderScale);

    // ===================================================================
    // 8. Wave-2 buffers + output image + full descriptor writes (verbatim M3;
    //    imgNr/bufRbNr kept allocated for descriptor-layout compatibility but
    //    the nr_out encode is NOT dispatched in the live loop).
    // ===================================================================
    VkDeviceMemory memT = VK_NULL_HANDLE, memFeat = VK_NULL_HANDLE, memHead = VK_NULL_HANDLE,
                   memHeadUp = VK_NULL_HANDLE, memFin = VK_NULL_HANDLE, memNr = VK_NULL_HANDLE,
                   memRbFinal = VK_NULL_HANDLE, memRbNr = VK_NULL_HANDLE,
                   memImgFinal = VK_NULL_HANDLE, memImgNr = VK_NULL_HANDLE;
    const uint64_t Tfloats = std::max<uint64_t>((uint64_t)netH * regionW * 3,
                                                (uint64_t)regionH * netW * 4);
    VkBuffer bufT = CreateBuf(c, Tfloats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memT);
    VkBuffer bufFeat = CreateBuf(c, (uint64_t)netW * netH * 16 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memFeat);
    VkBuffer bufHead = CreateBuf(c, (uint64_t)netW * netH * 4 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memHead);
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

    VkImage imgFinal = CreateImage2D(c, W, H, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgFinal);
    VkImage imgNr = CreateImage2D(c, regionW, regionH, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &memImgNr);
    VkImageView viewFinal = CreateView(c, imgFinal, VK_FORMAT_B8G8R8A8_UNORM);
    VkImageView viewNr = CreateView(c, imgNr, VK_FORMAT_B8G8R8A8_UNORM);
    {
        writeBuf(setFinal, 2, bufT, Tfloats * 4);
        writeBuf(setFinal, 3, bufFeat, (uint64_t)netW * netH * 16 * 4);
        writeBuf(setFinal, 4, bufHead, (uint64_t)netW * netH * 4 * 4);
        writeBuf(setFinal, 5, bufHeadUp, (uint64_t)regionW * regionH * 4 * 4);
        writeBuf(setFinal, 6, bufFin, (uint64_t)regionW * regionH * 3 * 4);
        writeBuf(setFinal, 7, bufNr, (uint64_t)regionW * regionH * 3 * 4);
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
    // 9. LIVE LOOP: per frame =
    //    DDA acquire (content-checked) -> CPU map -> upload memcpy ->
    //    [upload copy + M3 pipeline + blit to swapchain] (one cmd buffer,
    //    submit, FULL fence wait - M3 doctrine) -> present -> metrics.
    // ===================================================================
    const uint32_t regionOff = (uint32_t)(region.top * (int)W + region.left);
    const long verifyFrames[3] = {10, 30, 60};
    int verifyIdx = 0;
    long frame = 0, dropped = 0, stale = 0;
    double fpsFinal = 0.0;
    bool passMetrics = true;
    bool anyVerify = false;
    std::deque<double> frameTimes;
    std::vector<uint8_t> nativeRef;
    std::printf("[m4s] entering live loop (%ld frames)\n", framesTarget);
    const auto tLoopStart = clk::now();

    while (!g_stop.load() && frame < framesTarget) {
        // ---- (a) DDA capture; stale-reuse last frame on timeout/black.
        bool fresh = false;
        {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(100, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                ++dropped;   // no new desktop frame this interval
            } else if (FAILED(hr)) {
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
            } else {
                ComPtr<ID3D11Texture2D> frameTex;
                hr = res->QueryInterface(IID_PPV_ARGS(&frameTex));
                if (FAILED(hr)) { dup->ReleaseFrame(); ++dropped; }
                else {
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
                        black = (cnt == 0) || (s / (double)cnt) < 1.0;
                        if (!black) {
                            bool isVerify = (verifyIdx < 3 && verifyFrames[verifyIdx] == frame);
                            for (uint32_t y = 0; y < H; ++y)
                                std::memcpy(&original[(size_t)y * W * 4],
                                            (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
                            if (isVerify) nativeRef = original;
                        }
                        context->Unmap(stagingTex.Get(), 0);
                    }
                    if (black) { ++dropped; }
                    else fresh = true;
                }
            }
            if (fresh) stale = 0; // (kept simple: stale frames just reuse `original`)
        }
        if (g_stop.load()) break;

        // ---- (b) CPU bridge: staging -> upload buffer (only when fresh;
        //          otherwise the previous frame's pixels are still uploaded).
        if (fresh) std::memcpy(uploadPtr, original.data(), (size_t)W * H * 4);

        // ---- (c) acquire swapchain image (fully serialized: previous frame
        //          already fence-waited, so an image is always available).
        uint32_t imageIndex = 0;
        {
            VkResult ar = VK_SUCCESS;
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

        // ---- (d) record frame: upload + M3 pipeline + verify copy + blit.
        const bool verify = (verifyIdx < 3 && verifyFrames[verifyIdx] == frame);
        begin();
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
        {   // M3 steady pipeline (verbatim dispatch params; region fixed)
            Push p{};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
            vkCmdDispatch(cmd, W / 16, H / 16, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLetter);
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.b[0] = 0; push(p);
            vkCmdDispatch(cmd, (H + 255) / 256, 1, 1);
            barrierAll();
            p.b[0] = 1; push(p);
            vkCmdDispatch(cmd, (W + 255) / 256, 1, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale);
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)regionOff; p.a[2] = regionH; p.a[3] = regionW;
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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeStandin);
            p.a[0] = netW; p.a[1] = netH; push(p);
            vkCmdDispatch(cmd, ((uint64_t)netW * netH + 255) / 256, 1, 1);
            barrierAll();
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
            p.b[0] = regionW; p.b[1] = regionH; push(p);
            vkCmdDispatch(cmd, ((uint64_t)regionW * regionH + 255) / 256, 1, 1);
            barrierAll();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode);
            p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.a[2] = region.left; p.a[3] = region.top;
            p.b[0] = regionW; p.b[1] = regionH; p.b[2] = 0; push(p);
            vkCmdDispatch(cmd, (W + 15) / 16, (H + 15) / 16, 1);
            barrierAll();
        }
        if (verify) {   // readback copy for the verification frames
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
        {   // blit composite -> swapchain image
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

        // ---- (e) submit (wait acquire, signal render) + present + FULL wait.
        {
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = &semImage; si.pWaitDstStageMask = &st;
            si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semRender;
            VK_CHECK(vkQueueSubmit(c.queue, 1, &si, fence));
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semRender;
            pi.swapchainCount = 1; pi.pSwapchains = &swapchain;
            pi.pImageIndices = &imageIndex;
            vkQueuePresentKHR(c.queue, &pi);
            VK_CHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkResetFences(c.dev, 1, &fence);
        }

        // ---- (f) verification: BMP + M3 content checks vs native capture.
        if (verify) {
            std::vector<uint8_t> finalPx((size_t)W * H * 4);
            void* p1 = nullptr;
            VK_CHECK(vkMapMemory(c.dev, memRbFinal, 0, (uint64_t)W * H * 4, 0, &p1));
            std::memcpy(finalPx.data(), p1, (uint64_t)W * H * 4);
            vkUnmapMemory(c.dev, memRbFinal);
            char path[64];
            std::snprintf(path, sizeof(path), "out\\m4s_frame_%ld.bmp", frame);
            if (!WriteBmpBGRA(path, finalPx, W, H))
                std::fprintf(stderr, "[FAIL] write %s\n", path);
            if (nativeRef.empty()) nativeRef = original;
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
            swprintf(title, 96, L"DLSS5_INTEL m4s | %.1f fps", fps);
            SetWindowTextW(g_hwnd, title);
        }
        if ((frame % 60) == 0 || frame == framesTarget - 1) {
            std::printf("[m4s] frame %ld | rolling %.1f fps | dropped %ld | %s\n",
                        frame, fps, dropped, fresh ? "fresh" : "stale-reuse");
        }
        ++frame;
    }
    const auto tLoopEnd = clk::now();

    // ===================================================================
    // 10. Summary + teardown.
    // ===================================================================
    const double totalSec = std::chrono::duration<double>(tLoopEnd - tLoopStart).count();
    const double avgFps = frame > 0 ? (double)frame / totalSec : 0.0;
    std::printf("\n=== M4-SIMPLE summary ===\n");
    std::printf("frames: %ld, wall: %.2f s, avg fps: %.2f, last rolling fps: %.1f\n",
                frame, totalSec, avgFps, fpsFinal);
    std::printf("dropped (DDA timeout/black/access-lost): %ld\n", dropped);
    std::printf("present: %s, format %d, path %s\n",
                presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO", (int)swapFormat,
                swapStorage ? "blit.comp STORAGE" : "vkCmdCopyImage");
    std::printf("verify frames {10,30,60}: %s\n", anyVerify ? (passMetrics ? "ALL PASS" : "FAIL") : "none run");
    bool pass = anyVerify && passMetrics && frame >= framesTarget;
    std::printf("=== M4-SIMPLE result: %s ===\n", pass ? "PASS" : "FAIL");

    vkDeviceWaitIdle(c.dev);
    vkUnmapMemory(c.dev, memUpload);
    vkUnmapMemory(c.dev, memMax);
    if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
    vkDestroyFence(c.dev, fence, nullptr);
    vkDestroySemaphore(c.dev, semImage, nullptr);
    vkDestroySemaphore(c.dev, semRender, nullptr);
    vkDestroyCommandPool(c.dev, cmdPool, nullptr);
    for (VkPipeline p : {pipeDecode, pipeLetter, pipeRescale, pipeFeatures, pipeStandin, pipeCompose,
                         pipeEncode, pipeBlit})
        vkDestroyPipeline(c.dev, p, nullptr);
    vkDestroyPipelineLayout(c.dev, pipeLayout, nullptr);
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsLayout, nullptr);
    for (VkBuffer b : {bufRgb, bufMax, bufT, bufFeat, bufHead, bufHeadUp, bufFin, bufNr, bufRbFinal,
                       bufRbNr, bufUpload})
        vkDestroyBuffer(c.dev, b, nullptr);
    for (VkDeviceMemory m : {memRgb, memMax, memT, memFeat, memHead, memHeadUp, memFin, memNr,
                             memRbFinal, memRbNr, memImgFinal, memImgNr, memImgIn, memUpload})
        vkFreeMemory(c.dev, m, nullptr);
    vkDestroyImageView(c.dev, imgInView, nullptr);
    vkDestroyImageView(c.dev, viewFinal, nullptr);
    vkDestroyImageView(c.dev, viewNr, nullptr);
    for (auto v : swapViews) vkDestroyImageView(c.dev, v, nullptr);
    vkDestroyImage(c.dev, imgIn, nullptr);
    vkDestroyImage(c.dev, imgFinal, nullptr);
    vkDestroyImage(c.dev, imgNr, nullptr);
    vkDestroySwapchainKHR(c.dev, swapchain, nullptr);
    vkDestroyDevice(c.dev, nullptr);
    vkDestroySurfaceKHR(c.inst, surface, nullptr);
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
