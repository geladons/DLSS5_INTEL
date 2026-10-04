// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Live knobs (see knobs.h).
#include "knobs.h"


// ============================================================================
unsigned long long filetimeNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void pollKnobs(LiveKnobs& k) {
    if (k.path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(k.path.c_str(), GetFileExInfoStandard, &fa))
        return;                          // absent - keep current values
    unsigned long long wt = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32)
                            | fa.ftLastWriteTime.dwLowDateTime;
    if (wt == k.lastWrite) return;
    k.lastWrite = wt;
    FILE* f = fopen(k.path.c_str(), "r");
    if (!f) return;
    float g = k.gain, b = k.blend;
    if (std::fscanf(f, "%f %f", &g, &b) == 2) {
        if (g < 0.0f) g = 0.0f; if (g > 16.0f) g = 16.0f;
        if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
        if (g != k.gain || b != k.blend || !k.logged) {
            std::printf("[knobs] live: gain %.3f blend %.3f\n", (double)g, (double)b);
            k.logged = true;
        }
        k.gain = g; k.blend = b;
    }
    fclose(f);
}