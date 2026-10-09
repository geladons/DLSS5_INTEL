// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - DaemonCore (see daemon_core.h): engine lifecycle + NRCT control
// dispatch + the per-request frame path.
// ============================================================================
#include "daemon_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "daemon_io.h"
#include "engine_seh.h"

static const uint32_t MAGIC_PLAIN = 0x304E524Eu;
static const uint32_t MAGIC_MASKED = 0x314E524Eu;
static const uint32_t MAGIC_FGT = 0x324E524Eu;    // "NRN2" - framegen transfer
static const uint32_t MAGIC_FGI = 0x334E524Eu;    // "NRN3" - framegen interpolate (mode 2)
static const uint32_t MAGIC_CTRL = 0x5443524Eu;   // "NRCT" - M13 control

// Bound-exit marker: on a deliberate dead-engine exit we drop a timestamped
// file the m13 monitor reads so it does not immediately restart us into a
// still-resetting driver (each restart = a multi-GB engine build burst -
// fuel for an ongoing TDR storm). Freshness is checked by mtime.
static void writeFenceFile() {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return;
    std::string p = std::string(tmp) + "m11d_fenced.flag";
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%lld\n", (long long)std::time(nullptr));
    std::fclose(f);
}

// ------------------------------------------------------ engine lifecycle --
// EngineConfig assembly shared by the synchronous path and the async
// builder. renderScale is a parameter (not the live member) so the builder
// uses the pending snapshot - a mid-build SETSCALE then simply schedules
// one more rebuild instead of being silently baked in.
d5c::EngineConfig DaemonCore::makeEngineConfig(uint32_t w, uint32_t h,
                                               float renderScale) const {
    d5c::EngineConfig cfg{};
    cfg.canvasW = w; cfg.canvasH = h;
    cfg.weightsPath = weights;
    cfg.shaderDir = shaderDir;        // exe-relative unless overridden
    cfg.headGain = gain;
    // headBlend is the blend knob's own slot (no shader consumes it); the
    // live compose mix rides phase30.intensity via cfg.phase30 below.
    cfg.headBlend = p30Ctl.engineBlend();
    cfg.renderScale = renderScale;
    cfg.phase30 = p30Ctl.params();
    cfg.sharpness = sharpness;          // env baseline (member - see header)
    // Temporal conditioning default ON (the flicker fix): history anchors
    // each reconstruction to the previous output. The member carries the
    // launch baseline (DLSS5_TEMPORAL / NR_LAYER_TEMPORAL / D5C_TEMPORAL,
    // parsed once in main) OR a live SETTEMPORAL push - re-reading env
    // here reverted a pushed "off" to the baseline on every rebuild (a
    // meta.temporal:false profile silently ran temporal ON again after
    // any extent/scale re-init). Same env-leash fix as chain_c_api's
    // temporal_ member.
    cfg.temporal = temporal;
    // Gain anchor: EMA smoothing of the global output/input gain.
    // DEFAULT ON (EngineConfig.gainLock = 0.08, bounded: deadzone +
    // 0.80..1.25 clamp) - the field-verified DC-breathing fix. Opt out
    // via DLSS5_GAIN_LOCK / NR_LAYER_GAIN_LOCK / D5C_GAIN_LOCK = 0.
    if (const char* genv = std::getenv("DLSS5_GAIN_LOCK")) cfg.gainLock = (float)std::atof(genv);
    else if ((genv = std::getenv("NR_LAYER_GAIN_LOCK")))    cfg.gainLock = (float)std::atof(genv);
    else if ((genv = std::getenv("D5C_GAIN_LOCK")))         cfg.gainLock = (float)std::atof(genv);
    if (cfg.gainLock < 0.0f) cfg.gainLock = 0.0f;
    cfg.debugDumps = dump;
    cfg.dumpDir = "out";
    cfg.vkCfg.appName = "m11d";
    cfg.profPath = benchProf;   // empty = profiling off (production default)
    return cfg;
}

// Engine allocation with the shutdown deleter - every swap/drop of the
// shared_ptr runs sehEngineShutdown once the last reference (a serving
// frame may hold one) releases it.
static std::shared_ptr<d5c::ChainEngine> newEnginePtr() {
    return std::shared_ptr<d5c::ChainEngine>(new d5c::ChainEngine(),
        [](d5c::ChainEngine* e) { sehEngineShutdown(e); delete e; });
}

// Synchronous path for one-shot modes only (selftest/bench). curW/curH==0
// is the single source of truth for "engine down": a failed init updates
// cfg_.renderScale internally, so the early-return must be gated on
// curW/curH or a dead engine would pass as healthy (permanent brick).
bool DaemonCore::ensureEngine(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(engMu_);
    if (curW && curH && w == curW && h == curH
        && engine && std::abs(engine->renderScale() - scale) < 1e-4f)
        return true;
    if (curW || curH)
        std::printf("[m11d] config change (%ux%u -> %ux%u, scale %.2f -> %.2f): engine re-init\n",
                    curW, curH, w, h,
                    engine ? (double)engine->renderScale() : 0.0, (double)scale);
    auto ne = newEnginePtr();
    if (!sehEngineInit(ne.get(), &makeEngineConfig(w, h, scale))) {
        std::fprintf(stderr, "[FAIL] engine init for %ux%u\n", w, h);
        engine.reset();         // outgoing engine out; next frame retries
        curW = 0; curH = 0;
        return false;
    }
    engine = std::move(ne);     // deleter shuts the outgoing engine down
    curW = w; curH = h;
    engine->setFrameGenMode(fgMode);
    return true;
}

// ------------------------------------------------------ async re-init ----
// The builder thread realizes pending_ configs: engine init is seconds of
// device + weights + arena work and MUST NOT run on the accept thread
// (frames AND NRCT queued behind it -> "daemon did not answer" bursts and
// manager restart loops). Requests coalesce in pending_: the newest
// {extent, scale} always wins.
void DaemonCore::kickBuildLocked(uint32_t w, uint32_t h) {
    pending_.reset(new PendingBuild{w, h, scale, buildSettleUntil_});
    if (builderRunning_) return;          // worker picks up newer pending_
    builderRunning_ = true;
    builder_ = std::thread(&DaemonCore::builderLoop, this);
    builder_.detach();                    // daemon runs for process lifetime;
    std::printf("[m11d] engine rebuild scheduled (%ux%u scale %.2f)\n",   // no join under engMu_ (deadlock)
                w, h, (double)scale);
}

void DaemonCore::builderLoop() {
    for (;;) {
        PendingBuild pb;
        {
            std::lock_guard<std::mutex> lk(engMu_);
            if (!pending_) { builderRunning_ = false; return; }
            pb = *pending_;
        }
        d5c::EngineConfig cfg;
        {
            std::lock_guard<std::mutex> lk(engMu_);
            cfg = makeEngineConfig(pb.w, pb.h, pb.scale);
        }
        auto ne = newEnginePtr();
        const ULONGLONG t0 = GetTickCount64();
        /* Post-fault settle: building during the driver's TDR recovery
         * produced an engine whose first dispatch faulted the device a
         * second time. Wait out the floor on this thread - the accept
         * loop keeps refusing frames meanwhile (engine stays null). */
        if (pb.notBefore > t0) {
            const ULONGLONG wait = pb.notBefore - t0;
            std::printf("[m11d] build settle %llu ms (post-fault)\n",
                        (unsigned long long)wait);
            Sleep((DWORD)wait);
        }
        std::printf("[m11d] async engine build start (%ux%u scale %.2f)\n",
                    pb.w, pb.h, (double)pb.scale);
        bool ok = sehEngineInit(ne.get(), &cfg);
        std::printf("[m11d] async engine build %s in %llu ms\n",
                    ok ? "done" : "FAILED",
                    (unsigned long long)(GetTickCount64() - t0));
        {
            std::lock_guard<std::mutex> lk(engMu_);
            if (ok) {
                engine = std::move(ne);   // outgoing engine survives via the
                curW = pb.w; curH = pb.h; // serving frame's shared_ptr ref
                havePrev = false;         // anchors die with the old engine
                // Knobs pushed mid-build: member state is always current,
                // the cfg was snapshotted before they landed - re-apply.
                engine->setHeadGain(gain);
                engine->setIntensity(p30Ctl.engineBlend());
                engine->setPhase30(p30Ctl.params());
                engine->setTemporal(temporal);
                engine->setSharpness(sharpness);
                engine->setFrameGenMode(fgMode);
                std::printf("[m11d] engine hot-swapped (%ux%u scale %.2f temporal %d)\n",
                            pb.w, pb.h, (double)pb.scale, temporal ? 1 : 0);
            }
            if (!ok) {
                // A build that keeps failing (bad weights, missing spv, a
                // driver that never comes back) is the same starvation loop
                // as a dead engine - every frame re-kicks a multi-second
                // build. Count it into the same bound.
                if (++deadEngines_ >= kDeadEngineLimit) {
                    std::fprintf(stderr, "[m11d] %d consecutive engine "
                                "failures - exiting\n", kDeadEngineLimit);
                    writeFenceFile();
                    std::fflush(stderr);
                    std::exit(4);
                }
            }
            // A newer request during the build overwrote pending_: only
            // clear it when it still asks for what we just built (or
            // failed to build - the next frame re-kicks a retry).
            if (pending_ && pending_->w == pb.w && pending_->h == pb.h
                && std::abs(pending_->scale - pb.scale) < 1e-4f)
                pending_.reset();
        }
    }
}

std::shared_ptr<d5c::ChainEngine> DaemonCore::requestEngine(uint32_t w,
                                                          uint32_t h) {
    std::lock_guard<std::mutex> lk(engMu_);
    const bool sameExtent = engine && curW == w && curH == h;
    const bool scaleDrift =
        std::abs((engine ? engine->renderScale() : scale) - scale) >= 1e-4f;
    if (sameExtent && !scaleDrift)
        return engine;
    // Engine missing, wrong extent, or wrong scale: schedule the rebuild.
    // A same-extent engine keeps serving at the OLD scale until the
    // replacement is hot-swapped in - a scale retune drops zero frames.
    kickBuildLocked(w, h);
    return sameExtent ? engine : nullptr;
}

// ------------------------------------------------------------ NRCT ctrl --
// M13 control channel: runtime parameters, handled between frames on the
// accept thread. engMu_ covers the whole dispatch: p30Ctl/member state
// must not race the builder thread's config snapshot, and the engine
// shared_ptr must not race a hot-swap. All engine setter calls are cheap
// cfg_ field writes. Reply: {magic, ok, valueBits, frames}.
void DaemonCore::handleCtrl(SOCKET s, const uint32_t hdr[4]) {
    uint32_t rep[4] = { MAGIC_CTRL, 0, 0, 0 };
    {
        // serveMu_ first: a ctrl request waits out an in-flight frame
        // (<= ~40 ms) instead of racing engine state mid-processFrame.
        std::lock_guard<std::mutex> slk(serveMu_);
        std::lock_guard<std::mutex> lk(engMu_);
        handleCtrlLocked(hdr, rep);
    }
    rep[3] = (uint32_t)frameCounter;
    writeAll(s, (const uint8_t*)rep, 16);
}

void DaemonCore::handleCtrlLocked(const uint32_t hdr[4], uint32_t rep[4]) {
    if (hdr[1] == 1) {                   // SETGAIN: hdr[2] = float bits
        float g = 0.0f;                  // -> residual multiplier (headGain),
        std::memcpy(&g, &hdr[2], 4);     //    same slot as dlss5_set_gain on x64
        if (g >= 0.0f && g <= 16.0f) {
            gain = g;
            if (engine) engine->setHeadGain(g);  // next processed frame uses it
            rep[1] = 1;
            std::printf("[m11d] ctrl: gain -> %.3f\n", (double)g);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: gain %.3f out of range\n", (double)g);
        }
    } else if (hdr[1] == 15) {           // SETINTENSITY: hdr[2] = float bits
        float v = 0.0f;                  // -> Phase30 compose mix (pc.c.w)
        std::memcpy(&v, &hdr[2], 4);
        if (v >= 0.0f && v <= 2.0f) {
            blend = v;
            p30Ctl.setIntensity(v);
            if (engine) engine->setIntensity(v); // next processed frame uses it
            rep[1] = 1;
            std::printf("[m11d] ctrl: intensity -> %.3f\n", (double)v);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: intensity %.3f out of range\n", (double)v);
        }
    } else if (hdr[1] == 3) {            // SETBLEND: hdr[2] = float bits -> same
        float b = 0.0f;                  // compose mix slot as intensity (pc.c.w)
        std::memcpy(&b, &hdr[2], 4);
        if (b >= 0.0f && b <= 2.0f) {
            blend = b;
            p30Ctl.setIntensity(b);
            if (engine) engine->setIntensity(b);
            rep[1] = 1;
            std::printf("[m11d] ctrl: blend/intensity -> %.3f\n", (double)b);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: blend %.3f out of range\n", (double)b);
        }
    } else if (hdr[1] == 2) {            // STATUS
        // ok=1 when an engine is resident OR a build is in flight: the
        // manager restarts a daemon it believes dead, and killing a
        // mid-init daemon is exactly the restart loop this async model
        // exists to prevent. Only a dead engine with no pending build
        // reports dead.
        rep[1] = (engine || pending_) ? 1 : 0;
    } else if (hdr[1] == 4) {            // SETSCALE: hdr[2] = float bits
        float s = 0.0f;
        std::memcpy(&s, &hdr[2], 4);
        if (s >= 0.20f && s <= 1.5f) {
            scale = s;
            rep[1] = 1;                  // accepted; the rebuild runs async -
            if (curW && curH)            // the old engine keeps serving at the
                kickBuildLocked(curW, curH);  // old scale until the swap
            havePrev = false;            // anchors die with the old engine
                                         // (harmless if none ran)
            std::printf("[m11d] ctrl: scale -> %.2f (reinit scheduled)\n",
                        (double)s);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: scale %.2f out of range\n", (double)s);
        }
    } else if (hdr[1] == 5) {            // SETFRAMEGEN (on/off; mode preserved)
        fgMode = (hdr[2] != 0) ? (fgModeConfigured ? fgModeConfigured : 1) : 0;
        if (!fgMode) havePrev = false;
        if (engine) engine->setFrameGenMode(fgMode);
        rep[1] = 1;
        std::printf("[m11d] ctrl: framegen -> %d\n", fgMode);
    } else if (hdr[1] == 6) {            // SETLOCALTONE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setLocalTone(v);
        if (engine) engine->setLocalTone(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: local_tone -> %.3f\n", (double)v);
    } else if (hdr[1] == 7) {            // SETLOCALSTRUCTURE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setLocalStructure(v);
        if (engine) engine->setLocalStructure(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: local_structure -> %.3f\n", (double)v);
    } else if (hdr[1] == 8) {            // SETSKINSTRUCTURE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setSkinStructure(v);
        if (engine) engine->setSkinStructure(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: skin_structure -> %.3f\n", (double)v);
    } else if (hdr[1] == 9) {            // SETAUTOMASK: hdr[2] = int (0/1)
        bool v = (hdr[2] != 0);
        p30Ctl.setAutoMask(v);
        if (engine) engine->setAutoMask(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: auto_mask -> %d\n", v ? 1 : 0);
    } else if (hdr[1] == 10) {           // SETSTYLEINDEX: hdr[2] = int (0/1/2)
        int v = (int)hdr[2];
        p30Ctl.setStyleIndex(v);
        if (engine) engine->setStyleIndex(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: style_index -> %d\n", v);
    } else if (hdr[1] == 11) {           // SETCOLORSTRENGTH: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setColorStrength(v);
        if (engine) engine->setColorStrength(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: color_strength -> %.3f\n", (double)v);
    } else if (hdr[1] == 12) {           // RESETHIST: drop temporal history
        if (engine) engine->resetHistory();  // (next frame = first-frame path;
        havePrev = false;                //  photo batches / stream switches;
        rep[1] = 1;                      //  the stale prev pair must not
        std::printf("[m11d] ctrl: temporal history reset\n");   //  anchor a post-reset transfer either)
    } else if (hdr[1] == 13) {           // SETTEMPORAL: hdr[2] = int (0/1)
        // Member FIRST, then the live engine: an engine-only write was
        // lost on the next async rebuild (makeEngineConfig re-read the
        // env baseline), so a meta.temporal:false profile popped back to
        // temporal ON mid-session - a visible look change. Now the member
        // feeds cfg.temporal and the post-swap re-apply.
        temporal = (hdr[2] != 0);
        if (engine) engine->setTemporal(temporal);
        rep[1] = 1;
        std::printf("[m11d] ctrl: temporal -> %d\n", temporal ? 1 : 0);
    } else if (hdr[1] == 14) {           // SETFGMODE: hdr[2] = 0/1/2
        int m = (int)hdr[2];
        if (m < 0 || m > 2) m = 1;
        fgMode = m;
        fgModeConfigured = m;
        if (!fgMode) havePrev = false;
        if (engine) engine->setFrameGenMode(fgMode);
        rep[1] = 1;
        std::printf("[m11d] ctrl: fg mode -> %d\n", fgMode);
    } else {
        std::fprintf(stderr, "[m11d] ctrl: unknown cmd %u\n", hdr[1]);
    }
    // rep[2] echoes the knob the command touched (or the residual gain
    // for STATUS and valueless commands - the field is named "gain" in
    // the protocol and the manager displays it as such).
    if (hdr[1] == 3 || hdr[1] == 15) {
        float v = p30Ctl.params().intensity;
        std::memcpy(&rep[2], &v, 4);
    } else {
        std::memcpy(&rep[2], &gain, 4);
    }
}

// ------------------------------------------------------------- frame path --
bool DaemonCore::handleFrame(SOCKET s, const uint32_t hdr[4], ULONGLONG deadline,
                             std::vector<uint8_t>& inBuf, std::vector<uint8_t>& outBuf,
                             std::vector<uint8_t>& maskBuf) {
    const bool masked = hdr[0] == MAGIC_MASKED;
    const bool fgReq  = hdr[0] == MAGIC_FGT;
    const bool fgiReq = hdr[0] == MAGIC_FGI;
    if (hdr[0] != MAGIC_PLAIN && !masked && !fgReq && !fgiReq) {
        std::fprintf(stderr, "[m11d] bad magic 0x%08x - dropping connection\n", hdr[0]);
        return false;
    }
    const uint32_t w = hdr[1], h = hdr[2];
    const uint32_t fmt = hdr[3];
    const size_t px = (size_t)w * h;
    if (!w || !h || px > 16384ull * 16384ull) {
        std::fprintf(stderr, "[m11d] insane extent %ux%u - dropping\n", w, h);
        return false;
    }
    // Only B8G8R8A8 wire order is valid: the shaders decode B,G,R,A bytes.
    // An R8G8B8A8 (37/43) or packed-10 swapchain would come back channel-
    // swapped/garbled and be presented as a "processed" frame - fail closed.
    if (fmt != 44 && fmt != 50) {
        std::fprintf(stderr, "[m11d] VkFormat %u is not B8G8R8A8 - dropping "
                     "(layer must gate non-BGRA formats)\n", fmt);
        return false;
    }
    // Engine acquisition BEFORE the payload read: when nothing usable
    // exists the async builder is already running - closing now lets the
    // layer's send hit RST and it holds its last processed frame in ~ms
    // instead of stalling inside a multi-second init (the old synchronous
    // ensureEngine wedged the whole accept loop: frames AND NRCT queued
    // behind a 3-14 s build -> "daemon did not answer" + restart loops).
    std::shared_ptr<d5c::ChainEngine> eng = requestEngine(w, h);
    if (!eng)
        return false;
    // Live knobs ride the frame channel. Env is spawn-frozen and already
    // applied at startup: re-reading it per frame reverted live knob
    // pushes to the launch baseline (the "env leash").
    {
        std::lock_guard<std::mutex> lk(engMu_);
        if (p30Ctl.pollKnobsFile()) {
            // Engine knobs riding the same file: residual gain + compose
            // blend (same slot family as the x64 layer pushes via the C
            // ABI). Blend and intensity both write the compose-mix slot
            // (pc.c.w): apply blend via setIntensity FIRST so the Phase30
            // "intensity" key wins deterministically when both are
            // present (same precedence as the x64 path).
            eng->setIntensity(p30Ctl.engineBlend());
            eng->setHeadGain(p30Ctl.engineGain());
            eng->setPhase30(p30Ctl.params());
            // Encode CAS sharpness rides the same file now (x64/m12
            // paths parsed it all along; the daemon never did - the
            // slider was dead on GTA-IV-class x86 titles). -1 sentinel
            // means the file never carried the key: keep the env
            // baseline member in that case.
            if (p30Ctl.hasEngineSharpness()) {
                eng->setSharpness(p30Ctl.engineSharpness());
                sharpness = p30Ctl.engineSharpness();
            }
            // Mirror the pushed knobs into the members makeEngineConfig
            // reads: without the sync a knob-file gain/blend push rode
            // the live engine only and silently reverted to the stale
            // member on the next rebuild (same leak class as SETTEMPORAL).
            gain = p30Ctl.engineGain();
            blend = p30Ctl.engineBlend();
        }
    }
    inBuf.resize(px * 4);
    outBuf.resize(px * 4);
    if (!readAllDeadline(s, inBuf.data(), px * 4, deadline))
        return false;
    if (masked) {   // held-still UI mask: read and (v0) ignore
        maskBuf.resize(px);
        if (!readAllDeadline(s, maskBuf.data(), px, deadline))
            return false;
    }
    // Everything below owns the engine in arrival order: gap policy,
    // prevW/prevH/havePrev/lastFrameTick/frameCounter, the process/FG
    // dispatch and the fault path are shared DaemonCore state. Reads
    // above stay outside so a stalled client cannot hold the engine
    // hostage for its 8 s deadline. (serveMu_ -> engMu_ order is kept.)
    std::lock_guard<std::mutex> serveLk(serveMu_);
    if (w != prevW || h != prevH) {
        havePrev = false;
        prevW = w;
        prevH = h;
    }
    // Frame-gap policy (GTA IV stalls 2-4 s during world streaming - a
    // history reset per hitch was the field report's "periodic bright
    // pop": one un-damped brighter first frame + a cold gain EMA):
    //  >2 s  - drop the FG anchor pair only; a seconds-stale prev frame
    //          warps garbage in residual transfer/interpolation - the
    //          motion product is genuinely dead. Temporal history stays:
    //          features.comp already AABB-clamps the history input into
    //          the current 3x3 colour neighbourhood EVERY frame, so a
    //          stale history cannot drag beyond current content, and
    //          temporal_nr's gain EMA snaps through real scene changes
    //          (>50% residual jump). A wall-clock reset on top of that
    //          was redundant protection with a visible cost.
    //  >60 s - the stream is almost certainly dead (game exited, or a
    //          different title presenting on the same extent): drop
    //          history too. Pure backstop - it must NOT fire on hitches,
    //          load screens or alt-tabs; deliberate content switches use
    //          RESETHIST (cmd 12).
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG gap = lastFrameTick ? now - lastFrameTick : 0;
        if (gap > 60000) {
            eng->resetHistory();
            havePrev = false;
            std::printf("[m11d] stream gap %llu ms - temporal history reset\n",
                        (unsigned long long)gap);
        } else if (gap > 2000) {
            havePrev = false;   // stale anchors must not warp a
                                // transfer/interp across a hitch
            std::printf("[m11d] frame gap %llu ms - FG anchors dropped\n",
                        (unsigned long long)gap);
        }
        lastFrameTick = now;
    }
    d5c::FrameStats st{};
    ++frameCounter;
    auto t0 = std::chrono::steady_clock::now();

    // NRN2 (FGTRANSFER, mode 1): residual-transfer framegen. NRN3
    // (FGINTERP, mode 2): midpoint of the last two processed anchors
    // - the payload is the REAL middle frame the layer captured at
    // the previous mid present, not the current raw. Everything else
    // - first frame, post-reset/gap, engine re-init, FG toggled off,
    // or a refused/faulted transfer - falls back to a full process so
    // the reply is always a presentable frame (and rotates anchors
    // as a side effect).
    int generated = 0;   // 0=processed, 1=transferred, 2=interpolated
    bool ok;
    if (fgiReq && fgMode >= 2 && havePrev) {
        ok = sehInterpAnchors(eng.get(), w, h, inBuf.data(), outBuf.data(), &st);
        if (ok) {
            generated = 2;
        } else {
            std::fprintf(stderr, "[m11d] interpolateAnchors failed - processing instead\n");
            ok = sehProcessFrame(eng.get(), inBuf.data(), outBuf.data(), frameCounter, &st);
        }
    } else if (fgReq && fgMode && havePrev) {
        ok = sehTransferFrame(eng.get(), w, h, inBuf.data(), outBuf.data(), &st);
        if (ok) {
            generated = 1;
        } else {
            std::fprintf(stderr, "[m11d] transferFrame failed - processing instead\n");
            ok = sehProcessFrame(eng.get(), inBuf.data(), outBuf.data(), frameCounter, &st);
        }
    } else {
        // Clean forward processing for every frame via DLSS 5 engine
        ok = sehProcessFrame(eng.get(), inBuf.data(), outBuf.data(), frameCounter, &st);
    }
    double wall = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
        // Do NOT echo the input: a "successful" raw reply is presented
        // as a processed frame by the layer - a quality strobe.
        // Closing without a reply makes the layer treat this as a
        // transport failure and hold the last processed frame.
        std::fprintf(stderr, "[m11d] %s FAILED - closing, no reply\n",
                    fgiReq ? "fginterp/process" :
                    fgReq ? "fgtransfer/process" : "processFrame");
        havePrev = false;   // a torn frame can leave the prev pair dirty
        // A mid-frame fault (device lost etc.) leaves the engine
        // half-dead: dropping the shared_ptr forces a full re-init on
        // the next frame instead of fail-closing on this extent forever.
        // Our local `eng` keeps the corpse alive until this frame is
        // done; the deleter runs its shutdown off the accept hot path.
        {
            std::lock_guard<std::mutex> lk(engMu_);
            if (engine == eng) {        // a swap may already have replaced it
                engine.reset();
                curW = 0; curH = 0;
            }
            // Sliding-window fault count FIRST - it also feeds the settle
            // escalation below (deadEngines_ alone resets on every served
            // frame and never accumulates during a storm).
            ULONGLONG nowMs = GetTickCount64();
            if (nowMs - lastFaultAt_ > kFaultWindowMs) windowFaults_ = 0;
            lastFaultAt_ = nowMs;
            ++windowFaults_;
            /* The fault means the driver just went through a TDR; the
             * device it hands out for the next seconds is not guaranteed
             * stable. Every rebuild kicked from now on first waits out
             * the settle floor (cleared on the next clean frame). */
            buildSettleUntil_ = nowMs + 4000 * std::min<int64_t>(
                std::max(deadEngines_, windowFaults_) + 1, 5);
        }
        // A fault that survives a full rebuild is not transient (driver
        // reset loop, deterministic dispatch fault): each retry costs the
        // game ~3.5 s of GPU at the moment it needs it most. Bound it -
        // either streak (no good frame between losses) or the sliding
        // window (losses keep coming around served frames) trips the exit,
        // and the layer's passthrough gives the device back to the game.
        ++deadEngines_;
        if (deadEngines_ >= kDeadEngineLimit
                || windowFaults_ >= kWindowFaultLimit) {
            std::fprintf(stderr, "[m11d] engine-loss bound hit (streak %d, "
                        "window %d in %llus) - exiting so the game "
                        "reclaims the GPU\n",
                        deadEngines_, windowFaults_,
                        (unsigned long long)(kFaultWindowMs / 1000));
            writeFenceFile();
            std::fflush(stderr);
            std::exit(4);
        }
        return false;
    }
    deadEngines_ = 0;   // a served frame proves the current engine healthy
    {
        std::lock_guard<std::mutex> lk(engMu_);
        buildSettleUntil_ = 0;          // device is demonstrably stable again
    }
    if (!generated) {
        // Only a real process refreshes the prev pair - a
        // generated frame is not a reconstruction and must stay
        // out of the transfer baseline (x64 parity). The pair
        // itself lives engine-side (bufPrevFrame_/bufPrevRaw_,
        // written GPU-side inside processFrame); havePrev is only
        // the daemon-side gate for the NRN2 path - no host copy.
        havePrev = true;
    }
    // vendor parity (nr_daemon.py:88): alpha stays as the game
    // left it (encode forces opaque for the overlay use-case);
    // held-still UI pixels (mask) pass through untouched. For a
    // generated frame the loop is a byte-exact no-op: framegen
    // mode 1 already packs the current raw alpha (cur.a).
    // Dword-wise form: identical bytes, ~4x fewer operations, and the
    // unmasked case vectorizes (single linear pass over 1.9 MB each way).
    {
        const uint32_t* in32 = (const uint32_t*)inBuf.data();
        uint32_t* out32 = (uint32_t*)outBuf.data();
        if (!masked) {
            for (size_t i = 0; i < px; ++i)
                out32[i] = (out32[i] & 0x00FFFFFFu) | (in32[i] & 0xFF000000u);
        } else {
            for (size_t i = 0; i < px; ++i)
                out32[i] = maskBuf[i] ? in32[i]
                                      : ((out32[i] & 0x00FFFFFFu) | (in32[i] & 0xFF000000u));
        }
    }
    // Diagnostics: M11D_DUMP=<dir> dumps every in/out pair as <dir>\in_NNNNN.raw /
    // out_NNNNN.raw (16-byte NRN0 header + w*h*4 payload). Field tool only -
    // ~4 MB/frame at 800x600, never enable in production runs.
    {
        static const char* const dd = std::getenv("M11D_DUMP");
        if (dd && *dd) {
            char path[MAX_PATH];
            const uint32_t oh[4] = { hdr[0], w, h, fmt };
            std::snprintf(path, sizeof path, "%s\\in_%05lu.raw", dd,
                          (unsigned long)frameCounter);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(oh, 4, 4, f);
                std::fwrite(inBuf.data(), 1, px * 4, f);
                std::fclose(f);
            }
            std::snprintf(path, sizeof path, "%s\\out_%05lu.raw", dd,
                          (unsigned long)frameCounter);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(oh, 4, 4, f);
                std::fwrite(outBuf.data(), 1, px * 4, f);
                std::fclose(f);
            }
        }
    }
    if (!writeAll(s, outBuf.data(), px * 4)) {
        std::fprintf(stderr, "[m11d] reply write failed\n");
        return false;
    }
    // Stage timing in the chain parens: fe=features fp=featpack chain=net
    // tail=encode/compose (engine GPU timestamps; sums < wall = host side:
    // TCP recv + staging upload + reply write).
    std::printf("[m11d] frame %ld %ux%u%s%s in %.1f ms (%s fe %.1f fp %.1f c %.1f tail %.1f)\n",
                frameCounter, w, h, masked ? " +mask" : "",
                generated == 2 ? " +fgi" :
                generated == 1 ? " +fg" :
                ((fgReq || fgiReq) ? " +fg-fallback" : ""),
                wall,
                generated == 2 ? "interp" :
                generated == 1 ? "transfer" : "chain",
                st.feMs, st.fpMs,
                generated ? st.gpuTotalMs : st.chainMs, st.tailMs);
    return true;
}
