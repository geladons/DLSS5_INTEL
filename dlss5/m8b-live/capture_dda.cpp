// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - DDA capture (see capture_dda.h).
#include "capture_dda.h"
#include "win_util.h"

#include <cstdio>


// Enumerate every adapter/output; print the inventory table; pick the capture
// target by the priority above. Fills adapter/output/origin/names/luid only
// (no device — that is CapBuildFromPick).
bool CapPickInto(Cap& c, const char* prefName) {
    c = Cap{};
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FAIL] CreateDXGIFactory1: %s\n", HrName(hr));
        return false;
    }
    POINT cur{};
    const bool haveCursor = (GetCursorPos(&cur) != 0);

    struct Cand {
        ComPtr<IDXGIAdapter1> a;
        ComPtr<IDXGIOutput> o;
        DXGI_OUTPUT_DESC d;
        std::string devName, adpName;
        LUID luid;
    };
    std::vector<Cand> attached;

    std::printf("[cap] DXGI inventory (adapter x outputs):\n");
    UINT ai = 0;
    ComPtr<IDXGIAdapter1> a1;
    while (factory->EnumAdapters1(ai, &a1) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC ad{};
        a1->GetDesc(&ad);
        char an[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, an, sizeof(an) - 1, nullptr, nullptr);
        ComPtr<IDXGIOutput> o;
        UINT oi = 0;
        while (a1->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            char dn[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, dn, sizeof(dn) - 1, nullptr, nullptr);
            const bool primary =
                (d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0);
            std::printf("  [a%u] %-44s | %-16s | attached=%d primary=%d rect=(%d,%d)-(%d,%d)\n",
                        ai, an, dn, (int)d.AttachedToDesktop, (int)primary,
                        d.DesktopCoordinates.left, d.DesktopCoordinates.top,
                        d.DesktopCoordinates.right, d.DesktopCoordinates.bottom);
            if (d.AttachedToDesktop) {
                Cand cc;
                cc.a = a1; cc.o = o; cc.d = d;
                cc.devName = dn; cc.adpName = an; cc.luid = ad.AdapterLuid;
                attached.push_back(cc);
            }
            ++oi;
            o.Reset();
        }
        ++ai;
        a1.Reset();
    }
    if (attached.empty()) {
        std::fprintf(stderr, "[FAIL] no attached DXGI output\n");
        return false;
    }

    auto containsCursor = [&](const Cand& cc) {
        return haveCursor &&
               cur.x >= cc.d.DesktopCoordinates.left && cur.x < cc.d.DesktopCoordinates.right &&
               cur.y >= cc.d.DesktopCoordinates.top && cur.y < cc.d.DesktopCoordinates.bottom;
    };
    const Cand* pick = nullptr;
    const char* rule = "";
    if (prefName && prefName[0]) {
        for (const auto& cc : attached)
            if (StrContainsI(cc.devName.c_str(), prefName)) { pick = &cc; rule = "--output"; break; }
        if (!pick)
            std::printf("[warn] --output '%s' matched no attached output - auto-picking\n", prefName);
    }
    if (!pick) { for (const auto& cc : attached) if (containsCursor(cc)) { pick = &cc; rule = "cursor"; break; } }
    if (!pick) { for (const auto& cc : attached) if (cc.d.DesktopCoordinates.left == 0 && cc.d.DesktopCoordinates.top == 0) { pick = &cc; rule = "primary"; break; } }
    if (!pick) { pick = &attached.front(); rule = "first-attached"; }

    c.adapter = pick->a;
    c.output = pick->o;
    c.adapterLuid = pick->luid;
    c.adapterName = pick->adpName;
    c.outName = pick->devName;
    c.outX = pick->d.DesktopCoordinates.left;
    c.outY = pick->d.DesktopCoordinates.top;
    std::printf("[cap] picked '%s' on adapter '%s' (luid %s) origin=(%d,%d), rule=%s\n",
                c.outName.c_str(), c.adapterName.c_str(), LuidStr(c.adapterLuid).c_str(),
                c.outX, c.outY, rule);
    return true;
}

// Create the D3D11 device ON the picked adapter, duplicate the picked output,
// enforce the compile-time frame contract (2560x1440 B8G8R8A8 — a different
// resolution fails LOUDLY here, out of scope by design) and create the
// staging texture. Prints the [D3D11]/[DDA] active lines.
bool CapBuildFromPick(Cap& c, uint32_t& outW, uint32_t& outH) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = D3D11CreateDevice(c.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &c.device, &got, &c.context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FAIL] D3D11CreateDevice: %s\n", HrName(hr));
        return false;
    }
    std::printf("[D3D11] hardware device created on adapter '%s' (luid %s), feature level 0x%x\n",
                c.adapterName.c_str(), LuidStr(c.adapterLuid).c_str(), (unsigned)got);

    ComPtr<IDXGIOutput1> out1;
    hr = c.output->QueryInterface(IID_PPV_ARGS(&out1));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FAIL] IDXGIOutput1 QI: %s\n", HrName(hr));
        return false;
    }
    hr = out1->DuplicateOutput(c.device.Get(), &c.dup);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FAIL] DuplicateOutput: %s\n", HrName(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    c.dup->GetDesc(&dd);
    std::printf("[DDA] DuplicateOutput active: %ux%u format=%d rotation=%d origin=(%d,%d)\n",
                (unsigned)dd.ModeDesc.Width, (unsigned)dd.ModeDesc.Height,
                (int)dd.ModeDesc.Format, (int)dd.Rotation, c.outX, c.outY);
    outW = dd.ModeDesc.Width; outH = dd.ModeDesc.Height;   // pin to the LIVE mode
    if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        std::fprintf(stderr, "[FAIL] expected B8G8R8A8, got format=%d\n",
                     (int)dd.ModeDesc.Format);
        return false;
    }
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = outW; sd.Height = outH; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    hr = c.device->CreateTexture2D(&sd, nullptr, &c.stagingTex);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FAIL] staging CreateTexture2D: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    return true;
}

bool CapBuild(Cap& c, const char* prefName, uint32_t& outW, uint32_t& outH) {
    if (!CapPickInto(c, prefName)) return false;
    return CapBuildFromPick(c, outW, outH);
}