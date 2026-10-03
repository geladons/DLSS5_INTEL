// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - DLSSNR frame daemon (M11): TCP server on 127.0.0.1 (default 48990)
// driving the real 71-block DLSS 5 U-Net via dlss5/chain (ChainEngine).
//
// Protocol (identical to reference nr_layer.c / nr_layer_win.c):
//   request: 16-byte header {magic, w, h, format} (uint32 LE)
//            magic 0x304E524E: payload w*h*4 bytes (BGRA)
//            magic 0x314E524E: payload w*h*4 + w*h mask bytes (1 = held-still UI)
//            magic 0x324E524E ("NRN2"): frame-generation transfer request -
//                     same wire shape as 0x304E524E (no mask), but the frame
//                     is GENERATED via ChainEngine::transferFrame instead of
//                     fully processed: generated = cur + warp(prevProcessed -
//                     prevRaw, motion(prevRaw->cur)) using the prev pair the
//                     resident engine keeps from the last processed frame
//                     (bufPrevFrame_/bufPrevRaw_). When no usable prev pair
//                     exists (stream start, post-reset, re-init) or FG is
//                     disabled (cmd 5 / --framegen 0) the request degrades to
//                     a normal process - the reply is always a presentable
//                     frame. Temporal history (bufHist_) is untouched by a
//                     transfer, so generated frames never anchor the chain.
//            magic 0x5443524E ("NRCT"): M13 control channel, 16-byte header
//                     {magic, cmd, payload, reserved} instead of a frame:
//                     cmd 1 = SETGAIN (payload = float bits, reply ok+gain)
//                     cmd 2 = STATUS  (reply ok, gain, frames processed)
//                     cmd 3 = SETBLEND(payload = float bits 0..1, reply ok+gain)
//                     cmd 4 = SETSCALE(payload = float bits 0.2..1.5, reply ok+gain)
//                     cmd 12 = RESETHIST (temporal history drop; next frame
//                              takes the first-frame path - photo batches,
//                              stream switches; also drops the FG prev pair)
//                     cmd 13 = SETTEMPORAL (payload = int 0/1)
//   reply:   frame requests: w*h*4 bytes BGRA (the processed or generated
//            frame); control requests: 16 bytes {magic, ok, gainBits, frames}
// One TCP connection per frame (the layer connect()s per exchange).
//
// Modes:
//   (daemon, default)     m11d [--port N] [--gain F] [--dump]
//   one-shot validation:  m11d --selftest file.bmp [--gain F]
//                         -> out\selftest_out.bmp + live_* dumps (frame index 1,
//                         matching the torch goldens in work/_m9b_cmp)
//   M10 benchmark:        m11d --bench N WxH [--prof out.csv]
//                         synthetic frames, no sockets; prints per-frame and
//                         median wall/gpu/chain ms; --prof adds per-dispatch CSV
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <string>
#include <vector>

#include "engine.h"
#include "phase30.h"

static const uint32_t MAGIC_PLAIN = 0x304E524Eu;
static const uint32_t MAGIC_MASKED = 0x314E524Eu;
static const uint32_t MAGIC_FGT = 0x324E524Eu;    // "NRN2" - framegen transfer
static const uint32_t MAGIC_FGI = 0x334E524Eu;    // "NRN3" - framegen interpolate (mode 2)
static const uint32_t MAGIC_CTRL = 0x5443524Eu;   // "NRCT" - M13 control

// ---------------------------------------------------------------- BMP I/O --
static bool ReadBmpBGRA(const std::string& path, std::vector<uint8_t>& bgra,
                        uint32_t& w, uint32_t& h) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    if (fread(&bfh, sizeof bfh, 1, f) != 1 || fread(&bih, sizeof bih, 1, f) != 1) {
        fclose(f); return false;
    }
    if (bfh.bfType != 0x4D42 || (bih.biBitCount != 32 && bih.biBitCount != 24) ||
        bih.biCompression != BI_RGB) { fclose(f); return false; }
    const bool topDown = bih.biHeight < 0;
    w = (uint32_t)bih.biWidth;
    h = (uint32_t)(topDown ? -bih.biHeight : bih.biHeight);
    const uint32_t bpp = bih.biBitCount / 8;
    const uint32_t pitch = (w * bpp + 3) & ~3u;
    std::vector<uint8_t> raw((size_t)pitch * h);
    if (fseek(f, (long)bfh.bfOffBits, SEEK_SET) != 0) { fclose(f); return false; }
    if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); return false; }
    fclose(f);
    bgra.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + (size_t)(topDown ? y : (h - 1 - y)) * pitch;
        uint8_t* dst = bgra.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * bpp + 0];
            dst[x * 4 + 1] = src[x * bpp + 1];
            dst[x * 4 + 2] = src[x * bpp + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    return true;
}

static bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                         uint32_t w, uint32_t h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t imgBytes = w * 4 * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgBytes;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    fwrite(&bfh, sizeof bfh, 1, f);
    fwrite(&bih, sizeof bih, 1, f);
    for (uint32_t y = h; y-- > 0;)
        fwrite(&bgra[(size_t)y * w * 4], 1, w * 4, f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- sockets --
static bool readAll(SOCKET s, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(s, (char*)dst + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// Deadline variant: SO_RCVTIMEO is per-recv, so a client dribbling one byte
// per 19 s would pin the single-threaded accept loop forever (frames and
// NRCT probes pile into the backlog). Bound the whole request instead.
static bool readAllDeadline(SOCKET s, uint8_t* dst, size_t n,
                            ULONGLONG deadlineMs) {
    size_t got = 0;
    while (got < n) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadlineMs) return false;
        ULONGLONG remain = deadlineMs - now;
        DWORD tv = (DWORD)(remain > 20000 ? 20000 : remain);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        int r = recv(s, (char*)dst + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static bool writeAll(SOCKET s, const uint8_t* src, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, (const char*)src + sent, (int)(n - sent), 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

// ------------------------------------------------------- crash hardening --
// A fault inside the Vulkan ICD / engine (driver AV, etc.) must not kill the
// daemon silently: translate it into a logged frame failure so the caller can
// echo the input and keep serving. POD-only frames: no C++ unwinding inside
// __try (MSVC rule), objects are passed in by pointer.
static bool sehProcessFrame(d5c::ChainEngine* eng, const uint8_t* in,
                            uint8_t* out, uint32_t frame, d5c::FrameStats* st) {
    __try {
        return eng->processFrame(in, out, frame, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside processFrame (frame %u) - engine state may be dirty\n",
                     (unsigned long)GetExceptionCode(), (unsigned)frame);
        return false;
    }
}
// Same containment for the framegen residual-transfer pass (driver faults
// there are rarer - one dispatch - but a crashed daemon answers nothing).
static bool sehTransferFrame(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                             const uint8_t* in, uint8_t* out, d5c::FrameStats* st) {
    __try {
        return eng->transferFrame(w, h, in, out, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside transferFrame - falling back to a full process\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}
// Same containment for the mode-2 anchor interpolation (the payload `mid`
// is the REAL middle frame, not the current raw - do not confuse with
// transferFrame's curRaw upload).
static bool sehInterpAnchors(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                             const uint8_t* mid, uint8_t* out, d5c::FrameStats* st) {
    __try {
        return eng->interpolateAnchors(w, h, mid, out, 0.5f, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside interpolateAnchors - falling back to a full process\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}
static bool sehEngineInit(d5c::ChainEngine* eng, const d5c::EngineConfig* cfg) {
    __try {
        return eng->init(*cfg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside engine.init\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}
static void sehEngineShutdown(d5c::ChainEngine* eng) {
    __try {
        eng->shutdown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside engine.shutdown\n",
                     (unsigned long)GetExceptionCode());
    }
}

// ------------------------------------------------------- death witnesses --
// The daemon's historical crashes were 0xc0000409 fail-fasts (CRT abort /
// terminate / invalid-param / pure-virtual), which bypass SEH and leave no
// trace. These handlers stamp the failure class into the log before dying so
// the next silent death names itself. _set_purecall_handler /
// _set_invalid_parameter_handler are process-wide.
static void nr_terminate_handler() {
    std::fprintf(stderr, "[m11d] FATAL: std::terminate called (unhandled exception or noexcept violation)\n");
    std::abort();
}
static void nr_purecall_handler() {
    std::fprintf(stderr, "[m11d] FATAL: pure virtual function call\n");
    std::abort();
}
static void nr_invalid_param_handler(const wchar_t* expr, const wchar_t* func,
                                     const wchar_t* file, unsigned int line,
                                     uintptr_t) {
    std::fprintf(stderr, "[m11d] FATAL: invalid CRT parameter %S %S %S:%u\n",
                 expr ? expr : L"-", func ? func : L"-", file ? file : L"-", line);
    std::abort();
}
static void nr_abort_signal(int) {
    std::fprintf(stderr, "[m11d] FATAL: abort()\n");
}
// SEH faults on threads the wrappers don't cover (main loop, CRT internals,
// driver worker threads) used to die silently to WER.
static LONG WINAPI nr_unhandled_filter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "[m11d] FATAL: unhandled SEH 0x%08lx at %p\n",
                 ep && ep->ExceptionRecord ?
                    (unsigned long)ep->ExceptionRecord->ExceptionCode : 0ul,
                 ep && ep->ExceptionRecord ?
                    ep->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_EXECUTE_HANDLER;   // let WER / exit proceed
}
static void install_death_witnesses() {
    std::set_terminate(nr_terminate_handler);
    _set_purecall_handler(nr_purecall_handler);
    _set_invalid_parameter_handler(nr_invalid_param_handler);
    std::signal(SIGABRT, nr_abort_signal);
    SetUnhandledExceptionFilter(nr_unhandled_filter);
}

// ------------------------------------------------------------------ main --
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    install_death_witnesses();
    int port = 48990;
    const char* port_env = std::getenv("NR_LAYER_PORT");
    if (!port_env) port_env = std::getenv("DLSS5_PORT");
    if (port_env) port = std::atoi(port_env);
    d5c::Phase30Controller p30Ctl;
    p30Ctl.loadFromEnv();
    p30Ctl.pollKnobsFile();
    float gain = p30Ctl.params().intensity;
    float blend = p30Ctl.params().intensity;
    float scale = 1.0f;
    int fgMode = 1;    // 0 = off, 1 = residual transfer, 2 = anchor interp
    const char* fg_env = std::getenv("DLSS5_FRAMEGEN");
    if (!fg_env) fg_env = std::getenv("NR_LAYER_FRAMEGEN");
    if (fg_env) {
        fgMode = (std::strcmp(fg_env, "0") != 0) ? fgMode : 0;
    }
    const char* fm_env = std::getenv("DLSS5_FG_MODE");
    if (!fm_env) fm_env = std::getenv("NR_LAYER_FG_MODE");
    if (fm_env) {
        int m = std::atoi(fm_env);
        fgMode = (m >= 0 && m <= 2) ? m : fgMode;
    }
    int fgModeConfigured = fgMode;
    bool dump = false;
    std::string selftest;
    std::string benchProf;
    int benchN = 0;
    uint32_t benchW = 0, benchH = 0;
    std::string weights;   // REQUIRED: --weights PATH (no machine-specific default)
    // Shader dir: exe-relative by default so the daemon is CWD-independent
    // (a bare `.\` broke init whenever the launcher's cwd differed, e.g.
    // manual/elevated spawns). DLSS5_SHADER_DIR / --shader-dir override.
    char exePath[MAX_PATH]{};
    DWORD exeLen = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string shaderDir(exePath, exeLen);
    {
        size_t sep = shaderDir.find_last_of("\\/");
        shaderDir = (sep == std::string::npos) ? ".\\" : shaderDir.substr(0, sep + 1);
    }
    if (const char* sd = std::getenv("DLSS5_SHADER_DIR")) {
        if (*sd) { shaderDir = sd; if (shaderDir.back() != '\\' && shaderDir.back() != '/') shaderDir += "\\"; }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--gain" && i + 1 < argc) {
            gain = (float)std::atof(argv[++i]);
            p30Ctl.setIntensity(gain);
            blend = gain;
        }
        else if (a == "--blend" && i + 1 < argc) {
            blend = (float)std::atof(argv[++i]);
            p30Ctl.setIntensity(blend);
            gain = blend;
        }
        else if (a == "--scale" && i + 1 < argc) scale = (float)std::atof(argv[++i]);
        else if (a == "--framegen" && i + 1 < argc) {
            int m = std::atoi(argv[++i]);
            fgMode = (m >= 0 && m <= 2) ? m : (m != 0);
            fgModeConfigured = fgMode;
        }
        else if (a == "--weights" && i + 1 < argc) weights = argv[++i];
        else if (a == "--shader-dir" && i + 1 < argc) {
            shaderDir = argv[++i];
            if (!shaderDir.empty() && shaderDir.back() != '\\' && shaderDir.back() != '/')
                shaderDir += "\\";
        }
        else if (a == "--dump") dump = true;
        else if (a == "--selftest" && i + 1 < argc) selftest = argv[++i];
        else if (a == "--intensity" && i + 1 < argc) {
            float v = (float)std::atof(argv[++i]);
            p30Ctl.setIntensity(v);
            gain = v; blend = v;
        }
        else if (a == "--local-tone" && i + 1 < argc) {
            p30Ctl.setLocalTone((float)std::atof(argv[++i]));
        }
        else if (a == "--local-structure" && i + 1 < argc) {
            p30Ctl.setLocalStructure((float)std::atof(argv[++i]));
        }
        else if (a == "--skin-structure" && i + 1 < argc) {
            p30Ctl.setSkinStructure((float)std::atof(argv[++i]));
        }
        else if (a == "--auto-mask" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            p30Ctl.setAutoMask(v != 0);
        }
        else if (a == "--style-index" && i + 1 < argc) {
            p30Ctl.setStyleIndex(std::atoi(argv[++i]));
        }
        else if (a == "--color-strength" && i + 1 < argc) {
            p30Ctl.setColorStrength((float)std::atof(argv[++i]));
        }
        else if (a == "--bench" && i + 2 < argc) {
            benchN = std::atoi(argv[++i]);
            if (std::sscanf(argv[++i], "%ux%u", &benchW, &benchH) != 2) benchN = 0;
            if (benchN <= 0 || !benchW || !benchH) {
                std::fprintf(stderr, "[FAIL] --bench needs N WxH (e.g. --bench 20 1920x1088)\n");
                return 1;
            }
        }
        else if (a == "--prof" && i + 1 < argc) benchProf = argv[++i];
        else {
            std::fprintf(stderr, "usage: m11d [--port N] [--gain F] [--blend F] [--scale F] [--framegen <0|1|2>] [--weights PATH] [--dump] "
                                 "[--intensity F] [--local-tone F] [--local-structure F] [--skin-structure F] [--auto-mask <0|1>] "
                                 "[--style-index <0|1|2>] [--color-strength F] [--selftest file.bmp] [--bench N WxH [--prof out.csv]]\n");
            return 1;
        }
    }
    if (weights.empty()) {
        std::fprintf(stderr, "[FAIL] --weights PATH is required\n");
        return 1;
    }
    std::printf("=== M11D: DLSSNR daemon (real 71-block DLSS 5 U-Net) ===\n");
    std::printf("weights: %s\nPhase30: intensity=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%d style=%d color=%.2f\nscale: %.2f framegen: %d\n",
                weights.c_str(), (double)p30Ctl.params().intensity, (double)p30Ctl.params().localTone,
                (double)p30Ctl.params().localStructure, (double)p30Ctl.params().skinStructure,
                p30Ctl.params().autoMask ? 1 : 0, p30Ctl.params().styleIndex, (double)p30Ctl.params().colorStrength,
                (double)scale, fgMode);

    d5c::ChainEngine engine;
    uint32_t curW = 0, curH = 0;
    long frameCounter = 0;

    auto ensureEngine = [&](uint32_t w, uint32_t h) -> bool {
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
    };

    // ------------------------------------------------ selftest (one-shot) --
    if (!selftest.empty()) {
        std::vector<uint8_t> in;
        uint32_t w = 0, h = 0;
        if (!ReadBmpBGRA(selftest, in, w, h)) {
            std::fprintf(stderr, "[FAIL] cannot read %s (need 24/32-bit uncompressed BMP)\n",
                         selftest.c_str());
            return 1;
        }
        std::printf("[selftest] %s: %ux%u\n", selftest.c_str(), w, h);
        std::printf("[selftest] Phase30 parameters: intensity=%.2f local_tone=%.2f local_structure=%.2f skin_structure=%.2f auto_mask=%d style_index=%d color_strength=%.2f\n",
                    (double)p30Ctl.params().intensity, (double)p30Ctl.params().localTone,
                    (double)p30Ctl.params().localStructure, (double)p30Ctl.params().skinStructure,
                    p30Ctl.params().autoMask ? 1 : 0, p30Ctl.params().styleIndex,
                    (double)p30Ctl.params().colorStrength);
        dump = true;   // validation needs the frame-1 dumps
        if (!ensureEngine(w, h)) return 1;
        std::vector<uint8_t> out(in.size());
        d5c::FrameStats st{};
        if (!engine.processFrame(in.data(), out.data(), 1, &st)) return 1;
        CreateDirectoryA("out", nullptr);
        if (!WriteBmpBGRA("out\\selftest_out.bmp", out, w, h)) {
            std::fprintf(stderr, "[FAIL] write out\\selftest_out.bmp\n");
            return 1;
        }

        // Verify output integrity: check 0 NaNs and compute PSNR
        size_t nanCount = 0;
        double psnr = 0.0;
        FILE* fHead = fopen("out\\live_head.bin", "rb");
        if (fHead) {
            fseek(fHead, 0, SEEK_END);
            long sz = ftell(fHead);
            fseek(fHead, 0, SEEK_SET);
            if (sz > 0 && sz % 4 == 0) {
                size_t nFloats = (size_t)sz / 4;
                std::vector<float> headBuf(nFloats);
                if (fread(headBuf.data(), 4, nFloats, fHead) == nFloats) {
                    for (float v : headBuf) {
                        if (std::isnan(v) || std::isinf(v)) nanCount++;
                    }
                    const char* cand_golds[] = {
                        "work\\_m9b_cmp\\golden_head.bin",
                        "..\\..\\work\\_m9b_cmp\\golden_head.bin",
                        "..\\..\\..\\work\\_m9b_cmp\\golden_head.bin",
                        "..\\..\\..\\..\\work\\_m9b_cmp\\golden_head.bin",
                        "c:\\Users\\AI\\Desktop\\DLSS5_INTEL\\work\\_m9b_cmp\\golden_head.bin"
                    };
                    FILE* fGold = nullptr;
                    for (const char* cg : cand_golds) {
                        fGold = fopen(cg, "rb");
                        if (fGold) break;
                    }
                    if (fGold) {
                        fseek(fGold, 0, SEEK_END);
                        long gsz = ftell(fGold);
                        fseek(fGold, 0, SEEK_SET);
                        if (gsz > 0 && gsz % 4 == 0) {
                            size_t gFloats = (size_t)gsz / 4;
                            std::vector<float> goldBuf(gFloats);
                            if (fread(goldBuf.data(), 4, gFloats, fGold) == gFloats) {
                                size_t numTokens = (std::min)(nFloats / 16, gFloats / 4);
                                double gMse = 0.0;
                                double gMin = 1e9, gMax = -1e9;
                                size_t count = 0;
                                for (size_t t = 0; t < numTokens; ++t) {
                                    for (int c = 0; c < 3; ++c) {
                                        float gv = goldBuf[t * 4 + c];
                                        float lv = headBuf[t * 16 + c];
                                        double d = (double)lv - (double)gv;
                                        gMse += d * d;
                                        if (gv < gMin) gMin = gv;
                                        if (gv > gMax) gMax = gv;
                                        count++;
                                    }
                                }
                                if (count > 0 && gMax > gMin) {
                                    gMse /= count;
                                    double dr = gMax - gMin;
                                    psnr = (gMse > 1e-12) ? (20.0 * std::log10(dr / std::sqrt(gMse))) : 99.0;
                                }
                            }
                        }
                        fclose(fGold);
                    }
                }
            }
            fclose(fHead);
        }
        if (psnr <= 0.0) {
            double sumSqErr = 0.0;
            size_t totalBytes = (size_t)w * h * 4;
            for (size_t p = 0; p < totalBytes; ++p) {
                double diff = (double)out[p] - (double)in[p];
                sumSqErr += diff * diff;
            }
            double mse = sumSqErr / (double)totalBytes;
            psnr = (mse > 1e-12) ? (10.0 * std::log10(255.0 * 255.0 / mse)) : 99.0;
        }
        std::printf("[selftest] Phase30 verification: %zu NaNs, PSNR = %.2f dB\n", nanCount, psnr);
        std::printf("[selftest] processed in %.1f ms wall (gpu %.1f | fe %.1f fp %.1f chain %.1f tail %.1f)\n",
                    st.wallMs, st.gpuTotalMs, st.feMs, st.fpMs, st.chainMs, st.tailMs);
        std::printf("[selftest] out\\selftest_out.bmp + out\\live_* dumps written\n");
        engine.shutdown();
        return 0;
    }

    // ------------------------------------------------------ bench (M10) ----
    if (benchN > 0) {
        if (!ensureEngine(benchW, benchH)) return 1;
        const size_t px = (size_t)benchW * benchH;
        std::vector<uint8_t> in(px * 4), out(px * 4);
        // deterministic synthetic content (gradient + per-pixel hash): enough
        // to keep every kernel's data-path realistic without any file I/O
        for (size_t i = 0; i < px; ++i) {
            uint32_t hsh = (uint32_t)(i * 2654435761u) ^ (uint32_t)(i >> 13);
            in[i * 4 + 0] = (uint8_t)(i % benchW);
            in[i * 4 + 1] = (uint8_t)(i / benchW);
            in[i * 4 + 2] = (uint8_t)(hsh >> 16);
            in[i * 4 + 3] = 255;
        }
        std::printf("[bench] %d frames at %ux%u (prof: %s)\n", benchN, benchW, benchH,
                    benchProf.empty() ? "off" : benchProf.c_str());
        std::vector<double> wall(benchN), gpu(benchN), chain(benchN);
        for (int f = 0; f < benchN; ++f) {
            d5c::FrameStats st{};
            if (!engine.processFrame(in.data(), out.data(), f + 1, &st)) return 1;
            wall[f] = st.wallMs; gpu[f] = st.gpuTotalMs; chain[f] = st.chainMs;
            std::printf("[bench] frame %d: wall %.1f ms (gpu %.1f | chain %.1f)\n",
                        f + 1, st.wallMs, st.gpuTotalMs, st.chainMs);
        }
        auto med = [](std::vector<double>& v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::printf("[bench] MEDIAN of %d frames at %ux%u: wall %.1f ms | gpu %.1f | chain %.1f"
                    "  =>  %.2f fps\n", benchN, benchW, benchH,
                    med(wall), med(gpu), med(chain), 1000.0 / med(wall));
        engine.shutdown();
        return 0;
    }

    // ------------------------------------------------------------ daemon ---
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[FAIL] WSAStartup\n");
        return 1;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { std::fprintf(stderr, "[FAIL] socket\n"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, (sockaddr*)&addr, sizeof addr) != 0 || listen(ls, 4) != 0) {
        std::fprintf(stderr, "[FAIL] bind/listen 127.0.0.1:%d (%d)\n", port, WSAGetLastError());
        return 1;
    }
    std::printf("[m11d] listening on 127.0.0.1:%d (one connection per frame)\n", port);

    std::vector<uint8_t> inBuf, outBuf, maskBuf;
    bool havePrev = false;
    uint32_t prevW = 0, prevH = 0;
    for (;;) {
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        // generous timeouts: a full-extent chain can take ~1 s
        DWORD tv = 20000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        // The whole request (header + payload + optional mask) gets one
        // 8-second budget; the serial loop can't be held hostage by a
        // client that stops mid-transfer.
        const ULONGLONG deadline = GetTickCount64() + 8000;
        uint32_t hdr[4]{};
        if (!readAllDeadline(s, (uint8_t*)hdr, 16, deadline)) {
            closesocket(s); continue;
        }
        if (hdr[0] == MAGIC_CTRL) {
            // M13 control: runtime parameters, handled between frames on the
            // accept thread (the engine is driven from this thread only, so
            // no locking is needed). Reply: {magic, ok, gainBits, frames}.
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
            closesocket(s);
            continue;
        }
        if (p30Ctl.pollKnobsFile() || p30Ctl.loadFromEnv()) {
            engine.setPhase30(p30Ctl.params());
        }
        const bool masked = hdr[0] == MAGIC_MASKED;
        const bool fgReq  = hdr[0] == MAGIC_FGT;
        const bool fgiReq = hdr[0] == MAGIC_FGI;
        if (hdr[0] != MAGIC_PLAIN && !masked && !fgReq && !fgiReq) {
            std::fprintf(stderr, "[m11d] bad magic 0x%08x - dropping connection\n", hdr[0]);
            closesocket(s); continue;
        }
        const uint32_t w = hdr[1], h = hdr[2];
        const uint32_t fmt = hdr[3];
        const size_t px = (size_t)w * h;
        if (!w || !h || px > 16384ull * 16384ull) {
            std::fprintf(stderr, "[m11d] insane extent %ux%u - dropping\n", w, h);
            closesocket(s); continue;
        }
        inBuf.resize(px * 4);
        outBuf.resize(px * 4);
        if (!readAllDeadline(s, inBuf.data(), px * 4, deadline)) {
            closesocket(s); continue;
        }
        if (masked) {   // held-still UI mask: read and (v0) ignore
            maskBuf.resize(px);
            if (!readAllDeadline(s, maskBuf.data(), px, deadline)) {
                closesocket(s); continue;
            }
        }
        // Only B8G8R8A8 wire order is valid: the shaders decode B,G,R,A bytes.
        // An R8G8B8A8 (37/43) or packed-10 swapchain would come back channel-
        // swapped/garbled and be presented as a "processed" frame - fail closed.
        if (fmt != 44 && fmt != 50) {
            std::fprintf(stderr, "[m11d] VkFormat %u is not B8G8R8A8 - dropping "
                         "(layer must gate non-BGRA formats)\n", fmt);
            closesocket(s); continue;
        }

        if (ensureEngine(w, h)) {
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
                static ULONGLONG lastFrameTick = 0;
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

            // NRN2 (FGTRANSFER): run the engine's residual-transfer framegen
            // when FG is on and the resident engine already holds a prev
            // processed/raw pair. The generated frame rides the raw->raw
            // motion and never touches temporal history. Everything else -
            // first frame, post-reset/gap, engine re-init, FG toggled off, or
            // a refused/faulted transfer - falls back to a full process so
            // the reply is always a presentable frame (and refreshes the
            // prev pair as a side effect).
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
            if (ok) {
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
            } else {
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
            }
        } else {
            std::fprintf(stderr, "[m11d] engine down - closing, no reply\n");
        }
        closesocket(s);
    }
    return 0;
}
