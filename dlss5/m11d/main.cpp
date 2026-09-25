// ============================================================================
// m11d - DLSSNR frame daemon (M11): TCP server on 127.0.0.1 (default 47990)
// driving the real 71-block DLSS 5 U-Net via dlss5/chain (ChainEngine).
//
// Protocol (identical to reference nr_layer.c / nr_layer_win.c):
//   request: 16-byte header {magic, w, h, format} (uint32 LE)
//            magic 0x304E524E: payload w*h*4 bytes (BGRA)
//            magic 0x314E524E: payload w*h*4 + w*h mask bytes (1 = held-still UI)
//            magic 0x5443524E ("NRCT"): M13 control channel, 16-byte header
//                     {magic, cmd, payload, reserved} instead of a frame:
//                     cmd 1 = SETGAIN (payload = float bits, reply ok+gain)
//                     cmd 2 = STATUS  (reply ok, gain, frames processed)
//                     cmd 3 = SETBLEND(payload = float bits 0..1, reply ok+gain)
//   reply:   frame requests: w*h*4 bytes BGRA (the processed frame)
//            control requests: 16 bytes {magic, ok, gainBits, frames}
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"

static const uint32_t MAGIC_PLAIN = 0x304E524Eu;
static const uint32_t MAGIC_MASKED = 0x314E524Eu;
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

static bool writeAll(SOCKET s, const uint8_t* src, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, (const char*)src + sent, (int)(n - sent), 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

// ------------------------------------------------------------------ main --
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int port = 47990;
    float gain = 1.0f;
    float blend = 1.0f;
    bool dump = false;
    std::string selftest;
    std::string benchProf;
    int benchN = 0;
    uint32_t benchW = 0, benchH = 0;
    std::string weights;   // REQUIRED: --weights PATH (no machine-specific default)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--gain" && i + 1 < argc) gain = (float)std::atof(argv[++i]);
        else if (a == "--blend" && i + 1 < argc) blend = (float)std::atof(argv[++i]);
        else if (a == "--weights" && i + 1 < argc) weights = argv[++i];
        else if (a == "--dump") dump = true;
        else if (a == "--selftest" && i + 1 < argc) selftest = argv[++i];
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
            std::fprintf(stderr, "usage: m11d [--port N] [--gain F] [--blend F] [--weights PATH] [--dump] "
                                 "[--selftest file.bmp] [--bench N WxH [--prof out.csv]]\n");
            return 1;
        }
    }
    if (weights.empty()) {
        std::fprintf(stderr, "[FAIL] --weights PATH is required\n");
        return 1;
    }
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;
    std::printf("=== M11D: DLSSNR daemon (real 71-block DLSS 5 U-Net) ===\n");
    std::printf("weights: %s\ngain: %.2f blend: %.2f\n", weights.c_str(), (double)gain, (double)blend);

    d5c::ChainEngine engine;
    uint32_t curW = 0, curH = 0;
    long frameCounter = 0;

    auto ensureEngine = [&](uint32_t w, uint32_t h) -> bool {
        if (w == curW && h == curH) return true;
        if (curW || curH) {
            std::printf("[m11d] frame size change %ux%u -> %ux%u: engine re-init\n", curW, curH, w, h);
            engine.shutdown();
        }
        d5c::EngineConfig cfg{};
        cfg.canvasW = w; cfg.canvasH = h;
        cfg.weightsPath = weights;
        cfg.shaderDir = ".\\";            // spv staged next to the exe
        cfg.headGain = gain;
        cfg.headBlend = blend;
        cfg.debugDumps = dump;
        cfg.dumpDir = "out";
        cfg.vkCfg.appName = "m11d";
        cfg.profPath = benchProf;   // empty = profiling off (production default)
        // headless: no surface/swapchain extensions, first coopmat-capable device
        if (!engine.init(cfg)) {
            std::fprintf(stderr, "[FAIL] engine init for %ux%u\n", w, h);
            return false;
        }
        curW = w; curH = h;
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
    for (;;) {
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        // generous timeouts: a full-extent chain can take ~1 s
        DWORD tv = 20000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        uint32_t hdr[4]{};
        if (!readAll(s, (uint8_t*)hdr, 16)) { closesocket(s); continue; }
        if (hdr[0] == MAGIC_CTRL) {
            // M13 control: runtime parameters, handled between frames on the
            // accept thread (the engine is driven from this thread only, so
            // no locking is needed). Reply: {magic, ok, gainBits, frames}.
            uint32_t rep[4] = { MAGIC_CTRL, 0, 0, 0 };
            if (hdr[1] == 1) {                   // SETGAIN: hdr[2] = float bits
                float g = 0.0f;
                std::memcpy(&g, &hdr[2], 4);
                if (g >= 0.0f && g <= 16.0f) {
                    gain = g;
                    engine.setHeadGain(g);       // next processed frame uses it
                    rep[1] = 1;
                    std::printf("[m11d] ctrl: gain -> %.3f\n", (double)g);
                } else {
                    std::fprintf(stderr, "[m11d] ctrl: gain %.3f out of range\n", (double)g);
                }
            } else if (hdr[1] == 3) {            // SETBLEND: hdr[2] = float bits
                float b = 0.0f;
                std::memcpy(&b, &hdr[2], 4);
                if (b >= 0.0f && b <= 1.0f) {
                    blend = b;
                    engine.setHeadBlend(b);
                    rep[1] = 1;
                    std::printf("[m11d] ctrl: blend -> %.3f\n", (double)b);
                } else {
                    std::fprintf(stderr, "[m11d] ctrl: blend %.3f out of range\n", (double)b);
                }
            } else if (hdr[1] == 2) {            // STATUS
                rep[1] = 1;
            } else {
                std::fprintf(stderr, "[m11d] ctrl: unknown cmd %u\n", hdr[1]);
            }
            std::memcpy(&rep[2], &gain, 4);
            rep[3] = (uint32_t)frameCounter;
            writeAll(s, (const uint8_t*)rep, 16);
            closesocket(s);
            continue;
        }
        const bool masked = hdr[0] == MAGIC_MASKED;
        if (hdr[0] != MAGIC_PLAIN && !masked) {
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
        if (!readAll(s, inBuf.data(), px * 4)) { closesocket(s); continue; }
        if (masked) {   // held-still UI mask: read and (v0) ignore
            maskBuf.resize(px);
            if (!readAll(s, maskBuf.data(), px)) { closesocket(s); continue; }
        }
        if (fmt != 44 && fmt != 43 && fmt != 50 && fmt != 49)
            std::printf("[m11d] note: VkFormat %u treated as BGRA8\n", fmt);

        if (ensureEngine(w, h)) {
            d5c::FrameStats st{};
            ++frameCounter;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = engine.processFrame(inBuf.data(), outBuf.data(), frameCounter, &st);
            double wall = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                // vendor parity (nr_daemon.py:88): alpha stays as the game
                // left it (encode forces opaque for the overlay use-case);
                // held-still UI pixels (mask) pass through untouched.
                for (size_t i = 0; i < px; ++i) {
                    if (masked && maskBuf[i]) {
                        std::memcpy(&outBuf[i * 4], &inBuf[i * 4], 4);
                    } else {
                        outBuf[i * 4 + 3] = inBuf[i * 4 + 3];
                    }
                }
                if (!writeAll(s, outBuf.data(), px * 4))
                    std::fprintf(stderr, "[m11d] reply write failed\n");
                std::printf("[m11d] frame %ld %ux%u%s in %.1f ms (chain %.1f)\n",
                            frameCounter, w, h, masked ? " +mask" : "", wall, st.chainMs);
            } else {
                std::fprintf(stderr, "[m11d] processFrame FAILED - echoing input\n");
                writeAll(s, inBuf.data(), px * 4);
            }
        } else {
            writeAll(s, inBuf.data(), px * 4);   // engine down: pass through
        }
        closesocket(s);
    }
    return 0;
}
