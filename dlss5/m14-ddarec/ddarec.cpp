// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// ddarec — DXGI Desktop Duplication streamer for game-test recording.
//
// Writes CFR rawvideo BGRA frames to STDOUT at a fixed cadence so ffmpeg can
// encode in real time:
//
//   ddarec.exe 10 60 0,0,800,600 - | ffmpeg -f rawvideo -pix_fmt bgra
//        -s 800x600 -r 60 -i - -c:v h264_qsv -b:v 20M rec.mp4
//
//   argv[1] = seconds to record (float ok, 0 = run until killed)
//   argv[2] = output fps (default 60)
//   argv[3] = crop rect "x,y,w,h" in desktop coords (default: full output)
//   argv[4] = output path, or "-" for stdout (default "-")
//
// DDA is update-driven: AcquireNextFrame returns only when the desktop
// changed. The emitter thread holds a constant cadence — the LATEST acquired
// frame is repeated on timeout, so a game presenting at 30 fps lands in the
// 60 fps file as clean frame doubles (exactly what the user sees), and every
// real present is captured with no tearing. Frame 0 of a fresh duplication
// is the black warm-up frame; it is discarded before streaming starts.
//
// All diagnostics go to stderr (stdout is the video pipe).
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

using Microsoft::WRL::ComPtr;
using std::chrono::steady_clock;
using std::chrono::duration;
using std::chrono::milliseconds;

static const char* HrName(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_WAIT_TIMEOUT:      return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_ACCESS_LOST:       return "DXGI_ERROR_ACCESS_LOST";
        case DXGI_ERROR_INVALID_CALL:      return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_UNSUPPORTED:       return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_ACCESS_DENIED:     return "DXGI_ERROR_ACCESS_DENIED";
        case E_INVALIDARG:                 return "E_INVALIDARG";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        default: return "HRESULT";
    }
}
static std::string HresultHex(HRESULT hr) {
    char b[32]; std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

// Latest converted BGRA frame owned by the capture thread; the emitter
// thread swaps it out under a mutex. Double-buffered so the emitter never
// stalls the acquire loop longer than a memcpy.
struct FrameSlot {
    std::vector<uint8_t> buf;    // full-frame BGRA
    uint32_t w = 0, h = 0;
    uint64_t seq = 0;            // bumped on every new acquired frame
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
    setvbuf(stderr, nullptr, _IONBF, 0);

    double seconds = (argc > 1) ? std::atof(argv[1]) : 10.0;
    double outFps  = (argc > 2) ? std::atof(argv[2]) : 60.0;
    int    cropX = 0, cropY = 0, cropW = -1, cropH = -1;
    if (argc > 3 && argv[3][0] && std::strchr(argv[3], ','))
        std::sscanf(argv[3], "%d,%d,%d,%d", &cropX, &cropY, &cropW, &cropH);
    if (cropX < 0) { cropW += cropX; cropX = 0; }   // window frames can report
    if (cropY < 0) { cropH += cropY; cropY = 0; }   // negative border offsets
    const char* outPath = (argc > 4 && argv[4][0]) ? argv[4] : "-";

    FILE* out = stdout;
    if (std::strcmp(outPath, "-") != 0) {
        out = std::fopen(outPath, "wb");
        if (!out) { std::fprintf(stderr, "[FAIL] cannot open %s\n", outPath); return 1; }
    }

    // ------------------------------------------------- D3D11 + duplication
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, nullptr, &context);
    }
    if (FAILED(hr) || !device) {
        std::fprintf(stderr, "[FAIL] D3D11CreateDevice: %s %s\n", HrName(hr), HresultHex(hr).c_str());
        return 1;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    {
        ComPtr<IDXGIOutput> o; UINT i = 0;
        while (adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC d{}; o->GetDesc(&d);
            if (d.AttachedToDesktop && !output) output = o;
            ++i; o.Reset();
        }
        if (!output) { std::fprintf(stderr, "[FAIL] no attached output\n"); return 2; }
    }

    ComPtr<IDXGIOutputDuplication> dup;
    {
        ComPtr<IDXGIOutput5> out5;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out5)))) {
            DXGI_FORMAT want[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = out5->DuplicateOutput1(device.Get(), 0, 1, want, &dup);
        }
        if (FAILED(hr) || !dup) {
            ComPtr<IDXGIOutput1> out1;
            output->QueryInterface(IID_PPV_ARGS(&out1));
            hr = out1->DuplicateOutput(device.Get(), &dup);
        }
    }
    if (FAILED(hr) || !dup) {
        std::fprintf(stderr, "[FAIL] DuplicateOutput: %s %s\n", HrName(hr), HresultHex(hr).c_str());
        return 2;
    }
    DXGI_OUTDUPL_DESC ddesc{};
    dup->GetDesc(&ddesc);
    uint32_t deskW = ddesc.ModeDesc.Width, deskH = ddesc.ModeDesc.Height;
    std::fprintf(stderr, "[ddarec] duplication up: %ux%u %s\n", deskW, deskH,
                 ddesc.DesktopImageInSystemMemory ? "(sysmem)" : "");

    if (cropW <= 0 || (uint32_t)(cropX + cropW) > deskW) cropW = (int)deskW - cropX;
    if (cropH <= 0 || (uint32_t)(cropY + cropH) > deskH) cropH = (int)deskH - cropY;
    cropW &= ~1; cropH &= ~1;   // keep output dims even (yuv420 encoders)
    std::fprintf(stderr, "[ddarec] crop %d,%d %dx%d -> out fps %.1f dur %.1fs\n",
                 cropX, cropY, cropW, cropH, outFps, seconds);

    // ----------------------------------------------------- capture thread
    FrameSlot slots[2];
    std::mutex mtx;
    std::atomic<uint64_t> latestSeq{0};
    std::atomic<int> latestIdx{0};
    std::atomic<bool> stop{false};
    std::atomic<long> acquired{0}, timeouts{0}, errors{0};

    std::thread cap([&]() {
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC sd{};
        std::vector<uint8_t> conv;
        uint32_t fw = 0, fh = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        while (!stop.load()) {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT r = dup->AcquireNextFrame(8, &fi, &res);
            if (r == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
            if (r == DXGI_ERROR_ACCESS_LOST) {
                // rebuild duplication (mode change / secure desktop)
                dup.Reset(); Sleep(60);
                ComPtr<IDXGIOutput5> o5; HRESULT rr = E_FAIL;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&o5)))) {
                    DXGI_FORMAT want[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
                    rr = o5->DuplicateOutput1(device.Get(), 0, 1, want, &dup);
                }
                if (FAILED(rr)) {
                    ComPtr<IDXGIOutput1> o1;
                    output->QueryInterface(IID_PPV_ARGS(&o1));
                    rr = o1->DuplicateOutput(device.Get(), &dup);
                }
                staging.Reset();
                if (FAILED(rr)) { std::fprintf(stderr, "[ddarec] duplication rebuild failed\n"); break; }
                continue;
            }
            if (FAILED(r)) { ++errors; continue; }
            ComPtr<ID3D11Texture2D> tex;
            res->QueryInterface(IID_PPV_ARGS(&tex));
            D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
            if (!staging || td.Width != fw || td.Height != fh) {
                sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
                sd.MipLevels = 1; sd.ArraySize = 1;
                sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
                if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
                    dup->ReleaseFrame(); ++errors; continue;
                }
                fw = td.Width; fh = td.Height; fmt = td.Format;
                conv.resize((size_t)fw * fh * 4);
            }
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
            dup->ReleaseFrame();
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) { ++errors; continue; }
            // source is pinned B8G8R8A8 -> straight row copy
            int idx = (latestIdx.load() + 1) & 1;
            slots[idx].buf.resize((size_t)fw * fh * 4);
            for (uint32_t y = 0; y < fh; ++y)
                std::memcpy(&slots[idx].buf[(size_t)y * fw * 4],
                            (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch,
                            (size_t)fw * 4);
            context->Unmap(staging.Get(), 0);
            {
                std::lock_guard<std::mutex> lk(mtx);
                slots[idx].w = fw; slots[idx].h = fh;
                slots[idx].seq = acquired.load() + 1;
                latestIdx.store(idx);
                latestSeq.store(slots[idx].seq);
            }
            ++acquired;
        }
    });

    // discard the black warm-up frame (first frame of a new duplication)
    while (latestSeq.load() == 0 && !stop.load()) Sleep(5);
    {
        std::lock_guard<std::mutex> lk(mtx);
        slots[latestIdx.load()].seq = 0;
        latestSeq.store(0);
    }
    std::fprintf(stderr, "[ddarec] warm-up frame discarded, streaming\n");

    // -------------------------------------------------------- emit thread
    const auto framePeriod = duration<double>(1.0 / outFps);
    const size_t cropBytes = (size_t)cropW * cropH * 4;
    std::vector<uint8_t> line(cropBytes);
    steady_clock::time_point t0 = steady_clock::now();
    steady_clock::time_point next = t0;
    uint64_t emitted = 0, repeated = 0;
    uint64_t lastSeq = 0;

    while (seconds <= 0.0 || duration<double>(steady_clock::now() - t0).count() < seconds) {
        next += std::chrono::duration_cast<steady_clock::duration>(framePeriod);
        std::this_thread::sleep_until(next);
        int idx = latestIdx.load();
        const FrameSlot& s = slots[idx];
        if (!s.w || !s.h) continue;
        // crop rows out of the full frame
        for (int y = 0; y < cropH; ++y) {
            const uint8_t* src = &s.buf[((size_t)(cropY + y) * s.w + cropX) * 4];
            std::memcpy(&line[(size_t)y * cropW * 4], src, (size_t)cropW * 4);
        }
        if (std::fwrite(line.data(), 1, cropBytes, out) != cropBytes) {
            std::fprintf(stderr, "[ddarec] output write failed (pipe closed?)\n");
            break;
        }
        ++emitted;
        if (s.seq == lastSeq) ++repeated; else lastSeq = s.seq;
    }

    stop.store(true);
    if (cap.joinable()) cap.join();
    if (out != stdout) std::fclose(out);
    else std::fflush(out);

    std::fprintf(stderr, "[ddarec] done: emitted=%llu (repeated=%llu) acquired=%ld timeouts=%ld errors=%ld\n",
                 (unsigned long long)emitted, (unsigned long long)repeated,
                 acquired.load(), timeouts.load(), errors.load());
    return emitted ? 0 : 3;
}
