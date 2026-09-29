// ============================================================================
// dlss5/chain/phase30.h - NVIDIA Phase 30 condition parameter module.
//
// Encapsulates the 7 vendor tuning parameters:
//   1. intensity: 0.0 .. 2.0 (public baseline 1.0)
//   2. local_tone: 0.0 .. 2.0 (default 1.0)
//   3. local_structure: 0.0 .. 2.0 (public baseline 1.0)
//   4. skin_structure: -1.0 .. 2.0 (public baseline -1.0)
//   5. auto_mask: bool (public baseline false)
//   6. style_index: 0, 1, 2 (default 0)
//   7. color_strength: 0.0 .. 2.0 (default 1.0)
//
// Handles feature channel scalar mapping (channels 10..14), environment variable
// loading, and live JSON knob file polling (%TEMP%\m13_phase30_knobs.json).
// ============================================================================
#ifndef DLSS5_CHAIN_PHASE30_H
#define DLSS5_CHAIN_PHASE30_H

#include <cstdint>
#include <string>

namespace d5c {

struct Phase30Params {
    float intensity = 1.0f;        // Compose-side mix (0.0 .. 2.0, public baseline 1.0)
    float localTone = 1.0f;        // Local tone strength (0.0 .. 2.0, default 1.0)
    float localStructure = 1.0f;   // Local structure detail strength (baseline 1.0)
    float skinStructure = -1.0f;   // Skin structure disabled in the public baseline
    bool autoMask = false;         // Optional skin/UI condition disabled in the public baseline
    int styleIndex = 0;            // Style index (0: Default, 1: Natural, 2: Cinematic)
    float colorStrength = 1.0f;    // Color component strength (0.0 .. 2.0, default 1.0)

    // Compute the 5 condition scalars for feature channels 10..14.
    // Follows recovered MLX-DLSS features.py:155-171 and notes/phase30-control-atlas.md:
    //   ctrl[0] -> ch 10: normalized style (styleIndex / 128.0)
    //   ctrl[1] -> ch 11: local tone strength
    //   ctrl[2] -> ch 12: local structure (1.0 if auto_mask active with skin >= 0)
    //   ctrl[3] -> ch 13: skin structure (or -1.0 if auto_mask off / follow)
    //   ctrl[4] -> ch 14: auto-mask structure (local structure if follow, or -1.0 if auto_mask off)
    void computeFeatureControls(float ctrl[5]) const;

    // Equality check with tolerance
    bool equals(const Phase30Params& o, float eps = 1e-4f) const;
};

class Phase30Controller {
public:
    Phase30Controller();

    // Accessors
    const Phase30Params& params() const { return params_; }
    void setParams(const Phase30Params& p);

    // Individual setters (clamped to valid ranges)
    void setIntensity(float v);
    void setLocalTone(float v);
    void setLocalStructure(float v);
    void setSkinStructure(float v);
    void setAutoMask(bool v);
    void setStyleIndex(int v);
    void setColorStrength(float v);

    // Environment and file polling
    bool loadFromEnv();
    bool pollKnobsFile(const std::string& customPath = "");

    // Default file path for Phase30 live knobs (%TEMP%\m13_phase30_knobs.json)
    static std::string defaultKnobFilePath();

private:
    Phase30Params params_{};
    std::string knobFilePath_;
    uint64_t lastFileWriteTime_ = 0;
};

} // namespace d5c

#endif // DLSS5_CHAIN_PHASE30_H
