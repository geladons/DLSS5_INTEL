// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Safetensors weights parser + geometry structs + fragment
// swizzle (extracted from main.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Stage { uint32_t H, W, TOK; };

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

uint32_t pad8u(uint32_t v);
void     initSwizzle();          // fills the SWZ table - call once at startup
extern uint32_t SWZ[4096];       // logical[j] = stored[SWZ[j]] for attn_bias
int      blockIndexOf(const std::string &name);
bool     biasNeedsSwizzle(const std::string &n);
int      readFile(const std::string &path, std::vector<char> &out);
void     stParse(const std::string &path, WeightsFile &wf);

// ============================================================================
