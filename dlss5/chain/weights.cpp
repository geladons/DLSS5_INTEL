// ============================================================================
// dlss5/chain — WeightsStore implementation (see weights.h).
// ============================================================================
#include "weights.h"
#include "fp16.h"

#include <algorithm>
#include <cmath>

namespace d5c {

uint32_t SWZ[4096];

void InitSwizzle() {
    for (int entry = 0; entry < 4096; ++entry) {
        int q = entry / 64, k = entry % 64;
        int qy = q / 8, qx = q % 8, ky = k / 8, kx = k % 8;
        auto bit = [](int v, int p) { return (v >> p) & 1; };
        SWZ[entry] = (uint32_t)((bit(qy, 2) << 11) | (bit(qx, 2) << 10) | (bit(ky, 2) << 9) |
                                (bit(kx, 2) << 8) | (bit(qy, 0) << 7) | (bit(qx, 1) << 6) |
                                (bit(qx, 0) << 5) | (bit(ky, 0) << 4) | (bit(kx, 1) << 3) |
                                (bit(ky, 1) << 2) | (bit(qy, 1) << 1) | bit(kx, 0));
    }
}

static int blockIndexOf(const std::string& name) {
    auto dot = name.find('.');
    return std::atoi(name.substr(5, dot - 5).c_str());
}

static bool biasNeedsSwizzle(const std::string& n) {
    auto ends = [&](const char* suf) {
        size_t l = std::strlen(suf);
        return n.size() >= l && n.compare(n.size() - l, l, suf) == 0;
    };
    int blk = blockIndexOf(n);
    if (ends(".layer0.attn_bias")) return blk <= 4 || blk >= 66;
    if (ends(".layer2.attn_bias")) return (blk >= 23 && blk <= 30) || (blk >= 40 && blk <= 47);
    return false;
}

static int readFile(const std::string& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 1;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (!f.read(out.data(), sz)) return 1;
    return 0;
}

static void stParse(const std::string& path, WeightsFile& wf) {
    if (readFile(path, wf.bytes)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
    if (wf.bytes.size() < 8) { std::fprintf(stderr, "safetensors too small\n"); std::exit(1); }
    uint64_t hlen;
    std::memcpy(&hlen, wf.bytes.data(), 8);
    if (8 + hlen > wf.bytes.size()) { std::fprintf(stderr, "bad header len\n"); std::exit(1); }
    const char* p = wf.bytes.data() + 8;
    const char* end = p + hlen;
    auto ws = [&]() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; };
    auto expect = [&](char c) { ws(); if (p >= end || *p++ != c) { std::fprintf(stderr, "JSON: expected %c\n", c); std::exit(1); } };
    auto string = [&]() -> std::string {
        ws(); expect('"');
        std::string s;
        while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) ++p; s += *p++; }
        expect('"');
        return s;
    };
    auto integer = [&]() -> int64_t {
        ws(); bool neg = false;
        if (p < end && *p == '-') { neg = true; ++p; }
        int64_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    };
    ws(); expect('{');
    while (true) {
        ws();
        if (p < end && *p == '}') { ++p; break; }
        std::string key = string();
        ws(); expect(':');
        if (key == "__metadata__") {
            int depth = 0;
            ws(); expect('{'); depth = 1;
            while (p < end && depth) { char c = *p++; if (c == '{') ++depth; else if (c == '}') --depth; }
        } else {
            Tensor t;
            ws(); expect('{');
            while (true) {
                ws();
                if (*p == '}') { ++p; break; }
                std::string k = string();
                ws(); expect(':');
                if (k == "dtype") t.dtype = string();
                else if (k == "shape") {
                    ws(); expect('[');
                    while (true) { ws(); if (*p == ']') { ++p; break; } t.shape.push_back(integer()); ws(); if (*p == ',') ++p; }
                } else if (k == "data_offsets") {
                    ws(); expect('[');
                    ws(); t.off0 = (uint64_t)integer(); ws(); expect(','); ws(); t.off1 = (uint64_t)integer();
                    ws(); expect(']');
                } else {
                    ws();
                    if (*p == '"') string();
                    else while (p < end && *p != ',' && *p != '}') ++p;
                }
                ws(); if (*p == ',') ++p;
            }
            wf.tensors[key] = t;
        }
        ws(); if (p < end && *p == ',') ++p;
    }
    wf.dataBase = 8 + hlen;
    std::printf("safetensors: %zu tensors, data base %zu\n", wf.tensors.size(), wf.dataBase);
}

#pragma pack(push, 1)
struct Int8FileHeader {
    char magic[8];           // "DLSS5_I8"
    uint32_t version;        // 1
    uint32_t header_size;    // dataBase (256-aligned)
    uint32_t num_tensors;
    uint32_t alignment;      // 256
    uint64_t total_file_size;
    uint8_t reserved[32];
};

struct Int8TocEntry {
    char name[96];
    uint32_t dtype;          // 0 = F16, 1 = F32, 2 = INT8
    uint32_t ndim;
    int64_t shape[6];
    uint64_t data_offset;
    uint64_t data_bytes;
    uint64_t w_bytes;
    uint64_t scale_offset;
    uint64_t scale_bytes;
};
#pragma pack(pop)

static bool binParse(const std::string& path, WeightsFile& wf) {
    if (readFile(path, wf.bytes)) {
        std::fprintf(stderr, "cannot read binary pack %s\n", path.c_str());
        return false;
    }
    if (wf.bytes.size() < sizeof(Int8FileHeader)) {
        std::fprintf(stderr, "binary pack too small: %s\n", path.c_str());
        return false;
    }
    const Int8FileHeader* hdr = reinterpret_cast<const Int8FileHeader*>(wf.bytes.data());
    if (std::memcmp(hdr->magic, "DLSS5_I8", 8) != 0) {
        std::fprintf(stderr, "bad magic in %s (expected DLSS5_I8)\n", path.c_str());
        return false;
    }
    if (hdr->version != 1) {
        std::fprintf(stderr, "unsupported version %u in %s\n", hdr->version, path.c_str());
        return false;
    }
    if (sizeof(Int8FileHeader) + (size_t)hdr->num_tensors * sizeof(Int8TocEntry) > (size_t)hdr->header_size) {
        std::fprintf(stderr, "corrupt header_size in %s\n", path.c_str());
        return false;
    }
    const Int8TocEntry* toc = reinterpret_cast<const Int8TocEntry*>(wf.bytes.data() + sizeof(Int8FileHeader));
    wf.tensors.clear();
    for (uint32_t i = 0; i < hdr->num_tensors; ++i) {
        const Int8TocEntry& e = toc[i];
        Tensor t;
        t.dtype = (e.dtype == 2) ? "INT8" : ((e.dtype == 1) ? "F32" : "F16");
        t.isQuantized = (e.dtype == 2);
        for (uint32_t d = 0; d < e.ndim && d < 6; ++d) {
            t.shape.push_back(e.shape[d]);
        }
        t.off0 = e.data_offset;
        t.off1 = e.data_offset + e.data_bytes;
        t.scaleRelOff = e.scale_offset;
        wf.tensors[std::string(e.name)] = t;
    }
    wf.dataBase = hdr->header_size;
    std::printf("[weights] int8 pack: %zu tensors, data base %zu, total %llu bytes\n",
                wf.tensors.size(), wf.dataBase, (unsigned long long)hdr->total_file_size);
    return true;
}

std::string WeightsStore::tname(const char* fmt, int i) {
    char b[160]; std::snprintf(b, sizeof b, fmt, i); return b;
}

bool WeightsStore::load(const std::string& path) {
    bool isBin = false;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".bin") {
        isBin = true;
    } else {
        std::ifstream probe(path, std::ios::binary);
        if (probe) {
            char magic[8];
            probe.read(magic, 8);
            if (probe.gcount() == 8 && std::memcmp(magic, "DLSS5_I8", 8) == 0) isBin = true;
        }
    }
    if (isBin) {
        if (!binParse(path, wf)) return false;
        hasQuantizedWeights = true;
        return true;
    }
    stParse(path, wf);
    // Check if safetensors contains any I8/INT8 tensors
    for (auto& kv : wf.tensors) {
        if (kv.second.dtype == "I8" || kv.second.dtype == "INT8") {
            kv.second.isQuantized = true;
            hasQuantizedWeights = true;
            std::string scName = kv.first + ".scale";
            if (wf.tensors.count(scName)) {
                kv.second.scaleRelOff = wf.tensors.at(scName).off0 - kv.second.off0;
            }
        }
    }
    return true;
}

bool WeightsStore::loadInt8(const std::string& path) {
    if (!binParse(path, wfInt8)) return false;
    hasQuantizedWeights = true;
    size_t replaced = 0;

    auto parsePilot = [](const char* envName, const char* legacyEnv, int defaultVal) -> int {
        if (const char* p = std::getenv(envName)) {
            if (std::strcmp(p, "none") == 0 || std::strcmp(p, "off") == 0) return -1;
            return std::atoi(p);
        }
        if (legacyEnv) {
            if (const char* p = std::getenv(legacyEnv)) {
                if (std::strcmp(p, "none") == 0 || std::strcmp(p, "off") == 0) return -1;
                return std::atoi(p);
            }
        }
        return defaultVal;
    };

    int pilotW1   = parsePilot("D5C_INT8_W1", "D5C_INT8_PILOT", 70);
    int pilotW2   = parsePilot("D5C_INT8_W2", "D5C_INT8_PILOT", 70);
    int pilotAttn = parsePilot("D5C_INT8_ATTN", nullptr, -1);
    int pilotQKV  = parsePilot("D5C_INT8_ATTN_QKV", "D5C_INT8_ATTN", -1);
    int pilotProj = parsePilot("D5C_INT8_ATTN_PROJ", "D5C_INT8_ATTN", -1);

    for (auto& kv : wfInt8.tensors) {
        if (!kv.second.isQuantized) continue;
        int blk = std::atoi(kv.first.substr(5, kv.first.find('.') - 5).c_str());

        bool isWindow = (blk <= 30) || (blk >= 40);
        bool overrideTensor = false;
        if (kv.first.find(".layer0.weight1") != std::string::npos) {
            if (pilotW1 >= 0 && blk <= pilotW1) overrideTensor = true;
        } else if (kv.first.find(".layer0.weight2") != std::string::npos) {
            if (pilotW2 >= 0 && blk <= pilotW2) overrideTensor = true;
        } else if (kv.first.find(".qkv_weight") != std::string::npos) {
            if (isWindow && pilotQKV >= 0 && blk <= pilotQKV) overrideTensor = true;
        } else if (kv.first.find(".projection_weight") != std::string::npos) {
            if (isWindow && pilotProj >= 0 && blk <= pilotProj) overrideTensor = true;
        } else if (kv.first.find(".ffn_expand_weight") != std::string::npos) {
            if (pilotAttn >= 0 && blk <= pilotAttn) overrideTensor = true;
        }

        if (overrideTensor) {
            wf.tensors[kv.first] = kv.second;
            replaced++;
        }
    }
    std::printf("[weights] INT8 quantized weights loaded: %zu tensors overridden (w1: %d, w2: %d, qkv: %d, proj: %d)\n",
                replaced, pilotW1, pilotW2, pilotQKV, pilotProj);
    return true;
}

bool WeightsStore::computePack() {
    struct PackEntry {
        std::string name;
        uint64_t off, bytes;
        int blk;
        bool isQuant;
        uint64_t scaleRel;
    };
    std::vector<PackEntry> pack;
    for (auto& kv : wf.tensors) {
        if (kv.first.size() > 6 && kv.first.substr(kv.first.size() - 6) == ".scale") continue;
        const Tensor& t = kv.second;
        uint64_t bytes = 0;
        uint64_t scaleRel = 0;
        if (t.isQuantized) {
            uint64_t w_bytes = t.numel();
            scaleRel = (w_bytes + 255) & ~255ull;
            uint64_t sc_bytes = (uint64_t)t.N() * sizeof(float);
            bytes = (scaleRel + sc_bytes + 255) & ~255ull;
        } else {
            uint64_t esz = (t.dtype == "F32") ? 4 : 2;
            bytes = t.numel() * esz;
        }
        int blk = std::atoi(kv.first.substr(5, kv.first.find('.') - 5).c_str());
        pack.push_back({kv.first, 0, bytes, blk, t.isQuantized, scaleRel});
    }
    std::sort(pack.begin(), pack.end(), [](const PackEntry& a, const PackEntry& b) {
        if (a.blk != b.blk) return a.blk < b.blk;
        return a.name < b.name;
    });
    {
        uint64_t o = 0;
        for (auto& e : pack) {
            o = (o + 255) & ~255ull;
            e.off = o;
            o += e.bytes;
        }
        packTotal = (o + 255) & ~255ull;
    }
    poff.clear();
    scalePoff.clear();
    for (auto& e : pack) {
        poff[e.name] = e.off;
        if (e.isQuant) {
            scalePoff[e.name] = e.off + e.scaleRel;
        }
    }
    std::printf("pack: %zu tensors (%.2f MiB, quantized=%s)\n",
                pack.size(), packTotal / 1048576.0,
                hasQuantizedWeights ? "yes" : "no");
    if (!hasQuantizedWeights) {
        bool packOk = poff["block31.layer0.weight"] == 45084928ull &&
                      poff["block39.layer0.conv_weight"] == 246448384ull &&
                      poff["block0.layer0.input_adapter_weight"] == 8960ull;
        if (!packOk) { std::fprintf(stderr, "pack offset mismatch vs pack-layout.txt\n"); return false; }
        std::printf("pack offsets cross-checked vs docs/pack-layout.txt: OK\n");
    } else {
        std::printf("INT8 quantized pack layout initialized: packTotal = %llu bytes (%.2f MiB)\n",
                    (unsigned long long)packTotal, packTotal / 1048576.0);
    }
    return true;
}

static bool isBranchedExpand(const std::string& n) {
    return n.size() >= 18 && n.substr(n.size() - 18) == ".ffn_expand_weight";
}

void WeightsStore::preprocessInto(char* sp,
                                  const std::map<std::string, VkDeviceSize>& vecOff,
                                  VkDeviceSize oHeadW) const {
    // ---- pack region: copy tensors (fuse-fold branched expand, de-swizzle attn_bias) ----
    for (auto& kv : wf.tensors) {
        if (kv.first.size() > 6 && kv.first.substr(kv.first.size() - 6) == ".scale") continue;
        const Tensor& t = kv.second;
        const bool fromInt8 = t.isQuantized && (wfInt8.bytes.size() > 0);
        const WeightsFile& srcWf = fromInt8 ? wfInt8 : wf;
        const char* src = srcWf.bytes.data() + srcWf.dataBase + t.off0;
        char* dst = sp + poff.at(kv.first);

        if (t.isQuantized) {
            // Copy quantized int8 weights
            uint64_t w_bytes = t.numel();
            std::memcpy(dst, src, w_bytes);
            // Copy aligned per-channel scales
            const char* src_scale = src + t.scaleRelOff;
            char* dst_scale = sp + scalePoff.at(kv.first);
            std::memcpy(dst_scale, src_scale, (size_t)t.N() * sizeof(float));
        } else if (isBranchedExpand(kv.first)) {
            uint64_t G = t.shape[0];
            const uint16_t* s = (const uint16_t*)src;
            uint16_t* d = (uint16_t*)dst;
            for (uint64_t oh = 0; oh < G; ++oh)
                for (uint64_t ih = 0; ih < G; ++ih)
                    for (uint64_t r = 0; r < 32; ++r)
                        for (uint64_t br = 0; br < 4; ++br)
                            for (uint64_t c2 = 0; c2 < 32; ++c2)
                                d[((oh * G + ih) * 32 + r) * 128 + br * 32 + c2] =
                                    s[((oh * 4 + br) * G + ih) * 1024 + r * 32 + c2];
        } else if (biasNeedsSwizzle(kv.first)) {
            // M9: attn_bias of the 1-head and 16-head window blocks is stored
            // in fused-kernel mma fragment order — recover the logical layout
            // (nr_model._fragment_swizzle_indices). 30x accuracy fix.
            const uint16_t* s = (const uint16_t*)src;
            uint16_t* d = (uint16_t*)dst;
            uint64_t heads = t.numel() / 4096;
            for (uint64_t h = 0; h < heads; ++h)
                for (uint32_t j = 0; j < 4096; ++j)
                    d[h * 4096 + j] = s[h * 4096 + SWZ[j]];
        } else {
            std::memcpy(dst, src, t.numel() * ((t.dtype == "F32") ? 4 : 2));
        }
    }

    // ---- fp32 side tables (widened f16 vectors at the vecOff slots) ----
    auto placeVecF16 = [&](const std::string& n) {
        auto it = poff.find(n);
        if (it == poff.end()) { std::fprintf(stderr, "missing vec tensor %s\n", n.c_str()); return; }
        auto vo = vecOff.find(n);
        if (vo == vecOff.end()) { std::fprintf(stderr, "vecOff slot missing for %s\n", n.c_str()); std::exit(1); }
        const Tensor& t = wf.tensors.at(n);
        const uint16_t* s = (const uint16_t*)(sp + it->second);
        float* d = (float*)(sp + vo->second);
        for (uint64_t i = 0; i < t.numel(); ++i) d[i] = f16_bits_to_f32(s[i]);
    };
    for (int i = 0; i <= 70; ++i) {
        if (i == 39) { placeVecF16("block39.layer0.inp_upsample_sin"); continue; }
        if (i >= 31 && i <= 38) {
            placeVecF16(tname("block%d.layer1.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer4.attn_cos_skip", i));
            continue;
        }
        if ((i >= 23 && i <= 30) || (i >= 40 && i <= 47)) {
            placeVecF16(tname("block%d.layer1.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer3.attn_cos_skip", i));
        } else {
            placeVecF16(tname("block%d.layer0.ffn_cos_skip", i));
            placeVecF16(tname("block%d.layer0.attn_cos_skip", i));
        }
        if (i == 48 || i == 56 || i == 62 || i == 66)
            placeVecF16(tname("block%d.layer0.sin", i));
        if (i == 70) {
            placeVecF16("block70.layer0.inp_merge_sin");
            placeVecF16("block70.layer0.inp_merge_cos");
        }
    }
    // attn_scale q32 side tables for the global blocks (x sqrt(32))
    for (int i = 31; i <= 38; ++i) {
        std::string n = tname("block%d.layer2.attn_scale", i);
        const Tensor& t = wf.tensors.at(n);
        const float* s = (const float*)(sp + poff.at(n));
        auto vo = vecOff.find(n + ".q32");
        if (vo == vecOff.end()) { std::fprintf(stderr, "vecOff slot missing for %s.q32\n", n.c_str()); std::exit(1); }
        float* d = (float*)(sp + vo->second);
        float sq = std::sqrt(32.0f);
        for (uint64_t j = 0; j < t.numel(); ++j) d[j] = s[j] * sq;
    }
    // ---- folded [32,16] f16 head weight (out_gain over out_conv_weight) ----
    {
        uint16_t* d = (uint16_t*)(sp + oHeadW);
        std::memset(d, 0, 1024);
        const uint16_t* sg = (const uint16_t*)(sp + poff.at("block70.layer0.out_gain"));
        const uint16_t* sc = (const uint16_t*)(sp + poff.at("block70.layer0.out_conv_weight"));
        for (int r = 0; r < 16; ++r)
            for (int cc = 0; cc < 4; ++cc) {
                d[r * 16 + cc] = sg[r * 4 + cc];
                d[(16 + r) * 16 + cc] = sc[r * 4 + cc];
            }
    }
}

} // namespace d5c
