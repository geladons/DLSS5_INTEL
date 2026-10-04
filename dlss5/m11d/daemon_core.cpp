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

#include "daemon_io.h"
#include "engine_seh.h"

static const uint32_t MAGIC_PLAIN = 0x304E524Eu;
static const uint32_t MAGIC_MASKED = 0x314E524Eu;
static const uint32_t MAGIC_FGT = 0x324E524Eu;    // "NRN2" - framegen transfer
static const uint32_t MAGIC_FGI = 0x334E524Eu;    // "NRN3" - framegen interpolate (mode 2)
static const uint32_t MAGIC_CTRL = 0x5443524Eu;   // "NRCT" - M13 control

// ------------------------------------------------------ engine lifecycle --
bool DaemonCore::ensureEngine(uint32_t w, uint32_t h) {
    // curW/curH == 0 is the single source of truth for "engine down":
    // a failed init updates cfg_.renderScale internally, so the
    // early-return must be gated on curW/curH or a dead engine would
    // pass as healthy for the current extent (permanent brick).
    if (curW && curH && w == curW && h == curH
        && std::abs(engine.renderScale() - scale) < 1e-4f)
        return true;
    if (curW || curH)
        std::printf("[m11d] config change (%ux%u -> %ux%u, scale %.2f -> %.2f): engine re-init\n",
                    curW, curH, w, h, (double)engine.renderScale(), (double)scale);
    sehEngineShutdown(&engine);   // unconditional: half-built init state
                                  // must not leak a device into a retry
    curW = 0; curH = 0;
    d5c::EngineConfig cfg{};
    cfg.canvasW = w; cfg.canvasH = h;
    cfg.weightsPath = weights;
    cfg.shaderDir = shaderDir;        // exe-relative unless overridden
    cfg.headGain = 1.0f;
    cfg.headBlend = p30Ctl.params().intensity;
    cfg.renderScale = scale;
    cfg.phase30 = p30Ctl.params();
    // Temporal conditioning default ON (the flicker fix): history anchors
    // each reconstruction to the previous output. DLSS5_TEMPORAL=0 /
    // NR_LAYER_TEMPORAL=0 / D5C_TEMPORAL=0 disables for A/B and
    // determinism tests (same env names as chain_c_api).
    const char* tenv = std::getenv("DLSS5_TEMPORAL");
    if (!tenv) tenv = std::getenv("NR_LAYER_TEMPORAL");
    if (!tenv) tenv = std::getenv("D5C_TEMPORAL");
    cfg.temporal = !(tenv && std::strcmp(tenv, "0") == 0);
    // Gain anchor: EMA smoothing of the global output/input gain
    // (default 0.08). DLSS5_GAIN_LOCK / NR_LAYER_GAIN_LOCK / D5C_GAIN_LOCK.
    if (const char* genv = std::getenv("DLSS5_GAIN_LOCK")) cfg.gainLock = (float)std::atof(genv);
    else if ((genv = std::getenv("NR_LAYER_GAIN_LOCK")))    cfg.gainLock = (float)std::atof(genv);
    else if ((genv = std::getenv("D5C_GAIN_LOCK")))         cfg.gainLock = (float)std::atof(genv);
    if (cfg.gainLock < 0.0f) cfg.gainLock = 0.0f;
    cfg.debugDumps = dump;
    cfg.dumpDir = "out";
    cfg.vkCfg.appName = "m11d";
    cfg.profPath = benchProf;   // empty = profiling off (production default)
    // headless: no surface/swapchain extensions, first coopmat-capable device
    if (!sehEngineInit(&engine, &cfg)) {
        std::fprintf(stderr, "[FAIL] engine init for %ux%u\n", w, h);
        return false;   // curW/curH stay 0 -> the next frame re-inits clean
    }
    curW = w; curH = h;
    engine.setFrameGenMode(fgMode >= 2 ? 2 : 1);
    return true;
}

// ------------------------------------------------------------ NRCT ctrl --
// M13 control channel: runtime parameters, handled between frames on the
// accept thread (the engine is driven from this thread only, so no locking
// is needed). Reply: {magic, ok, gainBits, frames}.
void DaemonCore::handleCtrl(SOCKET s, const uint32_t hdr[4]) {
    uint32_t rep[4] = { MAGIC_CTRL, 0, 0, 0 };
    if (hdr[1] == 1) {                   // SETGAIN / SETINTENSITY: hdr[2] = float bits
        float g = 0.0f;
        std::memcpy(&g, &hdr[2], 4);
        if (g >= 0.0f && g <= 16.0f) {
            gain = g;
            blend = g;
            p30Ctl.setIntensity(g);
            engine.setIntensity(g);       // next processed frame uses it
            rep[1] = 1;
            std::printf("[m11d] ctrl: intensity/gain -> %.3f\n", (double)g);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: gain %.3f out of range\n", (double)g);
        }
    } else if (hdr[1] == 3) {            // SETBLEND / SETINTENSITY: hdr[2] = float bits
        float b = 0.0f;
        std::memcpy(&b, &hdr[2], 4);
        if (b >= 0.0f && b <= 2.0f) {
            blend = b;
            p30Ctl.setIntensity(b);
            engine.setIntensity(b);
            rep[1] = 1;
            std::printf("[m11d] ctrl: blend/intensity -> %.3f\n", (double)b);
        } else {
            std::fprintf(stderr, "[m11d] ctrl: blend %.3f out of range\n", (double)b);
        }
    } else if (hdr[1] == 2) {            // STATUS
        // ok=1 only when the engine is actually up - a dead engine
        // must not masquerade as a healthy daemon to the manager.
        rep[1] = (curW && curH) ? 1 : 0;
    } else if (hdr[1] == 4) {            // SETSCALE: hdr[2] = float bits
        float s = 0.0f;
        std::memcpy(&s, &hdr[2], 4);
        if (s >= 0.20f && s <= 1.5f) {
            scale = s;
            rep[1] = 1;
            if (curW && curH)
                rep[1] = ensureEngine(curW, curH) ? 1 : 0;
            havePrev = false;            // a re-init wiped the prev pair
                                         // (harmless if none ran)
            std::printf("[m11d] ctrl: scale -> %.2f (reinit %s)\n",
                        (double)s, rep[1] ? "ok" : "FAILED");
        } else {
            std::fprintf(stderr, "[m11d] ctrl: scale %.2f out of range\n", (double)s);
        }
    } else if (hdr[1] == 5) {            // SETFRAMEGEN (on/off; mode preserved)
        fgMode = (hdr[2] != 0) ? (fgModeConfigured ? fgModeConfigured : 1) : 0;
        if (!fgMode) havePrev = false;
        engine.setFrameGenMode(fgMode >= 2 ? 2 : 1);
        rep[1] = 1;
        std::printf("[m11d] ctrl: framegen -> %d\n", fgMode);
    } else if (hdr[1] == 6) {            // SETLOCALTONE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setLocalTone(v);
        engine.setLocalTone(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: local_tone -> %.3f\n", (double)v);
    } else if (hdr[1] == 7) {            // SETLOCALSTRUCTURE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setLocalStructure(v);
        engine.setLocalStructure(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: local_structure -> %.3f\n", (double)v);
    } else if (hdr[1] == 8) {            // SETSKINSTRUCTURE: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setSkinStructure(v);
        engine.setSkinStructure(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: skin_structure -> %.3f\n", (double)v);
    } else if (hdr[1] == 9) {            // SETAUTOMASK: hdr[2] = int (0/1)
        bool v = (hdr[2] != 0);
        p30Ctl.setAutoMask(v);
        engine.setAutoMask(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: auto_mask -> %d\n", v ? 1 : 0);
    } else if (hdr[1] == 10) {           // SETSTYLEINDEX: hdr[2] = int (0/1/2)
        int v = (int)hdr[2];
        p30Ctl.setStyleIndex(v);
        engine.setStyleIndex(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: style_index -> %d\n", v);
    } else if (hdr[1] == 11) {           // SETCOLORSTRENGTH: hdr[2] = float bits
        float v = 0.0f;
        std::memcpy(&v, &hdr[2], 4);
        p30Ctl.setColorStrength(v);
        engine.setColorStrength(v);
        rep[1] = 1;
        std::printf("[m11d] ctrl: color_strength -> %.3f\n", (double)v);
    } else if (hdr[1] == 12) {           // RESETHIST: drop temporal history
        engine.resetHistory();           // (next frame = first-frame path;
        havePrev = false;                //  photo batches / stream switches;
        rep[1] = 1;                      //  the stale prev pair must not
        std::printf("[m11d] ctrl: temporal history reset\n");   //  anchor a post-reset transfer either)
    } else if (hdr[1] == 13) {           // SETTEMPORAL: hdr[2] = int (0/1)
        engine.setTemporal(hdr[2] != 0);
        rep[1] = 1;
        std::printf("[m11d] ctrl: temporal -> %d\n", hdr[2] ? 1 : 0);
    } else if (hdr[1] == 14) {           // SETFGMODE: hdr[2] = 0/1/2
        int m = (int)hdr[2];
        if (m < 0 || m > 2) m = 1;
        fgMode = m;
        fgModeConfigured = m;
        if (!fgMode) havePrev = false;
        engine.setFrameGenMode(fgMode >= 2 ? 2 : 1);
        rep[1] = 1;
        std::printf("[m11d] ctrl: fg mode -> %d\n", fgMode);
    } else {
        std::fprintf(stderr, "[m11d] ctrl: unknown cmd %u\n", hdr[1]);
    }
    float curInten = p30Ctl.params().intensity;
    std::memcpy(&rep[2], &curInten, 4);
    rep[3] = (uint32_t)frameCounter;
    writeAll(s, (const uint8_t*)rep, 16);
}

// ------------------------------------------------------------- frame path --
void DaemonCore::handleFrame(SOCKET s, const uint32_t hdr[4], ULONGLONG deadline,
                             std::vector<uint8_t>& inBuf, std::vector<uint8_t>& outBuf,
                             std::vector<uint8_t>& maskBuf) {
    // Live knobs ride the frame channel (once per connection, same as the
    // old accept loop) - NRCT requests bypass it by construction.
    if (p30Ctl.pollKnobsFile() || p30Ctl.loadFromEnv()) {
        engine.setPhase30(p30Ctl.params());
    }
    const bool masked = hdr[0] == MAGIC_MASKED;
    const bool fgReq  = hdr[0] == MAGIC_FGT;
    const bool fgiReq = hdr[0] == MAGIC_FGI;
    if (hdr[0] != MAGIC_PLAIN && !masked && !fgReq && !fgiReq) {
        std::fprintf(stderr, "[m11d] bad magic 0x%08x - dropping connection\n", hdr[0]);
        return;
    }
    const uint32_t w = hdr[1], h = hdr[2];
    const uint32_t fmt = hdr[3];
    const size_t px = (size_t)w * h;
    if (!w || !h || px > 16384ull * 16384ull) {
        std::fprintf(stderr, "[m11d] insane extent %ux%u - dropping\n", w, h);
        return;
    }
    inBuf.resize(px * 4);
    outBuf.resize(px * 4);
    if (!readAllDeadline(s, inBuf.data(), px * 4, deadline))
        return;
    if (masked) {   // held-still UI mask: read and (v0) ignore
        maskBuf.resize(px);
        if (!readAllDeadline(s, maskBuf.data(), px, deadline))
            return;
    }
    // Only B8G8R8A8 wire order is valid: the shaders decode B,G,R,A bytes.
    // An R8G8B8A8 (37/43) or packed-10 swapchain would come back channel-
    // swapped/garbled and be presented as a "processed" frame - fail closed.
    if (fmt != 44 && fmt != 50) {
        std::fprintf(stderr, "[m11d] VkFormat %u is not B8G8R8A8 - dropping "
                     "(layer must gate non-BGRA formats)\n", fmt);
        return;
    }

    if (!ensureEngine(w, h)) {
        std::fprintf(stderr, "[m11d] engine down - closing, no reply\n");
        return;
    }
    if (w != prevW || h != prevH) {
        havePrev = false;
        prevW = w;
        prevH = h;
    }
    // Two gap thresholds (GTA IV stalls 2-4 s during world
    // streaming - treating those as new streams emitted one
    // un-damped brighter first-frame per hitch, the field report's
    // "periodic bright pop"):
    //  >2 s  - drop the FG anchor pair only; a seconds-stale prev
    //          frame warps garbage in residual transfer, while the
    //          AABB-clamped temporal history is safe to keep.
    //  >10 s - a real new stream (restart, alt-tab, other game):
    //          reset history too.
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG gap = lastFrameTick ? now - lastFrameTick : 0;
        if (gap > 10000) {
            engine.resetHistory();
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
        ok = sehInterpAnchors(&engine, w, h, inBuf.data(), outBuf.data(), &st);
        if (ok) {
            generated = 2;
        } else {
            std::fprintf(stderr, "[m11d] interpolateAnchors failed - processing instead\n");
            ok = sehProcessFrame(&engine, inBuf.data(), outBuf.data(), frameCounter, &st);
        }
    } else if (fgReq && fgMode && havePrev) {
        ok = sehTransferFrame(&engine, w, h, inBuf.data(), outBuf.data(), &st);
        if (ok) {
            generated = 1;
        } else {
            std::fprintf(stderr, "[m11d] transferFrame failed - processing instead\n");
            ok = sehProcessFrame(&engine, inBuf.data(), outBuf.data(), frameCounter, &st);
        }
    } else {
        // Clean forward processing for every frame via DLSS 5 engine
        ok = sehProcessFrame(&engine, inBuf.data(), outBuf.data(), frameCounter, &st);
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
        // half-dead: zeroing the extent forces a full re-init on the
        // next frame instead of fail-closing on this extent forever.
        curW = 0; curH = 0;
        return;
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
    for (size_t i = 0; i < px; ++i) {
        if (masked && maskBuf[i]) {
            std::memcpy(&outBuf[i * 4], &inBuf[i * 4], 4);
        } else {
            outBuf[i * 4 + 3] = inBuf[i * 4 + 3];
        }
    }
    if (!writeAll(s, outBuf.data(), px * 4))
        std::fprintf(stderr, "[m11d] reply write failed\n");
    std::printf("[m11d] frame %ld %ux%u%s%s in %.1f ms (%s %.1f)\n",
                frameCounter, w, h, masked ? " +mask" : "",
                generated == 2 ? " +fgi" :
                generated == 1 ? " +fg" :
                ((fgReq || fgiReq) ? " +fg-fallback" : ""),
                wall,
                generated == 2 ? "interp" :
                generated == 1 ? "transfer" : "chain",
                generated ? st.gpuTotalMs : st.chainMs);
}
