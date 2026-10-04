// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - f32<->f16 conversion helpers (extracted from main.cpp).
#pragma once

#include <cstdint>

extern bool g_hasF16C;   // set once at startup (CPUID); f32_to_f16 picks the path

uint16_t f32_to_f16_soft(float f);
uint16_t f32_to_f16(float f);
float    f16_bits_to_f32(uint16_t h);

// ============================================================================
