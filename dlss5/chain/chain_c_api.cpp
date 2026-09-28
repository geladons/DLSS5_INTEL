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
          blend_((b >= 0.0f && b <= 1.0f) ? b : 1.0f)
    {
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

        if (initialized_) {
            std::printf("[dlss5_c_api] config changed (res %ux%u -> %ux%u, scale %.2f -> %.2f): re-initializing\n",
                        curW_, curH_, w, h, static_cast<double>(curScale_), static_cast<double>(scale_));
            engine_.shutdown();
            initialized_ = false;
        }

        d5c::EngineConfig cfg{};
        cfg.canvasW = w;
        cfg.canvasH = h;
        cfg.weightsPath = weightsPath_;
        cfg.shaderDir = shaderDir_;
        cfg.headGain = gain_;
        cfg.headBlend = blend_;
        cfg.renderScale = scale_;
        cfg.debugDumps = false;
        cfg.dumpDir = "out";
        cfg.vkCfg.appName = "dlss5_inprocess";
        cfg.profPath = "";

        if (!engine_.init(cfg)) {
            std::fprintf(stderr, "[dlss5_c_api] engine.init failed for %ux%u (weights: %s)\n",
                         w, h, weightsPath_.c_str());
            return false;
        }

        curW_ = w;
        curH_ = h;
        curScale_ = scale_;
        initialized_ = true;
        return true;
    }

    int processFrame(uint32_t w, uint32_t h, const void* in_bgra,
                     void* out_bgra, long frame_index) {
        if (!ensure(w, h)) {
            return -1;
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

        d5c::FrameStats stats{};
        bool ok = engine_.processFrame(inBytes, outBytes, frame_index, &stats);
        if (!ok) {
            return -1;
        }

        // Restore original alpha channel for vendor parity
        for (size_t i = 0; i < px; ++i) {
            outBytes[i * 4 + 3] = alphaBuf_[i];
        }

        return 0;
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

    void setFrameGen(int enable) {
        frameGenEnabled_ = enable;
    }

    int getFrameGen() const {
        return frameGenEnabled_;
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
        if (initialized_) {
            engine_.setHeadBlend(b);
        }
    }

    void shutdown() {
        if (initialized_) {
            engine_.shutdown();
            initialized_ = false;
        }
    }

private:
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
            modDir + "dlssnr-logical.safetensors"
        };
        for (const auto& c : candidates) {
            if (FileExists(c)) {
                weightsPath_ = NormalizePath(c);
                return;
            }
        }
        weightsPath_ = "work\\mlxw\\dlssnr-logical.safetensors";
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
                shaderDir_ = modDir;
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
    float curScale_ = 1.0f;
    uint32_t curW_ = 0;
    uint32_t curH_ = 0;
    bool initialized_ = false;
    int frameGenEnabled_ = 0;
    std::vector<uint8_t> alphaBuf_;
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

} // extern "C"
