// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - DaemonCore: owns the resident ChainEngine, its lifecycle
// (init/re-init on extent/scale change), the temporal/FG anchor state, the
// NRCT control-channel dispatch and the per-request frame path.
// main.cpp keeps argv/env parsing, the socket accept loop and the one-shot
// modes (selftest/bench) - everything engine-facing lives here.
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "engine.h"
#include "phase30.h"

class DaemonCore {
public:
    // ---- configuration (set once from argv/env before serving) -----------
    std::string weights;        // REQUIRED - no machine-specific default
    std::string shaderDir;      // exe-relative unless overridden
    std::string benchProf;      // empty = profiling off (production default)
    float scale = 1.0f;
    int fgMode = 1;             // 0 = off, 1 = residual transfer, 2 = anchor interp
    int fgModeConfigured = 1;   // the value SETFRAMEGEN on restores to
    bool dump = false;
    float gain = 1.0f, blend = 1.0f;
    d5c::Phase30Controller p30Ctl;

    // ---- engine + stream state -------------------------------------------
    d5c::ChainEngine engine;
    uint32_t curW = 0, curH = 0;
    long frameCounter = 0;
    bool havePrev = false;
    uint32_t prevW = 0, prevH = 0;
    ULONGLONG lastFrameTick = 0;

    // Re-inits the engine when the extent or render scale changed (or the
    // engine is down). curW/curH == 0 is the single source of truth for
    // "engine down" - a failed init must not masquerade as healthy.
    bool ensureEngine(uint32_t w, uint32_t h);

    // NRCT control dispatch: applies the command to engine/p30Ctl/local
    // state and writes the 16-byte reply {magic, ok, gainBits, frames}.
    // Caller closes the socket.
    void handleCtrl(SOCKET s, const uint32_t hdr[4]);

    // Frame path for NRN0/NRN1/NRN2/NRN3: header already read; validates
    // magic/extent/format, reads payload (+mask), runs process/transfer/
    // interpolate with the FG+gap policy and writes the BGRA reply (or
    // closes silently on failure - never echoes raw input as "processed").
    // Caller closes the socket. Scratch buffers are caller-owned so the
    // accept loop reuses their capacity across frames.
    void handleFrame(SOCKET s, const uint32_t hdr[4], ULONGLONG deadline,
                     std::vector<uint8_t>& inBuf, std::vector<uint8_t>& outBuf,
                     std::vector<uint8_t>& maskBuf);
};
