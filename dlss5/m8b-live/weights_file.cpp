// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Safetensors weights parser (see weights_file.h).
#include "weights_file.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>


// ============================================================================
uint32_t pad8u(uint32_t v) { return (v + 7u) & ~7u; }


// Fragment swizzle (nr_model._fragment_swizzle_indices): the single-head
// window blocks (0-4, 66-70) and the 16-head split blocks (23-30, 40-47) store
// attn_bias in fused-kernel mma fragment order; logical[j] = stored[SWZ[j]].
// The 2/4/8-head blocks (5-22, 48-65) store the logical layout already.
uint32_t SWZ[4096];
void initSwizzle() {
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
int blockIndexOf(const std::string &name) {
    auto dot = name.find('.');
    return std::atoi(name.substr(5, dot - 5).c_str());
}
bool biasNeedsSwizzle(const std::string &n) {
    auto ends = [&](const char *suf) {
        size_t l = std::strlen(suf);
        return n.size() >= l && n.compare(n.size() - l, l, suf) == 0;
    };
    int blk = blockIndexOf(n);
    if (ends(".layer0.attn_bias")) return blk <= 4 || blk >= 66;
    if (ends(".layer2.attn_bias")) return (blk >= 23 && blk <= 30) || (blk >= 40 && blk <= 47);
    return false;
}

int readFile(const std::string &path, std::vector<char> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 1;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (!f.read(out.data(), sz)) return 1;
    return 0;
}

void stParse(const std::string &path, WeightsFile &wf) {
    if (readFile(path, wf.bytes)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
    if (wf.bytes.size() < 8) { std::fprintf(stderr, "safetensors too small\n"); std::exit(1); }
    uint64_t hlen;
    std::memcpy(&hlen, wf.bytes.data(), 8);
    if (8 + hlen > wf.bytes.size()) { std::fprintf(stderr, "bad header len\n"); std::exit(1); }
    const char *p = wf.bytes.data() + 8;
    const char *end = p + hlen;
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