// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - SEH containment wrappers around ChainEngine calls (see .cpp).
// ============================================================================
#pragma once

#include <cstdint>

namespace d5c {
class ChainEngine;
struct EngineConfig;
struct FrameStats;
}

// ------------------------------------------------------- crash hardening --
// A fault inside the Vulkan ICD / engine (driver AV, etc.) must not kill the
// daemon silently: translate it into a logged frame failure so the caller can
// echo the input and keep serving. POD-only frames: no C++ unwinding inside
// __try (MSVC rule), objects are passed in by pointer.
bool sehProcessFrame(d5c::ChainEngine* eng, const uint8_t* in,
                     uint8_t* out, uint32_t frame, d5c::FrameStats* st);
// Same containment for the framegen residual-transfer pass (driver faults
// there are rarer - one dispatch - but a crashed daemon answers nothing).
bool sehTransferFrame(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                      const uint8_t* in, uint8_t* out, d5c::FrameStats* st);
// Same containment for the mode-2 anchor interpolation (the payload `mid`
// is the REAL middle frame, not the current raw - do not confuse with
// transferFrame's curRaw upload).
bool sehInterpAnchors(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                      const uint8_t* mid, uint8_t* out, d5c::FrameStats* st);
bool sehEngineInit(d5c::ChainEngine* eng, const d5c::EngineConfig* cfg);
void sehEngineShutdown(d5c::ChainEngine* eng);
