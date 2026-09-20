// ============================================================================
// M3 neural pass slice — DLSS5_INTEL
//
// Capture ONE desktop frame via the M2 path (DDA -> D3D11 shared texture ->
// Vulkan import, D3D11-fence/timeline-semaphore sync), then run a PORTED slice
// of the reference dlss-nr-on-intel pipeline on the Arc Pro B50 GPU:
//
//   captured BGRA8
//     -> decode          (port of nr_decode8,   ref src/ref/nr_image.c:36-44)
//     -> letterbox scan  (port of active_region, ref src/layer/nr_daemon.py:261-296;
//                         GPU max-reduction + host decision)
//     -> rescale down    (port of resize/axis,   ref src/layer/nr_daemon.py:238-255)
//     -> feature volume  (port of nr_features,   ref src/ref/nr_image.c:84-112;
//                         16 fp32 channels, scaled_color, first-frame history,
//                         controls; noise ch0-2 = documented STAND-IN)
//     -> learned block   (*** STAND-IN *** — no weights exist in the reference
//                         repo; deterministic Laplacian+vendor-gate head through
//                         the verbatim-ported publish.glsl rounding chain)
//     -> rescale up      (head back to output extent)
//     -> compose         (port of nr_compose,    ref src/ref/nr_image.c:53-76,
//                         intensity=1: final = native + (nr_out - nr_in))
//     -> encode          (port of nr_encode8,    ref src/ref/nr_image.c:46-63)
//
// Saves out\m3_final.bmp (full frame) + out\m3_nr_out.bmp (region), reports
// per-channel mean|final-native| + spatial-structure metrics (delta must be
// > 0 and non-uniform), per-stage ms, and a steady-state single-submit timing.
//
// Exit codes: 0 = metrics pass (delta > 0, structured), 1 = any failure.
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
// active_region() ported from reference src/layer/nr_daemon.py:261-296.
// A bar is a run of rows/columns whose brightest channel <= tolerance
// everywhere; the scan caps at 45% of the extent, bars must be symmetric
// within 1 row, and the active area must be >= half the frame (else it is a
// dark frame, not a letterbox). Returns rows/cols with bottom/right exclusive.
struct Region { int top, bottom, left, right; };
static Region ActiveRegion(const float* rowMax, const float* colMax, int height, int width) {
    const float tolerance = 2.0f / 255.0f;   // LETTERBOX_TOLERANCE, nr_daemon.py:258
    const double limit = 0.45;               // nr_daemon.py:271
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

// Network extent contract (reference docs/ARCHITECTURE.md:35-43): extent >= 320
// and a multiple of 64 per axis; smaller/misaligned frames are mirror-padded and
// cropped back. NetworkGeometry.vendor_aligned lives in the vendored
// work/mlx-dlss features.py (ABSENT from this clone), so we round to the NEAREST
// multiple of 64 with a 320 floor and log the choice; with an exactly-aligned
// result the geometry crop back to the resampled extent is the identity.
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
// VK_CHECK variant for helpers that return Vulkan handles: fail hard instead.
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
        // fall back to HOST_VISIBLE|COHERENT when DEVICE_LOCAL unavailable
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

int main(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();

    std::string outDir = "out";
    int wiggle = 1;
    float renderScale = 0.55f;   // the reference's live starting point (M3 plan)
    if (argc > 1) outDir = argv[1];
    if (argc > 2) wiggle = std::atoi(argv[2]);
    if (argc > 3) renderScale = (float)std::atof(argv[3]);
    if (argc > 4) { std::fprintf(stderr, "usage: m3neural [outdir] [wiggle 0/1] [render_scale]\n"); return 1; }

    std::printf("=== M3: neural pass slice (reference ports + STAND-IN learned block) ===\n");
    std::printf("frame target: %ux%u B8G8R8A8, render_scale=%.2f\n\n", W, H, renderScale);
    CreateDirectoryA(outDir.c_str(), nullptr);

    // ===================================================================
    // 1. D3D11 hardware device + DDA capture of one fresh frame (M2 path).
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
    }
    ComPtr<IDXGIOutputDuplication> dup;
    {
        ComPtr<IDXGIOutput1> out1;
        HR_CHECK(output->QueryInterface(IID_PPV_ARGS(&out1)));
        HRESULT hr = out1->DuplicateOutput(device.Get(), &dup); // M1: DuplicateOutput1 rejected by driver
        if (FAILED(hr)) { std::fprintf(stderr, "[FAIL] DuplicateOutput: %s\n", HrName(hr)); return 1; }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        std::printf("[DDA] DuplicateOutput active: %ux%u format=%d rotation=%d\n",
                    (unsigned)dd.ModeDesc.Width, (unsigned)dd.ModeDesc.Height,
                    (int)dd.ModeDesc.Format, (int)dd.Rotation);
        if (dd.ModeDesc.Width != W || dd.ModeDesc.Height != H ||
            dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            std::fprintf(stderr, "[FAIL] expected %ux%u B8G8R8A8\n", W, H); return 1;
        }
    }
    ComPtr<ID3D11Texture2D> sharedTex;
    D3D11_TEXTURE2D_DESC sharedDesc{};
    {
        sharedDesc.Width = W; sharedDesc.Height = H; sharedDesc.MipLevels = 1; sharedDesc.ArraySize = 1;
        sharedDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sharedDesc.SampleDesc = {1, 0};
        sharedDesc.Usage = D3D11_USAGE_DEFAULT;
        sharedDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HR_CHECK(device->CreateTexture2D(&sharedDesc, nullptr, &sharedTex));
    }
    ComPtr<ID3D11Texture2D> stagingTex;
    {
        D3D11_TEXTURE2D_DESC sd = sharedDesc;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        HR_CHECK(device->CreateTexture2D(&sd, nullptr, &stagingTex));
    }
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence11;
    HANDLE fenceNT = nullptr;
    {
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)))) {
            if (FAILED(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceNT))) {
                fence11.Reset(); fenceNT = nullptr;
            }
        }
        std::printf("[sync] D3D11.5 fence: %s\n", fenceNT ? "available (NT handle)" : "UNAVAILABLE - host-wait fallback");
    }

    const auto tAcquireBegin = clk::now();
    bool gotFrame = false;
    std::thread wiggler;
    if (wiggle) {
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
        std::printf("[info] cursor-wiggle activity generator ON (DDA is dirty-rect driven)\n");
    }
    {
        // The first frame(s) after DuplicateOutput are a black warm-up surface on
        // this Arc driver (m1dda snapshot_1 = luma var 0; content from #2 onward),
        // and grabbing immediately can outrun DWM's first real present. So: keep
        // acquiring and content-check the staging copy until a non-black frame
        // arrives; after ~6s fall back to accepting whatever we get.
        auto deadline = tAcquireBegin + std::chrono::seconds(8);
        auto acceptAnyAt = tAcquireBegin + std::chrono::seconds(6);
        int tries = 0, acquired = 0, skippedBlack = 0;
        while (clk::now() < deadline && !gotFrame) {
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
            context->CopyResource(sharedTex.Get(), frameTex.Get());
            context->CopyResource(stagingTex.Get(), frameTex.Get());
            context->Flush();
            dup->ReleaseFrame();
            // content check on the staging copy (cheap sparse luma sample)
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
            if (context4 && fence11) context4->Signal(fence11.Get(), 1);
            context->Flush();
            gotFrame = true;
            std::printf("[DDA] acquired %s frame on try %d (acquired#%d, skippedBlack=%d, accumulated=%llu)\n",
                        black ? "FALLBACK-black" : "content", tries, acquired, skippedBlack,
                        (unsigned long long)fi.AccumulatedFrames);
        }
    }
    if (wiggler.joinable()) { g_wiggleStop.store(true); wiggler.join(); }
    if (!gotFrame) { std::fprintf(stderr, "[FAIL] no DDA frame within ~5s\n"); return 1; }
    const auto tD3DReady = clk::now();

    std::vector<uint8_t> original(W * H * 4);
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        HR_CHECK(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &ms));
        for (uint32_t y = 0; y < H; ++y)
            std::memcpy(&original[(size_t)y * W * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, W * 4);
        context->Unmap(stagingTex.Get(), 0);
        if (!fenceNT) while (fence11 && fence11->GetCompletedValue() < 1) Sleep(0);
        // diagnostic artifact: the raw captured native frame (host path, no Vulkan).
        WriteBmpBGRA(outDir + "\\m3_native.bmp", original, W, H);
        {
            double s = 0; for (size_t i = 0; i < original.size(); i += 4003) s += original[i];
            std::printf("[cap] native sample mean=%.2f (0 => capture black)\n",
                        s / (double)(original.size() / 4003 + 1));
        }
    }
    HANDLE sharedNT = nullptr;
    {
        ComPtr<IDXGIResource1> res1;
        HR_CHECK(sharedTex->QueryInterface(IID_PPV_ARGS(&res1)));
        HR_CHECK(res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedNT));
    }

    // ===================================================================
    // 2. Vulkan: instance + LUID-matched device (M2 path).
    // ===================================================================
    VkCtx c{};
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "m3-neural";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
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
            if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { c.qf = i; break; }
        if (c.qf == UINT32_MAX) { std::fprintf(stderr, "[FAIL] no compute queue\n"); return 1; }
        vkGetPhysicalDeviceMemoryProperties(c.pd, &c.memProps);

        // Discrete memory policy log (analysis-dlss-nr-on-intel.md sec.6.2,
        // ref libxmx.c want_unmapped()/memtype():137-230): discrete -> device-
        // local UNMAPPED intermediates, HOST_CACHED-leaning readback. We follow
        // it (CreateBuf wants DEVICE_LOCAL; readback buffers want
        // HOST_VISIBLE|COHERENT with CACHED preferred) and log the heaps.
        std::printf("[mem] heaps (discrete memory policy):\n");
        for (uint32_t i = 0; i < c.memProps.memoryHeapCount; ++i) {
            auto& h = c.memProps.memoryHeaps[i];
            bool hv = false;
            for (uint32_t t = 0; t < c.memProps.memoryTypeCount; ++t)
                if (c.memProps.memoryTypes[t].heapIndex == i &&
                    (c.memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) hv = true;
            std::printf("[mem]   heap%u: %.2f GB deviceLocal=%d hasHostVisibleType=%d\n", i,
                        (double)h.size / (1 << 30),
                        (int)((h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0), (int)hv);
        }
    }
    {
        const char* want[] = {
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        };
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = c.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = want;
        VK_CHECK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev));
        vkGetDeviceQueue(c.dev, c.qf, 0, &c.queue);
        std::printf("[VK] logical device up (compute family %u)\n", c.qf);
    }
    auto fpImportSemWin32 = (PFN_vkImportSemaphoreWin32HandleKHR)
        vkGetDeviceProcAddr(c.dev, "vkImportSemaphoreWin32HandleKHR");
    auto fpWaitSemaphores = (PFN_vkWaitSemaphores)
        vkGetDeviceProcAddr(c.dev, "vkWaitSemaphores");

    // ---- import the shared texture (dedicated alloc; M2 doctrine).
    VkImage imgIn = VK_NULL_HANDLE;
    VkDeviceMemory imgInMem = VK_NULL_HANDLE;
    VkImageView imgInView = VK_NULL_HANDLE;
    {
        VkExternalMemoryImageCreateInfo extMem{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extMem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &extMem;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {W, H, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &imgIn));
        VkMemoryDedicatedRequirements dedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        req2.pNext = &dedReq;
        VkImageMemoryRequirementsInfo2 imri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        imri.image = imgIn;
        vkGetImageMemoryRequirements2(c.dev, &imri, &req2);
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = imgIn;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = sharedNT;
        imp.pNext = &ded;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = req2.memoryRequirements.size;
        mai.memoryTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i)
            if (req2.memoryRequirements.memoryTypeBits & (1u << i)) { mai.memoryTypeIndex = i; break; }
        VK_CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &imgInMem));
        VK_CHECK(vkBindImageMemory(c.dev, imgIn, imgInMem, 0));
        imgInView = CreateView(c, imgIn, VK_FORMAT_B8G8R8A8_UNORM);
        std::printf("[interop] imported D3D11 image (%llu bytes, dedicated)\n",
                    (unsigned long long)req2.memoryRequirements.size);
    }
    const auto tImport = clk::now();

    // ---- cross-API sync: D3D11 fence -> Vulkan timeline semaphore, wait value 1
    {
        VkSemaphore fenceSem = VK_NULL_HANDLE;
        bool ok = fenceNT && fpImportSemWin32 && fpWaitSemaphores;
        if (ok) {
            VkSemaphoreTypeCreateInfo t{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; t.initialValue = 0;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            sci.pNext = &t;
            ok = vkCreateSemaphore(c.dev, &sci, nullptr, &fenceSem) == VK_SUCCESS;
            if (ok) {
                VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
                imp.semaphore = fenceSem;
                imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
                imp.handle = fenceNT;
                ok = fpImportSemWin32(c.dev, &imp) == VK_SUCCESS;
            }
            if (ok) {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1; wi.pSemaphores = &fenceSem;
                const uint64_t waitValue = 1; wi.pValues = &waitValue;
                ok = fpWaitSemaphores(c.dev, &wi, 5000000000ull) == VK_SUCCESS;
            }
            if (fenceSem) vkDestroySemaphore(c.dev, fenceSem, nullptr);
        }
        if (!ok && fence11) while (fence11->GetCompletedValue() < 1) Sleep(0);
        std::printf("[sync] cross-API ordering: %s\n", ok ? "timeline semaphore SIGNALED" :
                    (fence11 ? "host fence wait (fallback)" : "Flush only"));
    }

    // ===================================================================
    // 3. Shared pipeline layout + 7 pipelines.
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
    std::printf("[VK] 7 compute pipelines up\n");

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
    // 4. Wave-1 buffers + decode + letterbox.
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

    // descriptor pool + 2 sets: setFinal (imgOut=final image), setNrOut.
    // Buffer bindings are identical in both; image binding 9 differs.
    VkDescriptorPool dpool;
    VkDescriptorSet setFinal, setNrOut;
    {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 20}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(c.dev, &dpci, nullptr, &dpool));
        VkDescriptorSetLayout ls[2] = {dsLayout, dsLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool; ai.descriptorSetCount = 2; ai.pSetLayouts = ls;
        VkDescriptorSet sets[2];
        VK_CHECK(vkAllocateDescriptorSets(c.dev, &ai, sets));
        setFinal = sets[0]; setNrOut = sets[1];
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
    writeImg(setNrOut, 0, imgInView);
    writeBuf(setFinal, 1, bufRgb, (uint64_t)W * H * 3 * 4);
    writeBuf(setNrOut, 1, bufRgb, (uint64_t)W * H * 3 * 4);
    writeBuf(setFinal, 8, bufMax, 4096 * 4);
    writeBuf(setNrOut, 8, bufMax, 4096 * 4);

    double msDecode, msLetter;
    {
        begin();
        VkImageMemoryBarrier imb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.image = imgIn; imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imb.srcAccessMask = 0; imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        vkCmdDispatch(cmd, W / 16, H / 16, 1);
        barrierAll();
        VK_CHECK(vkEndCommandBuffer(cmd));
        msDecode = submitAndWait();
    }
    std::printf("[stage] decode (BGRA8 -> RGB f32 [0,1], bgra=1, nr_image.c:36-44): %.3f ms\n", msDecode);

    Region region{};
    {
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLetter);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = (int32_t)W; p.a[1] = (int32_t)H;
        p.b[0] = 0; push(p);
        vkCmdDispatch(cmd, (H + 255) / 256, 1, 1);
        barrierAll();
        p.b[0] = 1; push(p);
        vkCmdDispatch(cmd, (W + 255) / 256, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msLetter = submitAndWait();
    }
    {
        const float* m = (const float*)maxPtr;
        region = ActiveRegion(m, m + 2048, (int)H, (int)W);
        std::printf("[stage] letterbox: GPU row/col max + host active_region (nr_daemon.py:261-296): %.3f ms\n", msLetter);
        std::printf("[letterbox] region rows [%d,%d) cols [%d,%d) of %ux%u (tol 2/255, cap 45%%, sym 1, area>=half)\n",
                    region.top, region.bottom, region.left, region.right, W, H);
    }

    const int regionW = region.right - region.left;
    const int regionH = region.bottom - region.top;
    const int netW = Align64(regionW * renderScale);
    const int netH = Align64(regionH * renderScale);
    std::printf("[geometry] region %dx%d -> network extent %dx%d (x%.2f, nearest 64, min 320; crop-back identity)\n",
                regionW, regionH, netW, netH, renderScale);
    std::printf("[controls] profile=standard: ch10..14 = half(0),half(1),half(1),-1,-1 (style,tone,structure; vendor default structure=1.5)\n");

    // ===================================================================
    // 5. Wave-2 buffers + output images + full descriptor writes.
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
    for (VkDescriptorSet s : {setFinal, setNrOut}) {
        writeBuf(s, 2, bufT, Tfloats * 4);
        writeBuf(s, 3, bufFeat, (uint64_t)netW * netH * 16 * 4);
        writeBuf(s, 4, bufHead, (uint64_t)netW * netH * 4 * 4);
        writeBuf(s, 5, bufHeadUp, (uint64_t)regionW * regionH * 4 * 4);
        writeBuf(s, 6, bufFin, (uint64_t)regionW * regionH * 3 * 4);
        writeBuf(s, 7, bufNr, (uint64_t)regionW * regionH * 3 * 4);
    }
    writeImg(setFinal, 9, viewFinal);
    writeImg(setNrOut, 9, viewNr);
    std::printf("[mem] intermediates device-local (discrete policy); readback host-coherent\n");

    // ===================================================================
    // 6. GPU neural-slice stages, timed individually (serialize: submit+wait
    //    per stage, so times include no overlap — noted in the log).
    // ===================================================================
    const uint32_t regionOff = (uint32_t)(region.top * (int)W + region.left);

    double msDown, msFeat, msStandin, msUp, msCompose, msEncode, msReadback;

    {   // rescale down: v (rgbFull region -> T), h (T -> netRGB in bufHead slot)
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = (int32_t)W; p.a[1] = (int32_t)regionOff; p.a[2] = regionH; p.a[3] = regionW;
        p.b[0] = netH; p.b[1] = regionW; p.b[2] = 0; p.b[3] = 3;      // vertical
        p.d[0] = 1; p.d[1] = 2;                                       // src bufRgb -> dst bufT
        push(p);
        vkCmdDispatch(cmd, (regionW + 15) / 16, (netH + 15) / 16, 1);
        barrierAll();
        p.a[0] = regionW; p.a[1] = 0; p.a[2] = regionW; p.a[3] = netH;
        p.b[0] = netW; p.b[1] = netW; p.b[2] = 1; p.b[3] = 3;       // horizontal
        p.d[0] = 2; p.d[1] = 4;                                       // src bufT -> dst bufHead(netRGB)
        push(p);
        vkCmdDispatch(cmd, (netW + 15) / 16, (netH + 15) / 16, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msDown = submitAndWait();
    }
    std::printf("[stage] rescale down %dx%d -> %dx%d (nr_daemon.py:238-255): %.3f ms\n",
                regionW, regionH, netW, netH, msDown);

    {   // features: netRGB -> 16ch volume
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeFeatures);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = netW; p.a[1] = netH;
        p.b[0] = 0;                                                   // frame index
        p.c[0] = 0.0f; p.c[1] = 1.0f; p.c[2] = 1.0f;                  // style,tone,structure
        push(p);
        vkCmdDispatch(cmd, ((uint64_t)netW * netH + 255) / 256, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msFeat = submitAndWait();
    }
    std::printf("[stage] features 16ch (nr_image.c:84-112; noise ch0-2 = STAND-IN hash): %.3f ms\n", msFeat);

    {   // STAND-IN learned block: features -> head (4ch)
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeStandin);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = netW; p.a[1] = netH;
        push(p);
        vkCmdDispatch(cmd, ((uint64_t)netW * netH + 255) / 256, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msStandin = submitAndWait();
    }
    std::printf("[stage] STAND-IN learned block (Laplacian + vendor gate, publish.glsl chain): %.3f ms\n", msStandin);

    {   // rescale up: v (head -> T), h (T -> headUp), 4 channels
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeRescale);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = netW; p.a[1] = 0; p.a[2] = netH; p.a[3] = netW;
        p.b[0] = regionH; p.b[1] = netW; p.b[2] = 0; p.b[3] = 4;      // vertical
        p.d[0] = 4; p.d[1] = 2;                                       // src bufHead -> dst bufT
        push(p);
        vkCmdDispatch(cmd, (netW + 15) / 16, (regionH + 15) / 16, 1);
        barrierAll();
        p.a[0] = netW; p.a[1] = 0; p.a[2] = netW; p.a[3] = regionH;
        p.b[0] = regionW; p.b[1] = regionW; p.b[2] = 1; p.b[3] = 4;   // horizontal
        p.d[0] = 2; p.d[1] = 5;                                       // src bufT -> dst bufHeadUp
        push(p);
        vkCmdDispatch(cmd, (regionW + 15) / 16, (regionH + 15) / 16, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msUp = submitAndWait();
    }
    std::printf("[stage] rescale up head %dx%d -> %dx%d: %.3f ms\n", netW, netH, regionW, regionH, msUp);

    {   // compose: residual composite final = native + (nr_out - nr_in)
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeCompose);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = (int32_t)W; p.a[1] = region.top; p.a[2] = region.left;
        p.b[0] = regionW; p.b[1] = regionH;
        push(p);
        vkCmdDispatch(cmd, ((uint64_t)regionW * regionH + 255) / 256, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msCompose = submitAndWait();
    }
    std::printf("[stage] compose residual (nr_image.c:53-76, blend=1): %.3f ms\n", msCompose);

    {   // encode: final full-frame image + nr_out region image
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEncode);
        // final
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
        p.a[0] = (int32_t)W; p.a[1] = (int32_t)H; p.a[2] = region.left; p.a[3] = region.top;
        p.b[0] = regionW; p.b[1] = regionH; p.b[2] = 0;
        push(p);
        vkCmdDispatch(cmd, (W + 15) / 16, (H + 15) / 16, 1);
        barrierAll();
        // nr_out
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setNrOut, 0, nullptr);
        p.a[0] = 0; p.a[1] = 0; p.a[2] = 0; p.a[3] = 0;
        p.b[0] = regionW; p.b[1] = regionH; p.b[2] = 1;
        push(p);
        vkCmdDispatch(cmd, (regionW + 15) / 16, (regionH + 15) / 16, 1);
        // transfer layout for both outputs
        VkImageMemoryBarrier imb[2]{};
        for (int i = 0; i < 2; ++i) {
            imb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imb[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb[i].image = i == 0 ? imgFinal : imgNr;
            imb[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            imb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            imb[i].srcAccessMask = 0;
            imb[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, imb);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, imgFinal, VK_IMAGE_LAYOUT_GENERAL, bufRbFinal, 1, &rgn);
        VkBufferImageCopy rgn2{};
        rgn2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn2.imageExtent = {(uint32_t)regionW, (uint32_t)regionH, 1};
        vkCmdCopyImageToBuffer(cmd, imgNr, VK_IMAGE_LAYOUT_GENERAL, bufRbNr, 1, &rgn2);
        VK_CHECK(vkEndCommandBuffer(cmd));
        msEncode = submitAndWait();
    }
    std::printf("[stage] encode 2x (nr_encode8, nr_image.c:46-63) + image->buffer: %.3f ms\n", msEncode);

    const auto tGpuDone = clk::now();

    // ===================================================================
    // 7. Steady state: record the WHOLE GPU pipeline in one submit, 3 runs.
    //    (Input is the same captured frame; this measures pipeline cost once
    //    warm, excluding capture/import/readback-to-host.)
    // ===================================================================
    double steady[3];
    for (int iter = 0; iter < 3; ++iter) {
        begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeDecode);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setFinal, 0, nullptr);
        Push p{};
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
        p.a[0] = netW; p.a[1] = netH; p.b[0] = 0; p.c[0] = 0.0f; p.c[1] = 1.0f; p.c[2] = 1.0f; push(p);
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
        p.b[2] = 1; push(p);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &setNrOut, 0, nullptr);
        vkCmdDispatch(cmd, (regionW + 15) / 16, (regionH + 15) / 16, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        steady[iter] = submitAndWait();
    }
    std::sort(steady, steady + 3);
    std::printf("[timing] steady-state full GPU pipeline (decode..encode, one submit): "
                "%.3f / %.3f / %.3f ms (min/median of 3)\n", steady[0], steady[1], steady[2]);

    // ===================================================================
    // 8. Readback, BMPs, metrics.
    // ===================================================================
    std::vector<uint8_t> finalPx(W * H * 4), nrPx((uint64_t)regionW * regionH * 4);
    {
        void* p1 = nullptr; void* p2 = nullptr;
        VK_CHECK(vkMapMemory(c.dev, memRbFinal, 0, (uint64_t)W * H * 4, 0, &p1));
        VK_CHECK(vkMapMemory(c.dev, memRbNr, 0, (uint64_t)regionW * regionH * 4, 0, &p2));
        std::memcpy(finalPx.data(), p1, (uint64_t)W * H * 4);
        std::memcpy(nrPx.data(), p2, (uint64_t)regionW * regionH * 4);
        vkUnmapMemory(c.dev, memRbFinal);
        vkUnmapMemory(c.dev, memRbNr);
    }
    const auto tReadback = clk::now();
    msReadback = std::chrono::duration<double, std::milli>(tReadback - tGpuDone).count();

    std::string finalPath = outDir + "\\m3_final.bmp";
    std::string nrPath = outDir + "\\m3_nr_out.bmp";
    if (!WriteBmpBGRA(finalPath, finalPx, W, H)) { std::fprintf(stderr, "[FAIL] write %s\n", finalPath.c_str()); return 1; }
    if (!WriteBmpBGRA(nrPath, nrPx, regionW, regionH)) { std::fprintf(stderr, "[FAIL] write %s\n", nrPath.c_str()); return 1; }
    std::printf("[out] %s\n[out] %s\n", finalPath.c_str(), nrPath.c_str());

    // ---- metrics over the region, on 8-bit values (final vs native original).
    double meanAbs[3] = {0, 0, 0}, maxAbs = 0, sumSq = 0;
    double gradDx[3] = {0, 0, 0};       // forward-difference gradient magnitude of delta
    uint64_t changed = 0, total = (uint64_t)regionW * regionH;
    // BGR order in memory for both BMPs; compare bytes directly.
    for (int y = 0; y < regionH; ++y) {
        for (int x = 0; x < regionW; ++x) {
            size_t fullP = (size_t)(y + region.top) * W + (x + region.left);
            size_t regP = (size_t)y * regionW + x;
            int d[3];
            for (int ch = 0; ch < 3; ++ch) {
                int f = finalPx[fullP * 4 + ch];
                int n = original[fullP * 4 + ch];
                d[ch] = f - n;
                meanAbs[ch] += std::abs(d[ch]);
                sumSq += (double)d[ch] * d[ch];
                if (std::abs(d[ch]) > maxAbs) maxAbs = std::abs(d[ch]);
            }
            if (d[0] || d[1] || d[2]) ++changed;
            // gradient of delta (right & down neighbours, clipped)
            int x2 = x < regionW - 1 ? x + 1 : x;
            int y2 = y < regionH - 1 ? y + 1 : y;
            size_t fullX = (size_t)(y + region.top) * W + (x2 + region.left);
            size_t fullY = (size_t)(y2 + region.top) * W + (x + region.left);
            for (int ch = 0; ch < 3; ++ch) {
                double gx = (double)(finalPx[fullX * 4 + ch] - original[fullX * 4 + ch]) - d[ch];
                double gy = (double)(finalPx[fullY * 4 + ch] - original[fullY * 4 + ch]) - d[ch];
                gradDx[ch] += std::sqrt(gx * gx + gy * gy);
            }
        }
    }
    for (int ch = 0; ch < 3; ++ch) {
        meanAbs[ch] /= (double)total;
        gradDx[ch] /= (double)total;
    }
    double deltaStd = std::sqrt(sumSq / (3.0 * (double)total));
    double meanGrad = (gradDx[0] + gradDx[1] + gradDx[2]) / 3.0;

    std::printf("\n=== M3 metrics (region %dx%d, 8-bit, final vs native) ===\n", regionW, regionH);
    std::printf("per-channel mean|final-native|: B=%.5f G=%.5f R=%.5f (0-255 scale; must be > 0)\n",
                meanAbs[0], meanAbs[1], meanAbs[2]);
    std::printf("max|delta| = %.0f levels; delta std = %.5f; pixels changed = %.2f%%\n",
                maxAbs, deltaStd, 100.0 * (double)changed / (double)total);
    std::printf("spatial structure: mean |grad(delta)| per ch = B=%.5f G=%.5f R=%.5f "
                "(> 0 => delta is NOT a uniform/global shift)\n",
                gradDx[0], gradDx[1], gradDx[2]);

    // ---- full timing summary
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("\n=== M3 timing (ms; per-stage serialized submit+wait) ===\n");
    std::printf("acquire->D3D11 ready:  %.3f\n", ms(tAcquireBegin, tD3DReady));
    std::printf("import + cross-API sync: %.3f\n", ms(tD3DReady, tImport));
    std::printf("decode:                %.3f\n", msDecode);
    std::printf("letterbox:             %.3f\n", msLetter);
    std::printf("rescale down:          %.3f\n", msDown);
    std::printf("features:              %.3f\n", msFeat);
    std::printf("STAND-IN block:        %.3f\n", msStandin);
    std::printf("rescale up:            %.3f\n", msUp);
    std::printf("compose:               %.3f\n", msCompose);
    std::printf("encode x2 + copies:    %.3f\n", msEncode);
    std::printf("host readback (maps):  %.3f\n", msReadback);
    std::printf("steady-state GPU pipeline (median): %.3f\n", steady[1]);
    std::printf("TOTAL wall:            %.3f\n", ms(tStart, clk::now()));

    bool pass = (meanAbs[0] > 0.0 && meanAbs[1] > 0.0 && meanAbs[2] > 0.0 &&
                 meanGrad > 0.0 && changed > 0);
    std::printf("\n=== M3 result: %s ===\n", pass ? "PASS" : "FAIL");

    // ---- teardown (best effort)
    vkUnmapMemory(c.dev, memMax);
    vkDestroyFence(c.dev, fence, nullptr);
    vkDestroyCommandPool(c.dev, cmdPool, nullptr);
    for (VkPipeline p : {pipeDecode, pipeLetter, pipeRescale, pipeFeatures, pipeStandin, pipeCompose, pipeEncode})
        vkDestroyPipeline(c.dev, p, nullptr);
    vkDestroyPipelineLayout(c.dev, pipeLayout, nullptr);
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsLayout, nullptr);
    for (VkBuffer b : {bufRgb, bufMax, bufT, bufFeat, bufHead, bufHeadUp, bufFin, bufNr, bufRbFinal, bufRbNr})
        vkDestroyBuffer(c.dev, b, nullptr);
    for (VkDeviceMemory m : {memRgb, memMax, memT, memFeat, memHead, memHeadUp, memFin, memNr,
                             memRbFinal, memRbNr, memImgFinal, memImgNr})
        vkFreeMemory(c.dev, m, nullptr);
    vkDestroyImageView(c.dev, imgInView, nullptr);
    vkDestroyImageView(c.dev, viewFinal, nullptr);
    vkDestroyImageView(c.dev, viewNr, nullptr);
    vkDestroyImage(c.dev, imgIn, nullptr);
    vkDestroyImage(c.dev, imgFinal, nullptr);
    vkDestroyImage(c.dev, imgNr, nullptr);
    vkDestroyDevice(c.dev, nullptr);
    vkDestroyInstance(c.inst, nullptr);
    if (sharedNT) CloseHandle(sharedNT);
    if (fenceNT) CloseHandle(fenceNT);

    return pass ? 0 : 1;
}
