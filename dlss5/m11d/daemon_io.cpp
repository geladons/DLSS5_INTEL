// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - byte-level IO primitives (see daemon_io.h).
// ============================================================================
#include "daemon_io.h"

bool ReadBmpBGRA(const std::string& path, std::vector<uint8_t>& bgra,
                 uint32_t& w, uint32_t& h) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    if (fread(&bfh, sizeof bfh, 1, f) != 1 || fread(&bih, sizeof bih, 1, f) != 1) {
        fclose(f); return false;
    }
    if (bfh.bfType != 0x4D42 || (bih.biBitCount != 32 && bih.biBitCount != 24) ||
        bih.biCompression != BI_RGB) { fclose(f); return false; }
    const bool topDown = bih.biHeight < 0;
    w = (uint32_t)bih.biWidth;
    h = (uint32_t)(topDown ? -bih.biHeight : bih.biHeight);
    const uint32_t bpp = bih.biBitCount / 8;
    const uint32_t pitch = (w * bpp + 3) & ~3u;
    std::vector<uint8_t> raw((size_t)pitch * h);
    if (fseek(f, (long)bfh.bfOffBits, SEEK_SET) != 0) { fclose(f); return false; }
    if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); return false; }
    fclose(f);
    bgra.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + (size_t)(topDown ? y : (h - 1 - y)) * pitch;
        uint8_t* dst = bgra.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * bpp + 0];
            dst[x * 4 + 1] = src[x * bpp + 1];
            dst[x * 4 + 2] = src[x * bpp + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    return true;
}

bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                  uint32_t w, uint32_t h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t imgBytes = w * 4 * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgBytes;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    fwrite(&bfh, sizeof bfh, 1, f);
    fwrite(&bih, sizeof bih, 1, f);
    for (uint32_t y = h; y-- > 0;)
        fwrite(&bgra[(size_t)y * w * 4], 1, w * 4, f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool readAll(SOCKET s, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(s, (char*)dst + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

bool readAllDeadline(SOCKET s, uint8_t* dst, size_t n,
                     ULONGLONG deadlineMs) {
    size_t got = 0;
    while (got < n) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadlineMs) return false;
        ULONGLONG remain = deadlineMs - now;
        DWORD tv = (DWORD)(remain > 20000 ? 20000 : remain);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        int r = recv(s, (char*)dst + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

bool writeAll(SOCKET s, const uint8_t* src, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, (const char*)src + sent, (int)(n - sent), 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}
