// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - SEH containment wrappers (see engine_seh.h).
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

#include "engine_seh.h"
#include "engine.h"

bool sehProcessFrame(d5c::ChainEngine* eng, const uint8_t* in,
                     uint8_t* out, uint32_t frame, d5c::FrameStats* st) {
    __try {
        return eng->processFrame(in, out, frame, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside processFrame (frame %u) - engine state may be dirty\n",
                     (unsigned long)GetExceptionCode(), (unsigned)frame);
        return false;
    }
}

bool sehTransferFrame(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                      const uint8_t* in, uint8_t* out, d5c::FrameStats* st) {
    __try {
        return eng->transferFrame(w, h, in, out, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside transferFrame - falling back to a full process\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}

bool sehInterpAnchors(d5c::ChainEngine* eng, uint32_t w, uint32_t h,
                      const uint8_t* mid, uint8_t* out, d5c::FrameStats* st) {
    __try {
        return eng->interpolateAnchors(w, h, mid, out, 0.5f, st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside interpolateAnchors - falling back to a full process\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}

bool sehEngineInit(d5c::ChainEngine* eng, const d5c::EngineConfig* cfg) {
    __try {
        return eng->init(*cfg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside engine.init\n",
                     (unsigned long)GetExceptionCode());
        return false;
    }
}

void sehEngineShutdown(d5c::ChainEngine* eng) {
    __try {
        eng->shutdown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[m11d] SEH 0x%08lx inside engine.shutdown\n",
                     (unsigned long)GetExceptionCode());
    }
}
