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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // Persistent knobs the async rebuild must NOT lose. Every member here
    // is baked into the EngineConfig by makeEngineConfig and re-applied to
    // the swapped engine in builderLoop; a control command that wrote the
    // engine instance only (SETTEMPORAL did) silently reverted to the env
    // default on every extent/scale rebuild.
    bool temporal = true;       // SETTEMPORAL / *_TEMPORAL env baseline
    float sharpness = 0.8f;     // encode CAS; *_SHARPNESS env baseline,
                                // knob-file "sharpness" key live override
                                // (no NRCT command - member-synced like
                                // gain/blend so rebuilds keep the push)
    bool dump = false;
    float gain = 1.0f, blend = 1.0f;
    d5c::Phase30Controller p30Ctl;

    // ---- engine + stream state -------------------------------------------
    // shared_ptr + shutdown deleter: the async builder hot-swaps a fully
    // initialized engine, an in-flight frame keeps the outgoing engine
    // alive through its own copy, and the deleter runs sehEngineShutdown
    // once the last reference drops. Access on the serving path goes
    // through requestEngine()/liveEngine() (engMu_-locked copies) - a raw
    // read while the builder swaps is a shared_ptr race.
    std::shared_ptr<d5c::ChainEngine> engine;
    uint32_t curW = 0, curH = 0;
    long frameCounter = 0;
    std::atomic<bool> havePrev{false};   // the builder clears it on swap
    uint32_t prevW = 0, prevH = 0;
    ULONGLONG lastFrameTick = 0;

    // Synchronous re-init for the one-shot modes (selftest/bench). The
    // serving path uses requestEngine() instead - a synchronous init on
    // the accept thread wedges frames AND NRCT behind a multi-second
    // build (the old behavior: ~10% of a GTA IV session ran native).
    bool ensureEngine(uint32_t w, uint32_t h);

    // Serving-path engine acquisition. Returns the live engine when it
    // can serve this extent - a same-extent engine keeps serving at the
    // old scale while a rebuilt one loads in the background. nullptr when
    // nothing usable exists (cold start / extent migration): the caller
    // must then fail the frame FAST so the layer holds its last processed
    // frame instead of stalling inside init.
    std::shared_ptr<d5c::ChainEngine> requestEngine(uint32_t w, uint32_t h);

    // Engine lifecycle for STATUS replies lives inside handleCtrlLocked:
    // resident engine OR a pending build = alive (the manager restarts a
    // daemon it believes dead - killing a mid-init daemon is the restart
    // loop this async model exists to prevent).

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

private:
    // EngineConfig assembly shared by ensureEngine and the builder thread.
    // renderScale comes from the pending build, not the live member, so a
    // mid-build SETSCALE cannot be silently baked into the wrong engine.
    d5c::EngineConfig makeEngineConfig(uint32_t w, uint32_t h,
                                       float renderScale) const;
    void kickBuildLocked(uint32_t w, uint32_t h);     // engMu_ held
    void builderLoop();                               // async engine builds
    void handleCtrlLocked(const uint32_t hdr[4], uint32_t rep[4]);  // engMu_ held

    std::mutex engMu_;
    struct PendingBuild { uint32_t w, h; float scale; };
    std::unique_ptr<PendingBuild> pending_;           // config to realize next
    std::thread builder_;
    bool builderRunning_ = false;                     // guarded by engMu_
    // Device-loss streak: consecutive engine deaths (processFrame fault,
    // typically VK_ERROR_DEVICE_LOST) without one successful frame between
    // them. A fault that survives a full engine rebuild is not transient -
    // rebuilding forever burns ~3.5 s x 676 MB of GPU work per attempt and
    // starves the very game we serve (field report 2026-10-07: repeated
    // VULKANCOMPUTE_PAGEFAULT TDRs put the daemon in a rebuild loop and the
    // game froze behind it). deadEngines_ >= kDeadEngineLimit exits the
    // process so the game reclaims the device in pure passthrough.
    int deadEngines_ = 0;
    static const int kDeadEngineLimit = 3;
};
