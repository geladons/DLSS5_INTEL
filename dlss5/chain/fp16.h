// ============================================================================
// dlss5/chain — host-side f16 <-> f32 conversion (F16C fast path).
// Extracted VERBATIM from dlss5/m8b-live/main.cpp (M11 refactor).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>

namespace d5c {

inline bool g_hasF16C = false;

inline void InitFp16() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    g_hasF16C = (cpuInfo[2] & (1 << 29)) != 0;
}

inline uint16_t f32_to_f16_soft(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x7FFFFFu;
    int e = (int)((x >> 23) & 0xFFu);
    if (e == 255) {
        if (mant == 0) return (uint16_t)(sign | 0x7C00u);
        return (uint16_t)(sign | 0x7C00u | (uint16_t)(mant >> 13) | 1u);
    }
    int hexp = e - 112;
    if (hexp >= 31) return (uint16_t)(sign | 0x7C00u);
    if (hexp <= 0) {
        if (hexp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        int shift = 14 - hexp;
        uint32_t m = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1u);
        uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (m & 1u))) {
            m++;
            if (m == 0x400u) return (uint16_t)(sign | 0x400u);
        }
        return (uint16_t)(sign | m);
    }
    uint32_t m = mant >> 13;
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u))) {
        m++;
        if (m == 0x400u) { m = 0; hexp++; if (hexp >= 31) return (uint16_t)(sign | 0x7C00u); }
    }
    return (uint16_t)(sign | ((uint32_t)hexp << 10) | m);
}

inline uint16_t f32_to_f16(float f) {
    if (g_hasF16C) {
        __m128 v = _mm_set_ss(f);
        __m128i h = _mm_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT);
        return (uint16_t)_mm_extract_epi16(h, 0);
    }
    return f32_to_f16_soft(f);
}

inline float f16_bits_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t b;
    if (e == 0) {
        if (mant == 0) b = sign;
        else {
            int exp = -1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            b = sign | ((uint32_t)(114 + exp) << 23) | (mant << 13);
        }
    } else if (e == 31) {
        b = sign | 0x7F800000u | (mant << 13);
    } else {
        b = sign | ((e - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &b, 4);
    return out;
}

} // namespace d5c
