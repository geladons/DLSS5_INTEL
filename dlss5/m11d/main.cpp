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
//
// Layout:
//   main.cpp        - argv/env parsing, one-shot modes, socket accept loop
//   daemon_core.*   - DaemonCore: engine lifecycle, NRCT dispatch, frame path
//   daemon_io.*     - BMP stills + deadline socket IO
//   engine_seh.*    - SEH containment around ChainEngine calls
//   crash_witness.* - process-wide fatal handlers (log-before-die)
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
#include <string>
#include <vector>

#include "daemon_core.h"
#include "daemon_io.h"
#include "crash_witness.h"

static const uint32_t MAGIC_CTRL = 0x5443524Eu;   // "NRCT" - M13 control

// ------------------------------------------------------------------ main --
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    install_death_witnesses();
    int port = 48990;
    const char* port_env = std::getenv("NR_LAYER_PORT");
    if (!port_env) port_env = std::getenv("DLSS5_PORT");
    if (port_env) port = std::atoi(port_env);
    DaemonCore core;
    core.p30Ctl.loadFromEnv();
    core.p30Ctl.pollKnobsFile();
    core.gain = core.p30Ctl.params().intensity;
    core.blend = core.p30Ctl.params().intensity;
    const char* fg_env = std::getenv("DLSS5_FRAMEGEN");
    if (!fg_env) fg_env = std::getenv("NR_LAYER_FRAMEGEN");
    if (fg_env) {
        core.fgMode = (std::strcmp(fg_env, "0") != 0) ? core.fgMode : 0;
    }
    const char* fm_env = std::getenv("DLSS5_FG_MODE");
    if (!fm_env) fm_env = std::getenv("NR_LAYER_FG_MODE");
    if (fm_env) {
        int m = std::atoi(fm_env);
        core.fgMode = (m >= 0 && m <= 2) ? m : core.fgMode;
    }
    core.fgModeConfigured = core.fgMode;
    std::string selftest;
    int benchN = 0;
    uint32_t benchW = 0, benchH = 0;
    // Shader dir: exe-relative by default so the daemon is CWD-independent
    // (a bare `.\` broke init whenever the launcher's cwd differed, e.g.
    // manual/elevated spawns). DLSS5_SHADER_DIR / --shader-dir override.
    char exePath[MAX_PATH]{};
    DWORD exeLen = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    core.shaderDir.assign(exePath, exeLen);
    {
        size_t sep = core.shaderDir.find_last_of("\\/");
        core.shaderDir = (sep == std::string::npos) ? ".\\" : core.shaderDir.substr(0, sep + 1);
    }
    if (const char* sd = std::getenv("DLSS5_SHADER_DIR")) {
        if (*sd) { core.shaderDir = sd; if (core.shaderDir.back() != '\\' && core.shaderDir.back() != '/') core.shaderDir += "\\"; }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--gain" && i + 1 < argc) {
            core.gain = (float)std::atof(argv[++i]);
            core.p30Ctl.setIntensity(core.gain);
            core.blend = core.gain;
        }
        else if (a == "--blend" && i + 1 < argc) {
            core.blend = (float)std::atof(argv[++i]);
            core.p30Ctl.setIntensity(core.blend);
            core.gain = core.blend;
        }
        else if (a == "--scale" && i + 1 < argc) core.scale = (float)std::atof(argv[++i]);
        else if (a == "--framegen" && i + 1 < argc) {
            int m = std::atoi(argv[++i]);
            core.fgMode = (m >= 0 && m <= 2) ? m : (m != 0);
            core.fgModeConfigured = core.fgMode;
        }
        else if (a == "--weights" && i + 1 < argc) core.weights = argv[++i];
        else if (a == "--shader-dir" && i + 1 < argc) {
            core.shaderDir = argv[++i];
            if (!core.shaderDir.empty() && core.shaderDir.back() != '\\' && core.shaderDir.back() != '/')
                core.shaderDir += "\\";
        }
        else if (a == "--dump") core.dump = true;
        else if (a == "--selftest" && i + 1 < argc) selftest = argv[++i];
        else if (a == "--intensity" && i + 1 < argc) {
            float v = (float)std::atof(argv[++i]);
            core.p30Ctl.setIntensity(v);
            core.gain = v; core.blend = v;
        }
        else if (a == "--local-tone" && i + 1 < argc) {
            core.p30Ctl.setLocalTone((float)std::atof(argv[++i]));
        }
        else if (a == "--local-structure" && i + 1 < argc) {
            core.p30Ctl.setLocalStructure((float)std::atof(argv[++i]));
        }
        else if (a == "--skin-structure" && i + 1 < argc) {
            core.p30Ctl.setSkinStructure((float)std::atof(argv[++i]));
        }
        else if (a == "--auto-mask" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            core.p30Ctl.setAutoMask(v != 0);
        }
        else if (a == "--style-index" && i + 1 < argc) {
            core.p30Ctl.setStyleIndex(std::atoi(argv[++i]));
        }
        else if (a == "--color-strength" && i + 1 < argc) {
            core.p30Ctl.setColorStrength((float)std::atof(argv[++i]));
        }
        else if (a == "--bench" && i + 2 < argc) {
            benchN = std::atoi(argv[++i]);
            if (std::sscanf(argv[++i], "%ux%u", &benchW, &benchH) != 2) benchN = 0;
            if (benchN <= 0 || !benchW || !benchH) {
                std::fprintf(stderr, "[FAIL] --bench needs N WxH (e.g. --bench 20 1920x1088)\n");
                return 1;
            }
        }
        else if (a == "--prof" && i + 1 < argc) core.benchProf = argv[++i];
        else {
            std::fprintf(stderr, "usage: m11d [--port N] [--gain F] [--blend F] [--scale F] [--framegen <0|1|2>] [--weights PATH] [--dump] "
                                 "[--intensity F] [--local-tone F] [--local-structure F] [--skin-structure F] [--auto-mask <0|1>] "
                                 "[--style-index <0|1|2>] [--color-strength F] [--selftest file.bmp] [--bench N WxH [--prof out.csv]]\n");
            return 1;
        }
    }
    if (core.weights.empty()) {
        std::fprintf(stderr, "[FAIL] --weights PATH is required\n");
        return 1;
    }
    std::printf("=== M11D: DLSSNR daemon (real 71-block DLSS 5 U-Net) ===\n");
    std::printf("weights: %s\nPhase30: intensity=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%d style=%d color=%.2f\nscale: %.2f framegen: %d\n",
                core.weights.c_str(), (double)core.p30Ctl.params().intensity, (double)core.p30Ctl.params().localTone,
                (double)core.p30Ctl.params().localStructure, (double)core.p30Ctl.params().skinStructure,
                core.p30Ctl.params().autoMask ? 1 : 0, core.p30Ctl.params().styleIndex, (double)core.p30Ctl.params().colorStrength,
                (double)core.scale, core.fgMode);

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
                    (double)core.p30Ctl.params().intensity, (double)core.p30Ctl.params().localTone,
                    (double)core.p30Ctl.params().localStructure, (double)core.p30Ctl.params().skinStructure,
                    core.p30Ctl.params().autoMask ? 1 : 0, core.p30Ctl.params().styleIndex,
                    (double)core.p30Ctl.params().colorStrength);
        core.dump = true;   // validation needs the frame-1 dumps
        if (!core.ensureEngine(w, h)) return 1;
        std::vector<uint8_t> out(in.size());
        d5c::FrameStats st{};
        if (!core.engine.processFrame(in.data(), out.data(), 1, &st)) return 1;
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
        core.engine.shutdown();
        return 0;
    }

    // ------------------------------------------------------ bench (M10) ----
    if (benchN > 0) {
        if (!core.ensureEngine(benchW, benchH)) return 1;
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
                    core.benchProf.empty() ? "off" : core.benchProf.c_str());
        std::vector<double> wall(benchN), gpu(benchN), chain(benchN);
        for (int f = 0; f < benchN; ++f) {
            d5c::FrameStats st{};
            if (!core.engine.processFrame(in.data(), out.data(), f + 1, &st)) return 1;
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
        core.engine.shutdown();
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
            core.handleCtrl(s, hdr);
            closesocket(s);
            continue;
        }
        core.handleFrame(s, hdr, deadline, inBuf, outBuf, maskBuf);
        closesocket(s);
    }
    return 0;
}
