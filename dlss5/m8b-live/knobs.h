// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Live knobs: m13 writes %TEMP%\m13_screen_knobs.txt with
// "gain blend" floats; pollKnobs picks changes up by mtime (extracted).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

struct LiveKnobs {
    float gain = 1.0f, blend = 1.0f;
    std::string path;
    unsigned long long lastWrite = 0;   // last seen mtime (filetime quad)
    bool logged = false;
};

unsigned long long filetimeNow();
void pollKnobs(LiveKnobs& k);

// ============================================================================
