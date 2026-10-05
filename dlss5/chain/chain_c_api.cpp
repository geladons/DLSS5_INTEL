// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - C API implementation wrapping d5c::ChainEngine.
// Encapsulates engine lifecycle, lazy initialization, resolution tracking,
// and buffer management for in-process Vulkan present layers.
// ============================================================================
#include "chain_c_api.h"
#include "engine.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

// Helper to determine the directory of the current module.
std::string GetModuleDir() {
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(dlss5_create), &hMod) && hMod) {
        char buf[MAX_PATH] = {0};
        if (GetModuleFileNameA(hMod, buf, sizeof(buf))) {
            char* slash = std::strrchr(buf, '\\');
            if (!slash) slash = std::strrchr(buf, '/');
            if (slash) {
                *(slash + 1) = '\0';
                return std::string(buf);
            }
        }
    }
    return ".\\";
}

bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::string NormalizePath(const std::string& path) {
    char full[MAX_PATH] = {0};
    if (GetFullPathNameA(path.c_str(), sizeof(full), full, nullptr)) {
        return std::string(full);
    }
    return path;
}

class EngineContext {
public:
    EngineContext(const char* weights, const char* shaders,
                  float s, float g, float b)
        : scale_(s > 0.0f ? s : 1.0f),
          gain_(g >= 0.0f ? g : 1.0f),
          blend_((b >= 0.0f && b <= 1.0f) ? b : 1.0f),
          sharpness_(0.8f)
    {
        p30Ctl_.loadFromEnv();
        p30Ctl_.pollKnobsFile();

        const char* s_env = std::getenv("DLSS5_SHARPNESS");
        if (!s_env) s_env = std::getenv("NR_LAYER_SHARPNESS");
        if (s_env) {
            float val = static_cast<float>(std::atof(s_env));
            if (val >= 0.0f) sharpness_ = val;
        }
        const char* fg_env = std::getenv("DLSS5_FRAMEGEN");
        if (!fg_env) fg_env = std::getenv("NR_LAYER_FRAMEGEN");
        frameGenEnabled_ = (fg_env && std::strcmp(fg_env, "0") != 0);
        // FG mode: 1 = residual transfer (+0 latency), 2 = anchor
        // interpolation (+1 frame). Default 1 so a new dll under an old
        // layer keeps the mode-1 anchor layout byte-identical; mode 2 is
        // requested explicitly via env or set_framegen_mode.
        const char* fm_env = std::getenv("DLSS5_FG_MODE");
        if (!fm_env) fm_env = std::getenv("NR_LAYER_FG_MODE");
        if (fm_env) fgMode_ = (std::atoi(fm_env) == 2) ? 2 : 1;
        // Temporal conditioning default ON - it anchors each reconstruction
        // to the previous output (the flicker fix). DLSS5_TEMPORAL=0 /
        // NR_LAYER_TEMPORAL=0 disables for A/B and determinism tests.
        const char* t_env = std::getenv("DLSS5_TEMPORAL");
        if (!t_env) t_env = std::getenv("NR_LAYER_TEMPORAL");
        temporal_ = !(t_env && std::strcmp(t_env, "0") == 0);
        // Gain anchor: EMA smoothing alpha for the global mean gain.
        // Default OFF (0.0) - opt-in only via env (e.g. 0.08 = ~12-frame
        // window). Needs temporal_stats.spv.
        const char* g_env = std::getenv("DLSS5_GAIN_LOCK");
        if (!g_env) g_env = std::getenv("NR_LAYER_GAIN_LOCK");
        if (g_env) {
            float v = static_cast<float>(std::atof(g_env));
            if (v >= 0.0f) gainLock_ = v;
        }
        resolveWeights(weights);
        resolveShaders(shaders);
    }

    ~EngineContext() {
        shutdown();
    }

    bool ensure(uint32_t w, uint32_t h) {
        if (initialized_ && curW_ == w && curH_ == h && curScale_ == scale_) {
            return true;
        }

        // Init-failure latch: this code runs INSIDE game processes, so a
        // failed init must not be retried per frame (each attempt is a
        // vkCreateInstance + full weights parse - a multi-second hitch and a
        // churn of failed Vulkan objects). The latch releases only when the
        // requested resolution/scale actually changes (same place where
        // initialized_/havePrev_ are reset on a real config change).
        if (init_failed_ && curW_ == w && curH_ == h && curScale_ == scale_) {
            return false;
        }

        if (initialized_) {
            std::printf("[dlss5_c_api] config changed (res %ux%u -> %ux%u, scale %.2f -> %.2f): re-initializing\n",
                        curW_, curH_, w, h, static_cast<double>(curScale_), static_cast<double>(scale_));
            engine_.shutdown();
            initialized_ = false;
            havePrev_ = false;
        }
        init_failed_ = false;   // new config (or first attempt): allow one fresh try

        d5c::EngineConfig cfg{};
        cfg.canvasW = w;
        cfg.canvasH = h;
        cfg.weightsPath = weightsPath_;
        cfg.shaderDir = shaderDir_;
        cfg.headGain = gain_;
        cfg.headBlend = blend_;
        cfg.renderScale = scale_;
        cfg.temporal = temporal_;
        cfg.gainLock = gainLock_;
        cfg.phase30 = p30Ctl_.params();
        // sharpness_ already holds the ctor env baseline or a live knob
        // push (dlss5_set_sharpness). No env re-read here: re-applying the
        // launch baseline on a re-init would silently revert a pushed
        // value (same env-leash class as the per-frame read below).
        cfg.sharpness = sharpness_;
        cfg.debugDumps = false;
        cfg.dumpDir = "out";
        cfg.vkCfg.appName = "dlss5_inprocess";
        cfg.profPath = "";

        // engine.init can both return false and throw (weights/shader load
        // and Vulkan create failures are std::runtime_error now - a library
        // must never exit() the host game). Catch here so the latch below
        // covers BOTH failure modes and processFrame still returns -1.
        bool initOk = false;
        try {
            initOk = engine_.init(cfg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[dlss5_c_api] engine.init threw for %ux%u (weights: %s): %s\n",
                         w, h, weightsPath_.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr, "[dlss5_c_api] engine.init threw for %ux%u (weights: %s)\n",
                         w, h, weightsPath_.c_str());
        }
        if (!initOk) {
            std::fprintf(stderr, "[dlss5_c_api] engine.init failed for %ux%u (weights: %s)\n",
                         w, h, weightsPath_.c_str());
            engine_.shutdown();          // release any partially-created Vulkan state
            curW_ = w;
            curH_ = h;
            curScale_ = scale_;          // remember the config that failed
            init_failed_ = true;
            return false;
        }

        curW_ = w;
        curH_ = h;
        curScale_ = scale_;
        initialized_ = true;
        engine_.setFrameGenMode(fgMode_);   // clamps to 1 if fginterp.spv missing
        return true;
    }

    int processFrame(uint32_t w, uint32_t h, const void* in_bgra,
                     void* out_bgra, long frame_index) {
        if (!ensure(w, h)) {
            return -1;
        }

        // DLSS5_SHARPNESS / NR_LAYER_SHARPNESS / DLSS5_FRAMEGEN /
        // NR_LAYER_FRAMEGEN envs were captured ONCE in the ctor - the
        // process env is frozen at launch, so re-reading them per frame
        // only reverted live knob pushes (dlss5_set_sharpness /
        // dlss5_set_framegen via the layer's knob poll) back to the
        // baseline within one frame. Same pattern as the phase30 env fix:
        // knob file / ABI pushes are the live channel, env is baseline.
        if (w != prevW_ || h != prevH_) {
            havePrev_ = false;
        }

        const size_t px = static_cast<size_t>(w) * h;
        if (alphaBuf_.size() < px) {
            alphaBuf_.resize(px);
        }

        const auto* inBytes = static_cast<const uint8_t*>(in_bgra);
        auto* outBytes = static_cast<uint8_t*>(out_bgra);

        // Preserve original game alpha channel across processing
        for (size_t i = 0; i < px; ++i) {
            alphaBuf_[i] = inBytes[i * 4 + 3];
        }

        // Env is frozen at context creation (loadFromEnv ran in the ctor);
        // re-reading it per frame here reverted live knob pushes to the
        // launch baseline within one frame. The knob file is the live channel.
        if (p30Ctl_.pollKnobsFile()) {
            engine_.setPhase30(p30Ctl_.params());
        }

        d5c::FrameStats stats{};

        // Clean forward processing for every frame via DLSS 5 engine
        bool ok = engine_.processFrame(inBytes, outBytes, frame_index, &stats);
        if (ok) {
            if (prevProcessed_.size() < px * 4) {
                prevProcessed_.resize(px * 4);
            }
            std::memcpy(prevProcessed_.data(), outBytes, px * 4);
            havePrev_ = true;
            prevW_ = w;
            prevH_ = h;
            // Restore original game alpha channel
            for (size_t i = 0; i < px; ++i) {
                outBytes[i * 4 + 3] = alphaBuf_[i];
            }
            return 0;
        }

        return -1;
    }

    int interpolateFrame(uint32_t w, uint32_t h, const void* f0, const void* f1,
                         void* out, float factor) {
        if (!ensure(w, h)) {
            return -1;
        }
        d5c::FrameStats stats{};
        bool ok = engine_.interpolateFrame(w, h, f0, f1, out, factor, &stats);
        return ok ? 0 : -1;
    }

    int framegenTransfer(uint32_t w, uint32_t h, const void* cur, void* out) {
        if (!ensure(w, h)) {
            return -1;
        }
        d5c::FrameStats stats{};
        bool ok = engine_.transferFrame(w, h, cur, out, &stats);
        return ok ? 0 : -1;
    }

    // Mode-2 generated frame: midpoint of the two most recent processed
    // anchors; mid may be NULL (unwarped blend fallback).
    int framegenInterpolate(uint32_t w, uint32_t h, const void* mid,
                            void* out, float factor) {
        if (!ensure(w, h)) {
            return -1;
        }
        d5c::FrameStats stats{};
        bool ok = engine_.interpolateAnchors(w, h, mid, out, factor, &stats);
        return ok ? 0 : -1;
    }

    void setFrameGenMode(int mode) {
        fgMode_ = (mode == 2) ? 2 : 1;
        if (initialized_) {
            engine_.setFrameGenMode(fgMode_);
        }
    }

    int getFrameGenMode() const {
        return initialized_ ? engine_.frameGenMode() : fgMode_;
    }

    void setFrameGen(int enable) {
        frameGenEnabled_ = (enable != 0);
        if (!frameGenEnabled_) {
            havePrev_ = false;
        }
    }

    void setTemporal(int enable) {
        temporal_ = (enable != 0);
        if (initialized_) {
            engine_.setTemporal(temporal_);
        }
    }

    int getTemporal() const {
        return temporal_ ? 1 : 0;
    }

    // Drop temporal history - next frame takes the first-frame path. For
    // callers that feed the engine unrelated content back to back (photo
    // batch, window switch): stale history would otherwise bleed across.
    void resetHistory() {
        if (initialized_) {
            engine_.resetHistory();
        }
    }

    int getFrameGen() const {
        return frameGenEnabled_ ? 1 : 0;
    }

    float getFrameGenGpuMs() const {
        return static_cast<float>(engine_.lastFrameGenGpuMs());
    }

    void setScale(float s) {
        if (s <= 0.0f) return;
        scale_ = s;
    }

    void setGain(float g) {
        if (g < 0.0f) return;
        gain_ = g;
        if (initialized_) {
            engine_.setHeadGain(g);
        }
    }

    void setBlend(float b) {
        if (b < 0.0f || b > 1.0f) return;
        blend_ = b;
        // The blend knob drives the compose-mix slot (pc.c.w = Phase30
        // intensity): the old setHeadBlend dual-write is gone, so the live
        // push goes through the intensity channel explicitly - same
        // routing as the daemon's NRCT SETBLEND. p30Ctl_ tracks it so an
        // engine re-init (cfg.phase30 = params) does not revert the push.
        p30Ctl_.setIntensity(b);
        if (initialized_) {
            engine_.setHeadBlend(b);   // bookkeeping slot only now
            engine_.setIntensity(b);
        }
    }

    void setSharpness(float s) {
        if (s < 0.0f) return;
        sharpness_ = s;
        if (initialized_) {
            engine_.setSharpness(s);
        }
    }

    void setPhase30(float intensity, float local_tone, float local_structure,
                    float skin_structure, int auto_mask, int style_index,
                    float color_strength) {
        d5c::Phase30Params p{};
        p.intensity = intensity;
        p.localTone = local_tone;
        p.localStructure = local_structure;
        p.skinStructure = skin_structure;
        p.autoMask = (auto_mask != 0);
        p.styleIndex = style_index;
        p.colorStrength = color_strength;
        p30Ctl_.setParams(p);
        if (initialized_) {
            engine_.setPhase30(p30Ctl_.params());
        }
    }

    void setIntensity(float v) {
        p30Ctl_.setIntensity(v);
        if (initialized_) engine_.setIntensity(v);
    }
    void setLocalTone(float v) {
        p30Ctl_.setLocalTone(v);
        if (initialized_) engine_.setLocalTone(v);
    }
    void setLocalStructure(float v) {
        p30Ctl_.setLocalStructure(v);
        if (initialized_) engine_.setLocalStructure(v);
    }
    void setSkinStructure(float v) {
        p30Ctl_.setSkinStructure(v);
        if (initialized_) engine_.setSkinStructure(v);
    }
    void setAutoMask(int v) {
        p30Ctl_.setAutoMask(v != 0);
        if (initialized_) engine_.setAutoMask(v != 0);
    }
    void setStyleIndex(int v) {
        p30Ctl_.setStyleIndex(v);
        if (initialized_) engine_.setStyleIndex(v);
    }
    void setColorStrength(float v) {
        p30Ctl_.setColorStrength(v);
        if (initialized_) engine_.setColorStrength(v);
    }

    void shutdown() {
        if (initialized_) {
            engine_.shutdown();
            initialized_ = false;
            havePrev_ = false;
        }
    }

private:
    // Last-resort resolution shared by weights/shaders: a tiny KEY=VALUE
    // file the manager rewrites on every config save. Covers games launched
    // outside our spawn (Steam client, Rockstar/CDPR launchers, a manual
    // double-click) where the env block never reaches the game process and
    // the module-dir candidates cannot match.
    static std::string runtimePathsValue(const char* key) {
        char dir[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
        if (!n || n >= sizeof(dir)) return {};
        std::string p = std::string(dir) +
                        "\\DLSS5Manager\\runtime_paths.txt";
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return {};
        char line[1024];
        std::string out;
        const size_t klen = std::strlen(key);
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strncmp(line, key, klen) && line[klen] == '=') {
                out.assign(line + klen + 1);
                while (!out.empty() &&
                       (out.back() == '\n' || out.back() == '\r' ||
                        out.back() == ' ' || out.back() == '\t'))
                    out.pop_back();
                break;
            }
        }
        std::fclose(f);
        return out;
    }

    void resolveWeights(const char* weights) {
        if (weights && weights[0]) {
            weightsPath_ = NormalizePath(weights);
            return;
        }
        if (const char* envW = std::getenv("DLSS5_WEIGHTS_PATH")) {
            weightsPath_ = NormalizePath(envW);
            return;
        }
        if (const char* envW2 = std::getenv("NR_LAYER_WEIGHTS")) {
            weightsPath_ = NormalizePath(envW2);
            return;
        }

        std::string modDir = GetModuleDir();
        const std::string candidates[] = {
            "work\\mlxw\\dlssnr-logical.safetensors",
            modDir + "..\\..\\..\\work\\mlxw\\dlssnr-logical.safetensors",
            modDir + "..\\..\\..\\..\\work\\mlxw\\dlssnr-logical.safetensors",
            // Production bundle layout: runtime\layer\x64|m12|m11d ->
            // <bundle>\weights\dlssnr-logical.safetensors. Required for
            // launcher/Steam-spawned games where our env never reaches the
            // game process (dlss5_create must succeed without env).
            modDir + "..\\..\\..\\weights\\dlssnr-logical.safetensors",
            modDir + "dlssnr-logical.safetensors"
        };
        for (const auto& c : candidates) {
            if (FileExists(c)) {
                weightsPath_ = NormalizePath(c);
                return;
            }
        }
        std::string w = runtimePathsValue("WEIGHTS");
        weightsPath_ = !w.empty() ? NormalizePath(w)
                                  : "work\\mlxw\\dlssnr-logical.safetensors";
    }

    void resolveShaders(const char* shaders) {
        if (shaders && shaders[0]) {
            shaderDir_ = shaders;
        } else if (const char* envS = std::getenv("DLSS5_SHADER_DIR")) {
            shaderDir_ = envS;
        } else if (const char* envS2 = std::getenv("NR_LAYER_SHADERS")) {
            shaderDir_ = envS2;
        } else {
            std::string modDir = GetModuleDir();
            if (FileExists(modDir + "gemm.spv")) {
                shaderDir_ = modDir;
            } else if (FileExists(".\\gemm.spv")) {
                shaderDir_ = ".\\";
            } else {
                std::string s = runtimePathsValue("SHADER_DIR");
                shaderDir_ = !s.empty() ? s : modDir;
            }
        }

        if (!shaderDir_.empty() && shaderDir_.back() != '\\' && shaderDir_.back() != '/') {
            shaderDir_ += '\\';
        }
    }

    d5c::ChainEngine engine_;
    std::string weightsPath_;
    std::string shaderDir_;
    float scale_ = 1.0f;
    float gain_ = 1.0f;
    float blend_ = 1.0f;
    float sharpness_ = 0.8f;
    float curScale_ = 1.0f;
    uint32_t curW_ = 0;
    uint32_t curH_ = 0;
    bool initialized_ = false;
    bool init_failed_ = false;   // latched until dimensions/scale change
    bool frameGenEnabled_ = false;
    int fgMode_ = 1;               // 1 = transfer (+0), 2 = anchor interp (+1)
    bool temporal_ = true;
    float gainLock_ = 0.0f;           // DLSS5_GAIN_LOCK: gain EMA alpha
                                      // (default OFF - opt-in via env)
    std::vector<uint8_t> prevProcessed_;
    bool havePrev_ = false;
    uint32_t prevW_ = 0, prevH_ = 0;
    std::vector<uint8_t> alphaBuf_;
    d5c::Phase30Controller p30Ctl_;
};

} // namespace

extern "C" {

DLSS5_API void* dlss5_create(const char* weights_path, const char* shader_dir,
                             float scale, float gain, float blend) {
    try {
        auto* ctx = new EngineContext(weights_path, shader_dir, scale, gain, blend);
        return static_cast<void*>(ctx);
    } catch (...) {
        return nullptr;
    }
}

DLSS5_API void dlss5_destroy(void* handle) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        delete ctx;
    } catch (...) {
    }
}

DLSS5_API int dlss5_process_frame(void* handle, uint32_t w, uint32_t h,
                                  const void* in_bgra, void* out_bgra,
                                  long frame_index) {
    if (!handle || !in_bgra || !out_bgra || !w || !h) {
        return -1;
    }
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->processFrame(w, h, in_bgra, out_bgra, frame_index);
    } catch (...) {
        return -1;
    }
}

DLSS5_API void dlss5_set_scale(void* handle, float scale) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setScale(scale);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_gain(void* handle, float gain) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setGain(gain);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_blend(void* handle, float blend) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setBlend(blend);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_sharpness(void* handle, float s) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setSharpness(s);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_phase30(void* handle,
                                 float intensity,
                                 float local_tone,
                                 float local_structure,
                                 float skin_structure,
                                 int auto_mask,
                                 int style_index,
                                 float color_strength) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setPhase30(intensity, local_tone, local_structure, skin_structure,
                        auto_mask, style_index, color_strength);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_intensity(void* handle, float intensity) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setIntensity(intensity);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_local_tone(void* handle, float local_tone) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setLocalTone(local_tone);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_local_structure(void* handle, float local_structure) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setLocalStructure(local_structure);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_skin_structure(void* handle, float skin_structure) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setSkinStructure(skin_structure);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_auto_mask(void* handle, int auto_mask) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setAutoMask(auto_mask);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_style_index(void* handle, int style_index) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setStyleIndex(style_index);
    } catch (...) {
    }
}

DLSS5_API void dlss5_set_color_strength(void* handle, float color_strength) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setColorStrength(color_strength);
    } catch (...) {
    }
}

DLSS5_API int dlss5_interpolate_frame(void* handle, uint32_t w, uint32_t h,
                                      const void* frame0_bgra, const void* frame1_bgra,
                                      void* out_interp_bgra, float factor) {
    if (!handle || !frame1_bgra || !out_interp_bgra || !w || !h) {
        return -1;
    }
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->interpolateFrame(w, h, frame0_bgra, frame1_bgra, out_interp_bgra, factor);
    } catch (...) {
        return -1;
    }
}

DLSS5_API int dlss5_framegen_transfer(void* handle, uint32_t w, uint32_t h,
                                      const void* cur_raw_bgra, void* out_bgra) {
    if (!handle || !cur_raw_bgra || !out_bgra || !w || !h) {
        return -1;
    }
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->framegenTransfer(w, h, cur_raw_bgra, out_bgra);
    } catch (...) {
        return -1;
    }
}

DLSS5_API void dlss5_set_framegen(void* handle, int enable) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setFrameGen(enable);
    } catch (...) {
    }
}

DLSS5_API int dlss5_get_framegen(void* handle) {
    if (!handle) return 0;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->getFrameGen();
    } catch (...) {
        return 0;
    }
}

DLSS5_API float dlss5_get_framegen_gpu_ms(void* handle) {
    if (!handle) return 0.0f;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->getFrameGenGpuMs();
    } catch (...) {
        return 0.0f;
    }
}

DLSS5_API int dlss5_framegen_interpolate(void* handle, uint32_t w, uint32_t h,
                                         const void* mid_raw_bgra,
                                         void* out_bgra, float factor) {
    if (!handle || !out_bgra || !w || !h) {
        return -1;
    }
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->framegenInterpolate(w, h, mid_raw_bgra, out_bgra, factor);
    } catch (...) {
        return -1;
    }
}

DLSS5_API void dlss5_set_framegen_mode(void* handle, int mode) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setFrameGenMode(mode);
    } catch (...) {
    }
}

DLSS5_API int dlss5_get_framegen_mode(void* handle) {
    if (!handle) return 0;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->getFrameGenMode();
    } catch (...) {
        return 0;
    }
}

DLSS5_API void dlss5_set_temporal(void* handle, int enable) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->setTemporal(enable);
    } catch (...) {
    }
}

DLSS5_API int dlss5_get_temporal(void* handle) {
    if (!handle) return 0;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        return ctx->getTemporal();
    } catch (...) {
        return 0;
    }
}

DLSS5_API void dlss5_reset_history(void* handle) {
    if (!handle) return;
    try {
        auto* ctx = static_cast<EngineContext*>(handle);
        ctx->resetHistory();
    } catch (...) {
    }
}

} // extern "C"
