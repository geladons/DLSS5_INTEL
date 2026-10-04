// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - DDA capture: DXGI output pick + D3D11 device + DuplicateOutput
// session (extracted from main.cpp). The Cap struct carries the COM state;
// the live desktop extent is reported via out params (caller's W/H).
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct Cap {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> stagingTex;
    int outX = 0, outY = 0;
    std::string outName, adapterName;
    LUID adapterLuid{};
};

// Enumerates adapters/outputs and fills `c` with the capture pick
// (--output name > output under the cursor > primary > first).
bool CapPickInto(Cap& c, const char* prefName);
// D3D11 device + DuplicateOutput + staging texture on the picked output.
// Writes the live mode extent into outW/outH (the desktop may differ from
// the configured canvas).
bool CapBuildFromPick(Cap& c, uint32_t& outW, uint32_t& outH);
bool CapBuild(Cap& c, const char* prefName, uint32_t& outW, uint32_t& outH);
