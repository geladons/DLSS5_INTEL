// ============================================================================
// dlss5/chain — WeightsStore: DLSSNR safetensors parse + pack layout +
// load-time preprocessing (branched-FFN fuse-fold, attn_bias fragment
// de-swizzle, fp32 side tables, folded head weight).
// Extracted VERBATIM from dlss5/m8b-live/main.cpp sections "M8a chain
// constants + safetensors parser" and 8 (weights residency).
// ============================================================================
#pragma once

#include "vk_util.h"

#include <map>

namespace d5c {

struct Tensor {
    std::string dtype;
    std::vector<int64_t> shape;
    uint64_t off0 = 0, off1 = 0;
    uint64_t numel() const {
        uint64_t n = 1;
        for (auto s : shape) n *= (uint64_t)s;
        return n;
    }
};

struct WeightsFile {
    std::vector<char> bytes;
    size_t dataBase = 0;
    std::map<std::string, Tensor> tensors;
};

// Fragment swizzle (nr_model._fragment_swizzle_indices): the single-head
// window blocks (0-4, 66-70) and the 16-head split blocks (23-30, 40-47) store
// attn_bias in fused-kernel mma fragment order; logical[j] = stored[SWZ[j]].
// The 2/4/8-head blocks (5-22, 48-65) store the logical layout already.
extern uint32_t SWZ[4096];
void InitSwizzle();

class WeightsStore {
public:
    bool load(const std::string& path);     // read + parse safetensors
    bool computePack();                     // sorted pack offsets + layout cross-check

    // Writes the whole host arena image: pack region [0, packTotal) with
    // fuse-fold/de-swizzle applied, fp32 side tables at vecOff offsets,
    // the folded [32,16] f16 head weight at oHeadW.
    void preprocessInto(char* sp,
                        const std::map<std::string, VkDeviceSize>& vecOff,
                        VkDeviceSize oHeadW) const;

    const Tensor& tensor(const std::string& n) const { return wf.tensors.at(n); }
    bool has(const std::string& n) const { return wf.tensors.count(n) != 0; }

    static std::string tname(const char* fmt, int i);

    WeightsFile wf;
    std::map<std::string, uint64_t> poff;   // tensor name -> offset in the pack
    uint64_t packTotal = 291535872ull;      // docs/pack-layout.txt (256-aligned total)
};

} // namespace d5c
