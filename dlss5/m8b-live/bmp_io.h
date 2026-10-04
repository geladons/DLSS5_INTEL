// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - BMP dump helper (extracted from main.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                  uint32_t w, uint32_t h);

// ============================================================================
