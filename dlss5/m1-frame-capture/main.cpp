// M1 frame capture — DXGI Desktop Duplication (DDA) proof on Intel Arc Pro B50.
//
// C++17, raw DXGI/D3D11, deps: Windows SDK only (dxgi.lib, d3d11.lib).
//
//   m1dda.exe [frames] [snapshot_interval] [timeout_ms] [outdir]
//     frames            total AcquireNextFrame attempts        (default 300)
//     snapshot_interval write a BMP every N acquired frames  (default 60)
//     timeout_ms        AcquireNextFrame timeout             (default 500)
//     outdir            snapshot output dir                  (default out)
//
// Exit codes: 0 success, 1 usage/fatal, 2 duplication unavailable,
//             3 no frames acquired.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <wrl.h>

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

static const char* HrName(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_WAIT_TIMEOUT:              return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_ACCESS_LOST:               return "DXGI_ERROR_ACCESS_LOST";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:   return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_SESSION_DISCONNECTED:      return "DXGI_ERROR_SESSION_DISCONNECTED";
        case E_ACCESSDENIED:                       return "E_ACCESSDENIED";
        case E_INVALIDARG:                         return "E_INVALIDARG";
        case DXGI_ERROR_UNSUPPORTED:               return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_DEVICE_REMOVED:            return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_HUNG:               return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_RESET:              return "DXGI_ERROR_DEVICE_RESET";
        default:                                   return "(other)";
    }
}

static void ExplainDuplicationFailure(HRESULT hr) {
    std::fprintf(stderr, "\n[FAIL] IDXGIOutput duplication could not start: hr=0x%08lx (%s)\n", (unsigned long)hr, HrName(hr));
    switch (hr) {
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
            std::fprintf(stderr,
                "  DXGI_ERROR_NOT_CURRENTLY_AVAILABLE means the output is not currently\n"
                "  duplicable. Typical causes:\n"
                "    - the session is locked / on the secure (UAC) desktop,\n"
                "    - another application already duplicates this output\n"
                "      (another screen recorder, RemoteFX, dxcam, OBS, etc.),\n"
                "    - the session is in a desktop transition.\n"
                "  Unlock the console and stop other duplicators, then retry.\n");
            break;
        case DXGI_ERROR_SESSION_DISCONNECTED:
            std::fprintf(stderr, "  The session is disconnected (RDP/console switch). DDA needs an active\n  interactive session; reconnect the console session and retry.\n");
            break;
        case E_ACCESSDENIED:
            std::fprintf(stderr, "  Access denied — duplication from a secure desktop or insufficient\n  privilege. Run on the active unlocked console session.\n");
            break;
        case E_INVALIDARG:
            std::fprintf(stderr, "  E_INVALIDARG — the D3D11 device was not created with\n  D3D11_CREATE_DEVICE_BGRA_SUPPORT (required for duplication), or the\n  device/output pairing is invalid.\n");
            break;
        default:
            std::fprintf(stderr, "  Unclassified failure; see dxerr/HRESULT tables.\n");
            break;
    }
}

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {
            // subnormal
            int e = -1;
            do { mant <<= 1; --e; } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            f = sign | ((e + 127 - 14 + 1) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13); // inf / nan
    } else {
        f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

// Convert one pixel of the supported source formats to B8G8R8A8 (byte order B,G,R,A).
static bool ConvertPixelToBGRA(DXGI_FORMAT fmt, const uint8_t* src, uint8_t* bgra) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            bgra[0] = src[0]; bgra[1] = src[1]; bgra[2] = src[2]; bgra[3] = src[3];
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t v;
            std::memcpy(&v, src, 4);
            uint32_t b = (v >> 0)  & 0x3FFu;
            uint32_t g = (v >> 10) & 0x3FFu;
            uint32_t r = (v >> 20) & 0x3FFu;
            bgra[0] = (uint8_t)((b * 255u + 511u) / 1023u);
            bgra[1] = (uint8_t)((g * 255u + 511u) / 1023u);
            bgra[2] = (uint8_t)((r * 255u + 511u) / 1023u);
            bgra[3] = 255;
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t hb, hg, hr, ha;
            std::memcpy(&hb, src + 0, 2);
            std::memcpy(&hg, src + 2, 2);
            std::memcpy(&hr, src + 4, 2);
            std::memcpy(&ha, src + 6, 2);
            auto cvt = [](uint16_t h) -> uint8_t {
                float f = HalfToFloat(h);
                if (!(f > 0.0f)) f = 0.0f;           // also catches NaN
                f = f * 255.0f;
                if (f > 255.0f) f = 255.0f;
                return (uint8_t)(f + 0.5f);
            };
            bgra[0] = cvt(hb); bgra[1] = cvt(hg); bgra[2] = cvt(hr); bgra[3] = 255; // ignore alpha channel
            return true;
        }
        default:
            return false;
    }
}

static const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:   return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT";
        default:                              return "(other)";
    }
}

static const char* RotationName(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_UNSPECIFIED: return "UNSPECIFIED";
        case DXGI_MODE_ROTATION_IDENTITY:    return "IDENTITY";
        case DXGI_MODE_ROTATION_ROTATE90:    return "ROTATE90";
        case DXGI_MODE_ROTATION_ROTATE180:   return "ROTATE180";
        case DXGI_MODE_ROTATION_ROTATE270:   return "ROTATE270";
        default:                             return "(other)";
    }
}

// Returns packed DWord count of one row of the given format, or 0 if unsupported.
static bool FormatSupported(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
    }
}

static uint32_t BytesPerPixelOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        default:                             return 0;
    }
}

struct VarianceStats {
    double varB = 0, varG = 0, varR = 0;   // per-channel variance
    double varLuma = 0;                     // variance of (0.114B+0.587G+0.299R)
    int    minLuma = 255, maxLuma = 0;
    uint64_t samples = 0;
};

// Sample every `step`-th pixel in each direction.
static VarianceStats ComputeVarianceBGRA(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, uint32_t step) {
    VarianceStats s;
    double sumB = 0, sumG = 0, sumR = 0, sumL = 0;
    double sumB2 = 0, sumG2 = 0, sumR2 = 0, sumL2 = 0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; y += step) {
        const uint8_t* row = &bgra[(size_t)y * w * 4];
        for (uint32_t x = 0; x < w; x += step) {
            int b = row[(size_t)x * 4 + 0];
            int g = row[(size_t)x * 4 + 1];
            int r = row[(size_t)x * 4 + 2];
            int l = (114 * b + 587 * g + 299 * r + 500) / 1000;
            sumB += b; sumG += g; sumR += r; sumL += l;
            sumB2 += (double)b * b; sumG2 += (double)g * g; sumR2 += (double)r * r; sumL2 += (double)l * l;
            if (l < s.minLuma) s.minLuma = l;
            if (l > s.maxLuma) s.maxLuma = l;
            ++n;
        }
    }
    s.samples = n;
    if (n == 0) return s;
    double dn = (double)n;
    s.varB = sumB2 / dn - (sumB / dn) * (sumB / dn);
    s.varG = sumG2 / dn - (sumG / dn) * (sumG / dn);
    s.varR = sumR2 / dn - (sumR / dn) * (sumR / dn);
    s.varLuma = sumL2 / dn - (sumL / dn) * (sumL / dn);
    return s;
}

static bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t rowBytes = w * 4; // 4-byte aligned already
    const uint32_t imgBytes = rowBytes * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgBytes;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h;   // positive => bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    f.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    // bottom-up row order
    for (uint32_t y = h; y-- > 0;) {
        const uint8_t* row = &bgra[(size_t)y * rowBytes];
        f.write(reinterpret_cast<const char*>(row), rowBytes);
        if (!f) return false;
    }
    return f.good();
}

static std::string HresultHex(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
    return buf;
}

// Desktop Duplication is dirty-rect driven: on an idle VM desktop nothing
// changes and AcquireNextFrame times out forever. This thread nudges the
// mouse cursor back and forth (net-zero drift) so the compositor produces
// real frames. Suppress with argv[5]==0.
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

int main(int argc, char** argv) {
    int framesTotal = 300;
    int snapInterval = 60;
    int timeoutMs = 500;
    std::string outDir = "out";
    int wiggle = 1;
    if (argc > 1) framesTotal = std::atoi(argv[1]);
    if (argc > 2) snapInterval = std::atoi(argv[2]);
    if (argc > 3) timeoutMs = std::atoi(argv[3]);
    if (argc > 4) outDir = argv[4];
    if (argc > 5) wiggle = std::atoi(argv[5]);
    if (argc > 6 || framesTotal <= 0 || snapInterval <= 0 || timeoutMs <= 0) {
        std::fprintf(stderr, "usage: m1dda [frames] [snapshot_interval] [timeout_ms] [outdir] [wiggle 0/1]\n");
        return 1;
    }

    std::printf("=== M1: DXGI Desktop Duplication frame capture ===\n");
    std::printf("args: frames=%d snapshot_interval=%d timeout_ms=%d outdir=%s wiggle=%d\n\n",
                framesTotal, snapInterval, timeoutMs, outDir.c_str(), wiggle);

    // ---------------------------------------------------------------- device
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for duplication
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL gotLevel = (D3D_FEATURE_LEVEL)0;
    const char* driverUsed = nullptr;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device, &gotLevel, &context);
    if (SUCCEEDED(hr)) {
        driverUsed = "HARDWARE";
    } else {
        std::fprintf(stderr, "[warn] D3D11CreateDevice(HARDWARE) failed: %s — falling back to WARP (software)\n", HrName(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, &gotLevel, &context);
        if (SUCCEEDED(hr)) driverUsed = "WARP";
    }
    if (FAILED(hr) || !device) {
        std::fprintf(stderr, "[FAIL] no D3D11 device could be created: %s\n", HrName(hr));
        return 1;
    }
    if (std::strcmp(driverUsed, "WARP") == 0)
        std::printf("[warn] using WARP software device — Desktop Duplication may fail (needs hardware adapter)\n");

    // Which adapter does this device live on?
    ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    ComPtr<IDXGIAdapter> deviceAdapter;
    dxgiDevice->GetAdapter(&deviceAdapter);
    DXGI_ADAPTER_DESC adesc{};
    deviceAdapter->GetDesc(&adesc);
    char adescA[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, adescA, sizeof(adescA) - 1, nullptr, nullptr);
    std::printf("D3D11 device: driver=%s feature_level=0x%x adapter=\"%s\" (LUID %08lx:%08lx) VRAM=%.0f MB\n",
                driverUsed, (unsigned)gotLevel, adescA,
                (unsigned long)adesc.AdapterLuid.HighPart, (unsigned long)adesc.AdapterLuid.LowPart,
                (double)adesc.DedicatedVideoMemory / (1024.0 * 1024.0));

    // ------------------------------------------------------- output to grab
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC odesc{};
    {
        ComPtr<IDXGIOutput> o;
        UINT i = 0;
        ComPtr<IDXGIOutput> firstAttached;
        while (deviceAdapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            char nameA[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, nameA, sizeof(nameA) - 1, nullptr, nullptr);
            std::printf("output[%u]: \"%s\" attachedToDesktop=%d coords=(%ld,%ld)-(%ld,%ld)\n",
                        i, nameA, (int)d.AttachedToDesktop,
                        (long)d.DesktopCoordinates.left, (long)d.DesktopCoordinates.top,
                        (long)d.DesktopCoordinates.right, (long)d.DesktopCoordinates.bottom);
            if (d.AttachedToDesktop && !firstAttached) firstAttached = o;
            ++i;
            o.Reset();
        }
        if (!firstAttached) {
            std::fprintf(stderr, "[FAIL] no output attached to the desktop on this adapter — DDA has nothing to capture\n");
            return 2;
        }
        output = firstAttached;
        output->GetDesc(&odesc);
    }
    char odevA[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, odesc.DeviceName, -1, odevA, sizeof(odevA) - 1, nullptr, nullptr);

    // --------------------------------------------------------- duplication
    ComPtr<IDXGIOutputDuplication> dup;
    std::string dupMode;
    {
        ComPtr<IDXGIOutput5> out5;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out5)))) {
            DXGI_FORMAT want[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            hr = out5->DuplicateOutput1(device.Get(), 0, 1, want, &dup);
            if (SUCCEEDED(hr)) {
                dupMode = "DuplicateOutput1 (pinned B8G8R8A8_UNORM)";
            } else {
                std::printf("[info] DuplicateOutput1 (pin B8G8R8A8) failed: %s %s — falling back\n",
                            HrName(hr), HresultHex(hr).c_str());
            }
        } else {
            std::printf("[info] IDXGIOutput5 unavailable (older OS/driver) — using DuplicateOutput\n");
        }
        if (!dup) {
            ComPtr<IDXGIOutput1> out1;
            output->QueryInterface(IID_PPV_ARGS(&out1));
            hr = out1->DuplicateOutput(device.Get(), &dup);
            if (SUCCEEDED(hr)) dupMode = "DuplicateOutput (driver-chosen format)";
        }
    }
    if (FAILED(hr) || !dup) {
        ExplainDuplicationFailure(hr);
        return 2;
    }

    DXGI_OUTDUPL_DESC ddesc{};
    dup->GetDesc(&ddesc);
    std::printf("\nDuplication active via %s\n", dupMode.c_str());
    std::printf("output device: \"%s\"\n", odevA);
    std::printf("mode: %ux%u format=%s rotation=%s desktopImageInSystemMemory=%d\n\n",
                (unsigned)ddesc.ModeDesc.Width, (unsigned)ddesc.ModeDesc.Height,
                DxgiFormatName(ddesc.ModeDesc.Format), RotationName(ddesc.Rotation),
                (int)ddesc.DesktopImageInSystemMemory);

    CreateDirectoryA(outDir.c_str(), nullptr); // ok if it exists

    if (wiggle) {
        std::printf("[info] cursor-wiggle activity generator ON (desktop is dirty-rect driven; an idle\n"
                    "       VM desktop produces zero DDA frames). Pass 0 as argv[5] to disable.\n\n");
    }

    // ------------------------------------------------------------- capture
    ComPtr<ID3D11Texture2D> staging;   // CPU-readable, same format as source
    D3D11_TEXTURE2D_DESC stagingDesc{};
    std::vector<uint8_t> cpuBGRA;      // converted B8G8R8A8 frame
    uint32_t frameW = 0, frameH = 0;
    DXGI_FORMAT srcFmt = DXGI_FORMAT_UNKNOWN;

    long acquired = 0, timeouts = 0, updated = 0, repeated = 0, accessLost = 0, errors = 0;
    bool gotFirst = false;
    std::chrono::steady_clock::time_point tFirst, tLast, tPrev;
    double minDeltaMs = 1e30, maxDeltaMs = 0;

    std::vector<std::string> snapshotPaths;
    std::vector<VarianceStats> snapshotStats;

    auto RecreateDuplication = [&]() -> bool {
        dup.Reset();
        Sleep(100);
        ComPtr<IDXGIOutput5> out5;
        HRESULT r = E_FAIL;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out5)))) {
            DXGI_FORMAT want[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            r = out5->DuplicateOutput1(device.Get(), 0, 1, want, &dup);
        }
        if (FAILED(r)) {
            ComPtr<IDXGIOutput1> out1;
            output->QueryInterface(IID_PPV_ARGS(&out1));
            r = out1->DuplicateOutput(device.Get(), &dup);
        }
        staging.Reset(); // force rebuild
        return SUCCEEDED(r);
    };

    std::printf("capture loop: %d attempts, snapshot every %d acquired frames...\n", framesTotal, snapInterval);

    std::thread wiggler;
    if (wiggle) {
        g_wiggleStop.store(false);
        wiggler = std::thread(WiggleThread);
    }

    while (acquired + timeouts < framesTotal) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> resource;
        hr = dup->AcquireNextFrame(timeoutMs, &fi, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            ++accessLost;
            std::fprintf(stderr, "[warn] DXGI_ERROR_ACCESS_LOST (mode change / desktop switch) — re-creating duplication...\n");
            if (!RecreateDuplication()) { std::fprintf(stderr, "[FAIL] could not re-create duplication\n"); break; }
            continue;
        }
        if (FAILED(hr)) { ++errors; std::fprintf(stderr, "[error] AcquireNextFrame: %s\n", HrName(hr)); break; }

        if (fi.AccumulatedFrames > 0) ++updated; else ++repeated;

        ComPtr<ID3D11Texture2D> tex;
        hr = resource->QueryInterface(IID_PPV_ARGS(&tex));
        if (FAILED(hr)) { dup->ReleaseFrame(); std::fprintf(stderr, "[error] resource is not a texture2d\n"); break; }

        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);

        if (!FormatSupported(td.Format)) {
            std::fprintf(stderr, "[FAIL] unsupported desktop image format %s (%d) — extend FormatSupported/ConvertPixelToBGRA\n",
                         DxgiFormatName(td.Format), (int)td.Format);
            dup->ReleaseFrame();
            return 1;
        }

        // (re)create staging texture if geometry/format changed
        if (!staging || stagingDesc.Width != td.Width || stagingDesc.Height != td.Height || stagingDesc.Format != td.Format) {
            stagingDesc = td;
            stagingDesc.ArraySize = 1;
            stagingDesc.MipLevels = 1;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.SampleDesc.Quality = 0;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
            if (FAILED(hr)) {
                std::fprintf(stderr, "[FAIL] CreateTexture2D(staging): %s\n", HrName(hr));
                dup->ReleaseFrame();
                return 1;
            }
            frameW = td.Width; frameH = td.Height; srcFmt = td.Format;
            cpuBGRA.resize((size_t)frameW * frameH * 4);
            std::printf("staging texture: %ux%u format=%s bpp=%u\n",
                        frameW, frameH, DxgiFormatName(srcFmt), BytesPerPixelOf(srcFmt));
        }

        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
        dup->ReleaseFrame(); // release the duplicated frame ASAP, staging is ours

        // ---- map + convert to BGRA8
        D3D11_MAPPED_SUBRESOURCE ms{};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
        if (FAILED(hr)) { ++errors; std::fprintf(stderr, "[error] Map: %s\n", HrName(hr)); continue; }
        {
            const uint32_t srcBpp = BytesPerPixelOf(srcFmt);
            if (srcFmt == DXGI_FORMAT_B8G8R8A8_UNORM || srcFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                if (ms.RowPitch == (INT)(frameW * 4)) {
                    std::memcpy(cpuBGRA.data(), ms.pData, (size_t)frameW * frameH * 4);
                } else {
                    for (uint32_t y = 0; y < frameH; ++y)
                        std::memcpy(&cpuBGRA[(size_t)y * frameW * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)frameW * 4);
                }
            } else {
                for (uint32_t y = 0; y < frameH; ++y) {
                    const uint8_t* srow = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
                    uint8_t* drow = &cpuBGRA[(size_t)y * frameW * 4];
                    for (uint32_t x = 0; x < frameW; ++x)
                        ConvertPixelToBGRA(srcFmt, srow + (size_t)x * srcBpp, drow + (size_t)x * 4);
                }
            }
        }
        context->Unmap(staging.Get(), 0);

        // ---- timing
        auto now = std::chrono::steady_clock::now();
        if (!gotFirst) { tFirst = tPrev = tLast = now; gotFirst = true; }
        else {
            double d = std::chrono::duration<double, std::milli>(now - tPrev).count();
            if (d < minDeltaMs) minDeltaMs = d;
            if (d > maxDeltaMs) maxDeltaMs = d;
            tPrev = tLast = now;
        }
        ++acquired;

        // ---- snapshot?
        if (acquired % snapInterval == 0 || acquired == framesTotal) {
            VarianceStats st = ComputeVarianceBGRA(cpuBGRA, frameW, frameH, 8);
            char name[256];
            std::snprintf(name, sizeof(name), "%s\\snapshot_%ld.bmp", outDir.c_str(), acquired);
            bool ok = WriteBmpBGRA(name, cpuBGRA, frameW, frameH);
            std::printf("snapshot frame %ld -> %s : %s | luma var=%.2f range=[%d,%d] B/G/R var=%.1f/%.1f/%.1f samples=%llu\n",
                        acquired, name, ok ? "written" : "WRITE FAILED",
                        st.varLuma, st.minLuma, st.maxLuma, st.varB, st.varG, st.varR,
                        (unsigned long long)st.samples);
            if (ok) { snapshotPaths.push_back(name); snapshotStats.push_back(st); }
        }
    }

    // ------------------------------------------------------------- summary
    if (wiggler.joinable()) {
        g_wiggleStop.store(true);
        wiggler.join();
    }

    double avgFps = 0;
    if (acquired > 1) {
        double span = std::chrono::duration<double>(tLast - tFirst).count();
        if (span > 0) avgFps = (acquired - 1) / span;
    }
    double minFps = (maxDeltaMs > 0) ? 1000.0 / maxDeltaMs : 0; // slowest interval
    double maxFps = (minDeltaMs > 0 && minDeltaMs < 1e29) ? 1000.0 / minDeltaMs : 0; // fastest interval

    std::printf("\n=== M1 summary ===\n");
    std::printf("mode: DXGI Desktop Duplication (%s)\n", dupMode.c_str());
    std::printf("output: \"%s\" adapter=\"%s\" driver=%s\n", odevA, adescA, driverUsed);
    if (frameW == 0) { // no frame ever arrived — fall back to the mode we set up for
        frameW = ddesc.ModeDesc.Width;
        frameH = ddesc.ModeDesc.Height;
        srcFmt = ddesc.ModeDesc.Format;
    }
    std::printf("resolution: %ux%u desktop format=%s (captured as B8G8R8A8)\n",
                frameW, frameH, DxgiFormatName(srcFmt));
    std::printf("attempts: %ld | acquired: %ld | timeouts: %ld | updated(dirty): %ld | repeated(same): %ld | access-lost: %ld | errors: %ld\n",
                acquired + timeouts, acquired, timeouts, updated, repeated, accessLost, errors);
    if (acquired > 1)
        std::printf("fps: avg %.2f | interval min %.2f ms (%.1f fps) | interval max %.2f ms (%.1f fps)\n",
                    avgFps, minDeltaMs, maxFps, maxDeltaMs, minFps);
    std::printf("snapshots: %zu\n", snapshotPaths.size());
    bool contentOk = true;
    for (size_t i = 0; i < snapshotPaths.size(); ++i) {
        const VarianceStats& st = snapshotStats[i];
        bool nonUniform = st.varLuma > 1.0 && (st.maxLuma - st.minLuma) > 10;
        if (!nonUniform) contentOk = false;
        std::printf("  %s : luma var=%.2f range=[%d,%d] -> %s\n", snapshotPaths[i].c_str(),
                    st.varLuma, st.minLuma, st.maxLuma, nonUniform ? "REAL CONTENT (non-uniform)" : "SUSPECT (uniform/black)");
    }
    std::printf("\n");

    if (acquired == 0) { std::fprintf(stderr, "[FAIL] no frames acquired\n"); return 3; }
    if (snapshotPaths.size() < 3 || !contentOk) { std::fprintf(stderr, "[FAIL] content verification failed\n"); return 3; }
    std::printf("RESULT: PASS — %ld frames at %.1f avg fps, %zu snapshots verified non-uniform\n",
                acquired, avgFps, snapshotPaths.size());
    return 0;
}
