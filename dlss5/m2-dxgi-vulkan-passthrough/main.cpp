// ============================================================================
// M2 D3D11(DDA) -> Vulkan external-memory interop passthrough  --  DLSS5_INTEL
//
// Capture ONE fresh desktop frame via DXGI Desktop Duplication on the D3D11
// device, CopyResource it into a B8G8R8A8 shared texture (NT handle), import
// that memory into Vulkan as an OPTIMAL-tiling storage image, run an invert
// compute shader on it, copy the result back to a host-visible buffer, save
// both BMPs, and verify inverted == 255 - original per color channel.
//
// THE cross-API gate for the whole port: VkPhysicalDeviceIDProperties.deviceLUID
// must match the DXGI adapter LUID, and the D3D11 fence must be importable as a
// Vulkan timeline semaphore (sync) — otherwise cross-API ordering is guesswork.
//
// Exit codes: 0 = pixel verification PASS, 1 = any failure.
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
#include <d3d11_4.h> // ID3D11Device5 / ID3D11Fence / ID3D11DeviceContext4
#include <wrl.h>

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

static constexpr uint32_t W = 2560, H = 1440;
static constexpr uint64_t FRAME_BYTES = (uint64_t)W * H * 4;

#define VK_CHECK(x)                                                             \
    do {                                                                        \
        VkResult res_ = (x);                                                    \
        if (res_ != VK_SUCCESS) {                                               \
            std::fprintf(stderr, "[VK-FAIL] %s -> %d at %s:%d\n", #x, (int)res_, \
                         __FILE__, __LINE__);                                   \
            return 1;                                                           \
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

// Load a 32bpp BI_RGB bottom-up BMP written by WriteBmpBGRA. Row pitch in file
// may be padded to 4 bytes; with w*4 already a multiple of 4 it matches w*4.
static bool LoadBmpBGRA(const std::string& path, std::vector<uint8_t>& bgra,
                        uint32_t& w, uint32_t& h) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw((size_t)sz);
    if (!f.read(reinterpret_cast<char*>(raw.data()), sz)) return false;
    if (sz < (std::streamsize)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)))
        return false;
    auto bfh = reinterpret_cast<const BITMAPFILEHEADER*>(raw.data());
    auto bih = reinterpret_cast<const BITMAPINFOHEADER*>(raw.data() + sizeof(BITMAPFILEHEADER));
    if (bfh->bfType != 0x4D42 || bih->biCompression != BI_RGB || bih->biBitCount != 32)
        return false;
    if (bih->biHeight < 0) return false; // expect bottom-up
    w = (uint32_t)bih->biWidth;
    h = (uint32_t)bih->biHeight;
    const uint32_t rowBytes = ((w * 32 + 31) / 32) * 4; // == w*4 here
    bgra.assign((size_t)w * h * 4, 0);
    const uint8_t* src = raw.data() + bfh->bfOffBits;
    for (uint32_t y = 0; y < h; ++y) {
        // file row 0 is the BOTTOM of the image
        std::memcpy(&bgra[(size_t)(h - 1 - y) * w * 4], src + (size_t)y * rowBytes, w * 4);
    }
    return true;
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

// ------------------------------------------------------------ entry point --
int main(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();

    std::string outDir = "out";
    int wiggle = 1;
    if (argc > 1) outDir = argv[1];
    if (argc > 2) wiggle = std::atoi(argv[2]);
    if (argc > 3) {
        std::fprintf(stderr, "usage: m2interop [outdir] [wiggle 0/1]\n");
        return 1;
    }

    std::printf("=== M2: D3D11(DDA) -> Vulkan external-memory interop ===\n");
    std::printf("frame target: %ux%u B8G8R8A8, transform: INVERT rgb\n\n", W, H);
    CreateDirectoryA(outDir.c_str(), nullptr);

    // ===================================================================
    // 1. D3D11 hardware device + DDA capture of one fresh frame.
    //    M1 lessons carried over: DuplicateOutput (DuplicateOutput1 pinning
    //    is REJECTED by this driver); dirty-rect driven -> wiggle helper.
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
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] D3D11CreateDevice(HARDWARE): %s\n", HrName(hr));
            return 1;
        }
        std::printf("[D3D11] hardware device created, feature level 0x%x\n", (unsigned)got);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    HR_CHECK(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
    ComPtr<IDXGIAdapter> adapter;
    HR_CHECK(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC adesc{};
    adapter->GetDesc(&adesc);
    {
        char a[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, a, sizeof(a) - 1, nullptr, nullptr);
        bool warp = (adesc.VendorId == 0x1414);
        std::printf("[D3D11] adapter=\"%s\" vendor=0x%04x %s DXGI LUID=%s VRAM=%.0f MB\n",
                    a, adesc.VendorId, warp ? "(SOFTWARE - BAD)" : "(hardware)",
                    LuidStr(adesc.AdapterLuid).c_str(),
                    (double)adesc.DedicatedVideoMemory / (1024.0 * 1024.0));
        if (warp) { std::fprintf(stderr, "[FAIL] landed on WARP/software adapter\n"); return 1; }
    }

    // Pick the \\.\DISPLAY5 output if present, else first attached (M1 did the
    // same enumeration; DISPLAY5 is the real output on this host).
    ComPtr<IDXGIOutput> output;
    {
        ComPtr<IDXGIOutput> o;
        UINT i = 0;
        ComPtr<IDXGIOutput> firstAttached, display5;
        while (adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            char n[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, n, sizeof(n) - 1, nullptr, nullptr);
            std::printf("[D3D11] output[%u]: \"%s\" attached=%d\n", i, n, (int)d.AttachedToDesktop);
            if (d.AttachedToDesktop && !firstAttached) firstAttached = o;
            if (d.AttachedToDesktop && std::strcmp(n, "\\\\.\\DISPLAY5") == 0) display5 = o;
            ++i;
            o.Reset();
        }
        output = display5 ? display5 : firstAttached;
        if (!output) { std::fprintf(stderr, "[FAIL] no attached output\n"); return 1; }
    }

    ComPtr<IDXGIOutputDuplication> dup;
    {
        // M1 lesson (a): DuplicateOutput1 with pinned formats is REJECTED by
        // this Intel driver -> plain DuplicateOutput (driver still hands B8G8R8A8).
        ComPtr<IDXGIOutput1> out1;
        HR_CHECK(output->QueryInterface(IID_PPV_ARGS(&out1)));
        HRESULT hr = out1->DuplicateOutput(device.Get(), &dup);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] DuplicateOutput: %s (locked session / busy duplicator?)\n", HrName(hr));
            return 1;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        std::printf("[DDA] DuplicateOutput active: %ux%u format=%d (87=B8G8R8A8) rotation=%d\n",
                    (unsigned)dd.ModeDesc.Width, (unsigned)dd.ModeDesc.Height,
                    (int)dd.ModeDesc.Format, (int)dd.Rotation);
        if (dd.ModeDesc.Width != W || dd.ModeDesc.Height != H ||
            dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            std::fprintf(stderr, "[FAIL] expected %ux%u B8G8R8A8\n", W, H);
            return 1;
        }
    }

    // Our shared texture: the D3D11->Vulkan handoff surface.
    ComPtr<ID3D11Texture2D> sharedTex;
    D3D11_TEXTURE2D_DESC sharedDesc{};
    {
        sharedDesc.Width = W;
        sharedDesc.Height = H;
        sharedDesc.MipLevels = 1;
        sharedDesc.ArraySize = 1;
        sharedDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sharedDesc.SampleDesc = {1, 0};
        sharedDesc.Usage = D3D11_USAGE_DEFAULT;
        sharedDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // NT handle is REQUIRED for Vulkan import.
        sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HR_CHECK(device->CreateTexture2D(&sharedDesc, nullptr, &sharedTex));
    }
    // Staging texture for the CPU readback of the SAME frame.
    ComPtr<ID3D11Texture2D> stagingTex;
    {
        D3D11_TEXTURE2D_DESC sd = sharedDesc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        HR_CHECK(device->CreateTexture2D(&sd, nullptr, &stagingTex));
    }

    // D3D11.5 fence for the cross-API signal (shared NT handle -> Vulkan import).
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence11;
    HANDLE fenceNT = nullptr;
    {
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device5));
        if (SUCCEEDED(hr)) {
            hr = context->QueryInterface(IID_PPV_ARGS(&context4));
            if (SUCCEEDED(hr)) {
                hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11));
                if (SUCCEEDED(hr)) {
                    hr = fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceNT);
                    if (FAILED(hr)) {
                        std::printf("[sync] CreateSharedHandle(fence) failed 0x%08lx - no cross-API fence\n",
                                    (unsigned long)hr);
                        fence11.Reset(); fenceNT = nullptr;
                    }
                } else {
                    std::printf("[sync] CreateFence failed 0x%08lx - no cross-API fence\n", (unsigned long)hr);
                }
            }
        }
        std::printf("[sync] D3D11.5 fence: %s\n", fenceNT ? "available (NT handle)" : "UNAVAILABLE - host-wait fallback");
    }

    // ---- acquire ONE fresh frame (retry loop up to ~5s; wiggle un-idles DDA)
    const auto tAcquireBegin = clk::now();
    bool gotFrame = false;
    std::thread wiggler;
    if (wiggle) {
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
        std::printf("[info] cursor-wiggle activity generator ON (DDA is dirty-rect driven)\n");
    }
    {
        auto deadline = tAcquireBegin + std::chrono::seconds(5);
        int tries = 0;
        while (clk::now() < deadline && !gotFrame) {
            ++tries;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(500, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                std::fprintf(stderr, "[warn] ACCESS_LOST mid-acquire, re-creating duplication\n");
                dup.Reset(); Sleep(100);
                ComPtr<IDXGIOutput1> out1;
                HR_CHECK(output->QueryInterface(IID_PPV_ARGS(&out1)));
                HR_CHECK(out1->DuplicateOutput(device.Get(), &dup));
                continue;
            }
            if (FAILED(hr)) {
                std::fprintf(stderr, "[error] AcquireNextFrame: %s\n", HrName(hr));
                continue;
            }
            ComPtr<ID3D11Texture2D> frameTex;
            hr = res->QueryInterface(IID_PPV_ARGS(&frameTex));
            dup->ReleaseFrame();
            if (FAILED(hr)) continue;
            D3D11_TEXTURE2D_DESC fd{};
            frameTex->GetDesc(&fd);
            if (fd.Width != W || fd.Height != H || fd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                std::fprintf(stderr, "[FAIL] frame %ux%u fmt %d != expected\n",
                             (unsigned)fd.Width, (unsigned)fd.Height, (int)fd.Format);
                if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
                return 1;
            }
            // Hand the frame to BOTH destinations from the same source texture.
            context->CopyResource(sharedTex.Get(), frameTex.Get());
            context->CopyResource(stagingTex.Get(), frameTex.Get());
            if (context4 && fence11)
                context4->Signal(fence11.Get(), 1);   // cross-API GPU-side ordering
            context->Flush();
            gotFrame = true;
            std::printf("[DDA] acquired fresh frame on try %d (accumulated=%llu)\n",
                        tries, (unsigned long long)fi.AccumulatedFrames);
        }
    }
    if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
    if (!gotFrame) {
        std::fprintf(stderr, "[FAIL] no DDA frame within ~5s\n");
        return 1;
    }
    const auto tD3DReady = clk::now();

    // CPU readback of the original (same frame as handed to Vulkan).
    std::vector<uint8_t> original(W * H * 4);
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        HR_CHECK(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms));
        for (uint32_t y = 0; y < H; ++y)
            std::memcpy(&original[(size_t)y * W * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
        context->Unmap(stagingTex.Get(), 0);
        // If the cross-API fence is unavailable, fall back to a host-side wait
        // for D3D11 GPU completion (busy-poll the fence value).
        if (!fenceNT) {
            while (fence11 && fence11->GetCompletedValue() < 1) Sleep(0);
        }
    }

    // NT handle for the Vulkan import.
    HANDLE sharedNT = nullptr;
    {
        ComPtr<IDXGIResource1> res1;
        HR_CHECK(sharedTex->QueryInterface(IID_PPV_ARGS(&res1)));
        HR_CHECK(res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedNT));
        std::printf("[interop] shared texture NT handle: %p\n", (void*)sharedNT);
    }

    // ===================================================================
    // 2. Vulkan: instance, pick physical device by LUID match.
    // ===================================================================
    VkInstance inst;
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "m2-interop";
        app.apiVersion = VK_API_VERSION_1_2; // external memory is core since 1.1
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));
    }

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qfCompute = UINT32_MAX;
    std::string pdName;
    LUID vkLuid{};
    {
        uint32_t n = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
        std::vector<VkPhysicalDevice> pds(n);
        VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
        for (auto d : pds) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &id;
            vkGetPhysicalDeviceProperties2(d, &p2);
            LUID l{};
            if (id.deviceLUIDValid) std::memcpy(&l, id.deviceLUID, VK_LUID_SIZE);
            bool luidMatch = id.deviceLUIDValid &&
                             l.HighPart == adesc.AdapterLuid.HighPart &&
                             l.LowPart == adesc.AdapterLuid.LowPart;
            std::printf("[VK] device \"%s\" LUID=%s %s\n", p2.properties.deviceName,
                        id.deviceLUIDValid ? LuidStr(l).c_str() : "(invalid)",
                        luidMatch ? "  <== MATCHES DXGI adapter" : "");
            if (luidMatch && pd == VK_NULL_HANDLE) {
                pd = d;
                pdName = p2.properties.deviceName;
                vkLuid = l;
            }
        }
        if (pd == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[FAIL] no Vulkan device LUID-matches DXGI adapter %s\n",
                         LuidStr(adesc.AdapterLuid).c_str());
            return 1;
        }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps.data());
        for (uint32_t i = 0; i < nq; ++i)
            if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfCompute = i; break; }
        if (qfCompute == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no compute queue\n"); return 1; }
    }
    std::printf("[interop] LUID MATCH: DXGI %s == Vulkan %s (device \"%s\")\n",
                LuidStr(adesc.AdapterLuid).c_str(), LuidStr(vkLuid).c_str(), pdName.c_str());

    // ---- capability checks (importable D3D11 image? importable D3D11 fence?)
    {
        VkPhysicalDeviceExternalImageFormatInfo ext{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkPhysicalDeviceImageFormatInfo2 fmtInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        fmtInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        fmtInfo.type = VK_IMAGE_TYPE_2D;
        fmtInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        fmtInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        fmtInfo.pNext = &ext;
        VkExternalImageFormatProperties extProps{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        props2.pNext = &extProps;
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fmtInfo, &props2);
        std::printf("[cap] D3D11_IMAGE import for B8G8R8A8/OPTIMAL/STORAGE|TRANSFER_SRC: "
                    "query=%s importable=%d\n",
                    (r == VK_SUCCESS) ? "OK" : ((r == VK_ERROR_FORMAT_NOT_SUPPORTED) ? "NOT-SUPPORTED" : "err"),
                    (r == VK_SUCCESS &&
                     (extProps.externalMemoryProperties.externalMemoryFeatures &
                      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) ? 1 : 0);
    }
    bool fenceSemSupported = false;
    {
        VkPhysicalDeviceExternalSemaphoreInfo si{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &si, &sp);
        fenceSemSupported = (sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
        std::printf("[cap] D3D11_FENCE semaphore import: importable=%d\n", (int)fenceSemSupported);
    }

    // ---- logical device with win32 external-memory extensions
    VkDevice vkDev;
    VkQueue vkQueue;
    {
        const char* want[] = {
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        };
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
        std::vector<const char*> enable;
        for (auto w : want) {
            for (auto& e : exts)
                if (std::strcmp(e.extensionName, w) == 0) { enable.push_back(w); break; }
        }
        if (enable.size() != 2) {
            std::fprintf(stderr, "[FAIL] win32 external memory/semaphore extensions missing\n");
            return 1;
        }
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfCompute;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)enable.size();
        dci.ppEnabledExtensionNames = enable.data();
        VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &vkDev));
        vkGetDeviceQueue(vkDev, qfCompute, 0, &vkQueue);
        std::printf("[VK] logical device up: %s + compute queue family %u\n",
                    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, qfCompute);
    }

    auto fpImportSemWin32 = (PFN_vkImportSemaphoreWin32HandleKHR)
        vkGetDeviceProcAddr(vkDev, "vkImportSemaphoreWin32HandleKHR");
    auto fpWaitSemaphores = (PFN_vkWaitSemaphores)
        vkGetDeviceProcAddr(vkDev, "vkWaitSemaphores");

    // ===================================================================
    // 3. Import the shared texture as a Vulkan image (dedicated alloc).
    //    Layout doctrine (vulkan-samples / Sascha Willems external memory):
    //    create with initialLayout=UNDEFINED, then one barrier
    //    UNDEFINED->GENERAL with srcAccessMask=0. For imported D3D11 content
    //    the driver performs no layout conversion on this barrier; content
    //    validity is PROVEN by the pixel verification at the end.
    // ===================================================================
    VkImage image;
    VkDeviceMemory imageMem;
    VkDeviceSize imageMemSize = 0;
    {
        VkExternalMemoryImageCreateInfo extMem{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extMem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &extMem;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {W, H, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vkDev, &ici, nullptr, &image));

        // Dedicated allocation (the standard route for imported D3D11 images).
        VkMemoryDedicatedRequirements dedReq{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        req2.pNext = &dedReq;
        VkImageMemoryRequirementsInfo2 imri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        imri.image = image;
        vkGetImageMemoryRequirements2(vkDev, &imri, &req2);
        imageMemSize = req2.memoryRequirements.size;
        std::printf("[interop] image memory: size=%llu requiresDedicated=%d preferDedicated=%d -> using dedicated alloc\n",
                    (unsigned long long)imageMemSize,
                    (int)dedReq.requiresDedicatedAllocation, (int)dedReq.prefersDedicatedAllocation);

        VkMemoryDedicatedAllocateInfo ded{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = image;
        VkImportMemoryWin32HandleInfoKHR imp{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = sharedNT;
        imp.pNext = &ded;

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = imageMemSize;
        mai.memoryTypeIndex = UINT32_MAX;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if (req2.memoryRequirements.memoryTypeBits & (1u << i)) { mai.memoryTypeIndex = i; break; }
        if (mai.memoryTypeIndex == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no memory type\n"); return 1; }

        VK_CHECK(vkAllocateMemory(vkDev, &mai, nullptr, &imageMem));
        VK_CHECK(vkBindImageMemory(vkDev, image, imageMem, 0));
        std::printf("[interop] imported D3D11 image: memory bound (%llu bytes)\n",
                    (unsigned long long)imageMemSize);
    }
    const auto tImport = clk::now();

    // ---- sync: import the D3D11 fence as a Vulkan TIMELINE semaphore and
    //      wait for value 1 (the Signal from the CopyResource batch). The spec
    //      requires D3D11 fences to be imported as timeline semaphores.
    VkSemaphore fenceSem = VK_NULL_HANDLE;
    if (fenceNT && fenceSemSupported && fpImportSemWin32 && fpWaitSemaphores) {
        VkSemaphoreTypeCreateInfo t{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        t.initialValue = 0;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &t;
        if (vkCreateSemaphore(vkDev, &sci, nullptr, &fenceSem) == VK_SUCCESS) {
            VkImportSemaphoreWin32HandleInfoKHR imp{
                VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
            imp.semaphore = fenceSem;
            imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
            imp.handle = fenceNT;
            if (fpImportSemWin32(vkDev, &imp) != VK_SUCCESS) {
                vkDestroySemaphore(vkDev, fenceSem, nullptr);
                fenceSem = VK_NULL_HANDLE;
            }
        }
        if (fenceSem) {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &fenceSem;
            const uint64_t waitValue = 1;
            wi.pValues = &waitValue;
            VkResult wr = fpWaitSemaphores(vkDev, &wi, 5000000000ull); // 5s
            std::printf("[sync] Vulkan timeline wait on imported D3D11 fence: %s\n",
                        wr == VK_SUCCESS ? "SIGNALED (CopyResource complete on GPU)" : "TIMEOUT/FAIL");
            if (wr != VK_SUCCESS) { std::fprintf(stderr, "[FAIL] cross-API semaphore wait\n"); return 1; }
        }
    }
    if (!fenceSem) {
        if (fence11) {
            while (fence11->GetCompletedValue() < 1) Sleep(0);
            std::printf("[sync] fallback: D3D11 Flush + host wait on fence (GetCompletedValue=1)\n");
        } else {
            std::printf("[sync] fallback: D3D11 Flush only (no fence available)\n");
        }
    }

    // ===================================================================
    // 4. Compute pipeline: invert shader on the imported image.
    // ===================================================================
    VkShaderModule shader;
    {
        std::string spvPath;
        {
            char buf[MAX_PATH];
            GetModuleFileNameA(nullptr, buf, MAX_PATH);
            spvPath = buf;
            auto pos = spvPath.find_last_of('\\');
            if (pos != std::string::npos) spvPath.resize(pos + 1);
            spvPath += "passthrough.spv";
        }
        std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
        if (!f) { std::fprintf(stderr, "[FAIL] cannot open %s\n", spvPath.c_str()); return 1; }
        auto sz = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<char> code((size_t)sz);
        if (!f.read(code.data(), sz)) { std::fprintf(stderr, "[FAIL] read spv\n"); return 1; }
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = (size_t)sz;
        smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VK_CHECK(vkCreateShaderModule(vkDev, &smci, nullptr, &shader));
    }

    VkDescriptorSetLayout dsLayout;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(vkDev, &ci, nullptr, &dsLayout));
    }
    VkPipelineLayout pipeLayout;
    {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &dsLayout;
        VK_CHECK(vkCreatePipelineLayout(vkDev, &ci, nullptr, &pipeLayout));
    }
    VkPipeline pipeline;
    {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader;
        ci.stage.pName = "main";
        ci.layout = pipeLayout;
        VK_CHECK(vkCreateComputePipelines(vkDev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
    }
    VkDescriptorPool dpool;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 1;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &ps;
        VK_CHECK(vkCreateDescriptorPool(vkDev, &ci, nullptr, &dpool));
    }
    VkDescriptorSet dset;
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsLayout;
        VK_CHECK(vkAllocateDescriptorSets(vkDev, &ai, &dset));
        VkDescriptorImageInfo ii{};
        ii.imageView = VK_NULL_HANDLE; // filled below
        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkImageView view;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vkDev, &vci, nullptr, &view));
        ii.imageView = view;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = dset;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(vkDev, 1, &w, 0, nullptr);
    }

    // Host-visible readback buffer.
    VkBuffer outBuf;
    VkDeviceMemory outMem;
    void* outPtr = nullptr;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = FRAME_BYTES;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vkDev, &bci, nullptr, &outBuf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vkDev, outBuf, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i))) continue;
            auto& mt = mp.memoryTypes[i];
            if ((mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { idx = i; break; }
        }
        if (idx == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no host-visible coherent memory\n"); return 1; }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = idx;
        VK_CHECK(vkAllocateMemory(vkDev, &mai, nullptr, &outMem));
        VK_CHECK(vkBindBufferMemory(vkDev, outBuf, outMem, 0));
        VK_CHECK(vkMapMemory(vkDev, outMem, 0, FRAME_BYTES, 0, &outPtr));
    }

    VkCommandPool cmdPool;
    VkCommandBuffer cmd;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = qfCompute;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(vkDev, &ci, nullptr, &cmdPool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(vkDev, &ai, &cmd));
    }

    const auto tPipelineReady = clk::now();

    // Record: barrier UNDEFINED->GENERAL, dispatch invert, barrier, copy out.
    VkFence doneFence;
    {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(vkDev, &fci, nullptr, &doneFence));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

        VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.image = image;
        imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // imported D3D11 content:
        imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;     // vulkan-samples doctrine;
        imb.srcAccessMask = 0;                       // no Vulkan-side prior access
        imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &imb);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout,
                                0, 1, &dset, 0, nullptr);
        vkCmdDispatch(cmd, W / 16, H / 16, 1);

        imb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &imb);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, outBuf, 1, &region);

        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = outBuf;
        bmb.size = FRAME_BYTES;
        bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bmb, 0, nullptr);

        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(vkQueue, 1, &si, doneFence));
        VK_CHECK(vkWaitForFences(vkDev, 1, &doneFence, VK_TRUE, UINT64_MAX));
    }
    const auto tCopyBack = clk::now();

    // ===================================================================
    // 5. Save BMPs + 6. verify inverted == 255 - original per channel.
    // ===================================================================
    std::vector<uint8_t> inverted(W * H * 4);
    std::memcpy(inverted.data(), outPtr, FRAME_BYTES);

    std::string origPath = outDir + "\\m2_original.bmp";
    std::string invPath = outDir + "\\m2_inverted.bmp";
    if (!WriteBmpBGRA(origPath, original, W, H)) { std::fprintf(stderr, "[FAIL] write %s\n", origPath.c_str()); return 1; }
    if (!WriteBmpBGRA(invPath, inverted, W, H)) { std::fprintf(stderr, "[FAIL] write %s\n", invPath.c_str()); return 1; }
    std::printf("[out] %s\n[out] %s\n", origPath.c_str(), invPath.c_str());

    // Verify from the files on disk (full round-trip including BMP IO).
    std::vector<uint8_t> fOrig, fInv;
    uint32_t ow, oh, iw, ih;
    if (!LoadBmpBGRA(origPath, fOrig, ow, oh) || !LoadBmpBGRA(invPath, fInv, iw, ih)) {
        std::fprintf(stderr, "[FAIL] BMP reload\n");
        return 1;
    }
    if (ow != W || oh != H || iw != W || ih != H) {
        std::fprintf(stderr, "[FAIL] BMP dims mismatch\n");
        return 1;
    }

    uint64_t rgbMatch[3] = {0, 0, 0}, alphaMatch = 0;
    uint64_t rgbTotal = (uint64_t)W * H;
    int firstBad = -1;
    for (uint64_t p = 0; p < rgbTotal; ++p) {
        for (int ch = 0; ch < 3; ++ch) {
            uint8_t o = fOrig[p * 4 + ch];
            uint8_t v = fInv[p * 4 + ch];
            if (v == (uint8_t)(255 - o)) ++rgbMatch[ch];
            else if (firstBad < 0) firstBad = (int)(p * 4 + ch);
        }
        if (fInv[p * 4 + 3] == fOrig[p * 4 + 3]) ++alphaMatch;
    }
    double pct[3];
    for (int ch = 0; ch < 3; ++ch) pct[ch] = 100.0 * (double)rgbMatch[ch] / (double)rgbTotal;
    double aPct = 100.0 * (double)alphaMatch / (double)rgbTotal;

    const auto tTotal = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    std::printf("\n=== M2 verify ===\n");
    std::printf("inverted == 255-original: B=%.4f%% G=%.4f%% R=%.4f%% (must be 100)\n",
                pct[0], pct[1], pct[2]);
    std::printf("alpha preserved (not part of pass criteria): %.4f%%\n", aPct);
    if (firstBad >= 0) {
        size_t pix = (size_t)firstBad / 4;
        std::printf("first mismatch at pixel %zu chan %d: orig=%u inv=%u\n",
                    pix, firstBad % 4, fOrig[firstBad], fInv[firstBad]);
    }
    std::printf("\n=== M2 timing (ms) ===\n");
    std::printf("acquire->D3D11 ready (incl. wiggle + copy + signal/flush): %.3f\n",
                ms(tAcquireBegin, tD3DReady));
    std::printf("import (vk image+mem+bind, semaphore import+wait):            %.3f\n",
                ms(tD3DReady, tImport));
    std::printf("pipeline setup + dispatch + copyback (record+submit+wait):    %.3f\n",
                ms(tImport, tCopyBack));
    std::printf("verify:                                                       %.3f\n",
                ms(tCopyBack, tTotal));
    std::printf("TOTAL:                                                        %.3f\n",
                ms(tStart, tTotal));

    bool pass = (pct[0] == 100.0 && pct[1] == 100.0 && pct[2] == 100.0);

    // ---- teardown (best effort)
    vkUnmapMemory(vkDev, outMem);
    vkDestroyFence(vkDev, doneFence, nullptr);
    vkDestroyBuffer(vkDev, outBuf, nullptr);
    vkFreeMemory(vkDev, outMem, nullptr);
    vkDestroyCommandPool(vkDev, cmdPool, nullptr);
    vkDestroyPipeline(vkDev, pipeline, nullptr);
    vkDestroyPipelineLayout(vkDev, pipeLayout, nullptr);
    vkDestroyDescriptorPool(vkDev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(vkDev, dsLayout, nullptr);
    vkDestroyShaderModule(vkDev, shader, nullptr);
    vkDestroyImage(vkDev, image, nullptr);
    vkFreeMemory(vkDev, imageMem, nullptr);
    if (fenceSem) vkDestroySemaphore(vkDev, fenceSem, nullptr);
    vkDestroyDevice(vkDev, nullptr);
    vkDestroyInstance(inst, nullptr);
    if (sharedNT) CloseHandle(sharedNT);
    if (fenceNT) CloseHandle(fenceNT);

    std::printf("\n=== M2 result: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
