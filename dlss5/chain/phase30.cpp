// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain/phase30.cpp - NVIDIA Phase 30 condition parameter implementation.
// ============================================================================
#include "phase30.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace d5c {

namespace {

float clampFloat(float v, float lo, float hi) {
    if (std::isnan(v)) return lo;
    return (std::max)(lo, (std::min)(hi, v));
}

// Lightweight JSON value parser for Phase30 knob files
bool extractJsonField(const std::string& json, const std::string& key, std::string& outVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    if (pos >= json.size()) return false;
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != '\r' && json[end] != '\n') {
        end++;
    }
    std::string token = json.substr(pos, end - pos);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
        token.pop_back();
    }
    if (token.front() == '"' && token.back() == '"' && token.size() >= 2) {
        token = token.substr(1, token.size() - 2);
    }
    outVal = token;
    return true;
}

} // namespace

void Phase30Params::computeFeatureControls(float ctrl[5]) const {
    // ctrl[0] -> ch 10: normalized style (style_index / 128.0)
    int sIdx = (std::max)(0, (std::min)(2, styleIndex));
    ctrl[0] = static_cast<float>(sIdx) / 128.0f;

    // ctrl[1] -> ch 11: local tone strength
    ctrl[1] = clampFloat(localTone, 0.0f, 2.0f);

    // Channels 12..14: structure / skin / auto-mask
    // Follows MLX-DLSS features.py:159-171:
    // When automatic_mask is active:
    //   enabled = max(skin_structure, auto_structure) >= 0
    //   structure = 1.0 if enabled else local_structure
    //   skin_structure = skin if enabled else -1.0
    //   auto_structure = local_structure if enabled else -1.0
    float lStruct = clampFloat(localStructure, 0.0f, 2.0f);
    float sStruct = clampFloat(skinStructure, -1.0f, 2.0f);

    if (autoMask) {
        bool enabled = (sStruct >= 0.0f);
        if (enabled) {
            ctrl[2] = 1.0f;       // local structure normalized to 1.0 under auto-mask
            ctrl[3] = sStruct;    // skin structure scalar
            ctrl[4] = lStruct;    // automatic structure scalar follows local structure
        } else {
            // Negative skin structure (-1.0) means "follow local structure"
            ctrl[2] = lStruct;
            ctrl[3] = -1.0f;
            ctrl[4] = -1.0f;
        }
    } else {
        // Auto mask is OFF: skin structure is inactive
        ctrl[2] = lStruct;
        ctrl[3] = -1.0f;
        ctrl[4] = -1.0f;
    }
}

bool Phase30Params::equals(const Phase30Params& o, float eps) const {
    return std::abs(intensity - o.intensity) <= eps &&
           std::abs(localTone - o.localTone) <= eps &&
           std::abs(localStructure - o.localStructure) <= eps &&
           std::abs(skinStructure - o.skinStructure) <= eps &&
           autoMask == o.autoMask &&
           styleIndex == o.styleIndex &&
           std::abs(colorStrength - o.colorStrength) <= eps;
}

Phase30Controller::Phase30Controller() {
    knobFilePath_ = defaultKnobFilePath();
    loadFromEnv();
}

std::string Phase30Controller::defaultKnobFilePath() {
    char tempPath[MAX_PATH] = {0};
    DWORD len = GetTempPathA(MAX_PATH, tempPath);
    if (len > 0 && len < MAX_PATH) {
        std::string p(tempPath);
        if (p.back() != '\\' && p.back() != '/') p += '\\';
        return p + "m13_phase30_knobs.json";
    }
    return "m13_phase30_knobs.json";
}

void Phase30Controller::setParams(const Phase30Params& p) {
    params_.intensity = clampFloat(p.intensity, 0.0f, 2.0f);
    params_.localTone = clampFloat(p.localTone, 0.0f, 2.0f);
    params_.localStructure = clampFloat(p.localStructure, 0.0f, 2.0f);
    params_.skinStructure = clampFloat(p.skinStructure, -1.0f, 2.0f);
    params_.autoMask = p.autoMask;
    params_.styleIndex = (std::max)(0, (std::min)(2, p.styleIndex));
    params_.colorStrength = clampFloat(p.colorStrength, 0.0f, 2.0f);
}

void Phase30Controller::setIntensity(float v) {
    params_.intensity = clampFloat(v, 0.0f, 2.0f);
}

void Phase30Controller::setLocalTone(float v) {
    params_.localTone = clampFloat(v, 0.0f, 2.0f);
}

void Phase30Controller::setLocalStructure(float v) {
    params_.localStructure = clampFloat(v, 0.0f, 2.0f);
}

void Phase30Controller::setSkinStructure(float v) {
    params_.skinStructure = clampFloat(v, -1.0f, 2.0f);
}

void Phase30Controller::setAutoMask(bool v) {
    params_.autoMask = v;
}

void Phase30Controller::setStyleIndex(int v) {
    params_.styleIndex = (std::max)(0, (std::min)(2, v));
}

void Phase30Controller::setColorStrength(float v) {
    params_.colorStrength = clampFloat(v, 0.0f, 2.0f);
}

bool Phase30Controller::loadFromEnv() {
    bool changed = false;

    // Intensity / Gain
    const char* inten = std::getenv("DLSS5_INTENSITY");
    if (!inten) inten = std::getenv("NR_LAYER_INTENSITY");
    if (!inten) inten = std::getenv("M12_INTENSITY");
    if (!inten) inten = std::getenv("DLSS5_GAIN");
    if (!inten) inten = std::getenv("NR_LAYER_GAIN");
    if (!inten) inten = std::getenv("M12_GAIN");
    if (inten) {
        float val = static_cast<float>(std::atof(inten));
        if (std::abs(val - params_.intensity) > 1e-4f) {
            setIntensity(val);
            changed = true;
        }
    }

    // Local Tone
    const char* tone = std::getenv("DLSS5_LOCAL_TONE");
    if (!tone) tone = std::getenv("NR_LAYER_LOCAL_TONE");
    if (!tone) tone = std::getenv("M12_LOCAL_TONE");
    if (tone) {
        float val = static_cast<float>(std::atof(tone));
        if (std::abs(val - params_.localTone) > 1e-4f) {
            setLocalTone(val);
            changed = true;
        }
    }

    // Local Structure
    const char* lstruct = std::getenv("DLSS5_LOCAL_STRUCTURE");
    if (!lstruct) lstruct = std::getenv("NR_LAYER_LOCAL_STRUCTURE");
    if (!lstruct) lstruct = std::getenv("M12_LOCAL_STRUCTURE");
    if (lstruct) {
        float val = static_cast<float>(std::atof(lstruct));
        if (std::abs(val - params_.localStructure) > 1e-4f) {
            setLocalStructure(val);
            changed = true;
        }
    }

    // Skin Structure
    const char* sstruct = std::getenv("DLSS5_SKIN_STRUCTURE");
    if (!sstruct) sstruct = std::getenv("NR_LAYER_SKIN_STRUCTURE");
    if (!sstruct) sstruct = std::getenv("M12_SKIN_STRUCTURE");
    if (sstruct) {
        float val = static_cast<float>(std::atof(sstruct));
        if (std::abs(val - params_.skinStructure) > 1e-4f) {
            setSkinStructure(val);
            changed = true;
        }
    }

    // Auto Mask
    const char* amask = std::getenv("DLSS5_AUTO_MASK");
    if (!amask) amask = std::getenv("NR_LAYER_AUTO_MASK");
    if (!amask) amask = std::getenv("M12_AUTO_MASK");
    if (amask) {
        bool val = (amask[0] == '1' || !std::strcmp(amask, "true") || !std::strcmp(amask, "on"));
        if (val != params_.autoMask) {
            setAutoMask(val);
            changed = true;
        }
    }

    // Style Index
    const char* sidx = std::getenv("DLSS5_STYLE_INDEX");
    if (!sidx) sidx = std::getenv("NR_LAYER_STYLE_INDEX");
    if (!sidx) sidx = std::getenv("M12_STYLE_INDEX");
    if (sidx) {
        int val = std::atoi(sidx);
        if (val != params_.styleIndex) {
            setStyleIndex(val);
            changed = true;
        }
    }

    // Color Strength
    const char* cstr = std::getenv("DLSS5_COLOR_STRENGTH");
    if (!cstr) cstr = std::getenv("NR_LAYER_COLOR_STRENGTH");
    if (!cstr) cstr = std::getenv("M12_COLOR_STRENGTH");
    if (cstr) {
        float val = static_cast<float>(std::atof(cstr));
        if (std::abs(val - params_.colorStrength) > 1e-4f) {
            setColorStrength(val);
            changed = true;
        }
    }

    return changed;
}

bool Phase30Controller::pollKnobsFile(const std::string& customPath) {
    const std::string& path = customPath.empty() ? knobFilePath_ : customPath;
    if (path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) {
        return false;
    }

    uint64_t wt = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                  static_cast<uint64_t>(fa.ftLastWriteTime.dwLowDateTime);
    if (wt == lastFileWriteTime_ && lastFileWriteTime_ != 0) {
        return false;
    }
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();
    // m13 rewrites this file non-atomically; a mid-rewrite poll reads a
    // truncated JSON (no closing brace). Bail WITHOUT consuming the mtime so
    // the next poll retries the same write - otherwise one torn read sticks
    // a never-intended parameter mix into the engine for >=1 frame.
    if (json.find('}') == std::string::npos) return false;
    lastFileWriteTime_ = wt;

    std::string val;
    bool changed = false;

    if (extractJsonField(json, "intensity", val)) {
        float v = static_cast<float>(std::atof(val.c_str()));
        if (std::abs(v - params_.intensity) > 1e-4f) {
            setIntensity(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "local_tone", val)) {
        float v = static_cast<float>(std::atof(val.c_str()));
        if (std::abs(v - params_.localTone) > 1e-4f) {
            setLocalTone(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "local_structure", val)) {
        float v = static_cast<float>(std::atof(val.c_str()));
        if (std::abs(v - params_.localStructure) > 1e-4f) {
            setLocalStructure(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "skin_structure", val)) {
        float v = static_cast<float>(std::atof(val.c_str()));
        if (std::abs(v - params_.skinStructure) > 1e-4f) {
            setSkinStructure(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "auto_mask", val)) {
        bool v = (val == "true" || val == "1" || val == "True");
        if (v != params_.autoMask) {
            setAutoMask(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "style_index", val)) {
        int v = std::atoi(val.c_str());
        if (v != params_.styleIndex) {
            setStyleIndex(v);
            changed = true;
        }
    }
    if (extractJsonField(json, "color_strength", val)) {
        float v = static_cast<float>(std::atof(val.c_str()));
        if (std::abs(v - params_.colorStrength) > 1e-4f) {
            setColorStrength(v);
            changed = true;
        }
    }

    return changed;
}

} // namespace d5c
