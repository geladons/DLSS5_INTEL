// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - BMP dump helper (see bmp_io.h).
#include "bmp_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>


// ============================================================================
bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                         uint32_t w, uint32_t h) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t rowBytes = w * 4;
    const uint32_t imgBytes = rowBytes * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgBytes;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h; // positive -> bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    f.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    for (uint32_t y = h; y-- > 0;) {
        const uint8_t* row = &bgra[(size_t)y * rowBytes];
        f.write(reinterpret_cast<const char*>(row), rowBytes);
        if (!f) return false;
    }
    return f.good();
}