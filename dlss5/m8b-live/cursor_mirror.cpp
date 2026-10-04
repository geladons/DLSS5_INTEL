// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - CursorMirror implementation (see cursor_mirror.h). The blending
// math is verbatim from the old DrawCursorBGRA.
// ============================================================================
#include "cursor_mirror.h"

#include <chrono>
#include <cstdio>

void CursorMirror::fetchPointerState(IDXGIOutputDuplication* dup,
                                     const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.PointerShapeBufferSize > 0) {
        shapeBuf.resize(fi.PointerShapeBufferSize);
        UINT filled = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
        HRESULT hs = dup->GetFramePointerShape(fi.PointerShapeBufferSize, shapeBuf.data(),
                                               &filled, &si);
        if (SUCCEEDED(hs)) {
            shapeW = si.Width; shapeH = si.Height;
            shapePitch = si.Pitch; shapeType = (int)si.Type;
            shapeValid = true;
        }
    }
    curX = fi.PointerPosition.Position.x;
    curY = fi.PointerPosition.Position.y;
    curVisible = fi.PointerPosition.Visible != FALSE;
    updateDirty();
}

bool CursorMirror::pollCursorFallback() {
    CURSORINFO ci{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&ci)) return false;
    curX = ci.ptScreenPos.x; curY = ci.ptScreenPos.y;
    curVisible = (ci.flags & CURSOR_SHOWING) != 0;
    updateDirty();
    return curDirty;
}

// alpha-blend the cached DXGI pointer shape over a tight WxH B8G8R8A8 frame.
// posX/posY = pointer position (shape top-left, per the MS DDA sample), clipped.
void CursorMirror::drawBGRA(uint8_t* dst, int posX, int posY, uint32_t W, uint32_t H) {
    if (shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
        shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        for (uint32_t y = 0; y < shapeH; ++y) {
            const int dy = posY + (int)y;
            if (dy < 0 || dy >= (int)H) continue;
            const uint8_t* srow = shapeBuf.data() + (size_t)y * shapePitch;
            for (uint32_t x = 0; x < shapeW; ++x) {
                const int dx = posX + (int)x;
                if (dx < 0 || dx >= (int)W) continue;
                const uint8_t* s = srow + (size_t)x * 4;
                const unsigned a = s[3];
                if (a == 0) continue;
                uint8_t* d = dst + ((size_t)dy * W + (size_t)dx) * 4;
                if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
                for (int k = 0; k < 3; ++k)
                    d[k] = (uint8_t)((s[k] * a + d[k] * (255u - a) + 127u) / 255u);
                d[3] = 255;
            }
        }
        return;
    }
    // MONOCHROME: buffer = AND mask (H/2 rows, 1bpp) then XOR mask (H/2 rows, 32bpp)
    if (shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        const uint32_t halfH = shapeH / 2;
        for (uint32_t y = 0; y < halfH; ++y) {
            const int dy = posY + (int)y;
            if (dy < 0 || dy >= (int)H) continue;
            const uint8_t* andRow = shapeBuf.data() + (size_t)y * shapePitch;
            const uint8_t* xorRow = shapeBuf.data() + ((size_t)halfH + y) * shapePitch;
            for (uint32_t x = 0; x < shapeW; ++x) {
                const int dx = posX + (int)x;
                if (dx < 0 || dx >= (int)W) continue;
                const unsigned andBit = (andRow[x >> 3] >> (7 - (x & 7))) & 1u;
                if (andBit) continue;                    // transparent: keep desktop
                const uint32_t xc = *(const uint32_t*)(xorRow + (size_t)x * 4);
                if (xc == 0xFF000000u) continue;         // encoded transparent
                uint8_t* d = dst + ((size_t)dy * W + (size_t)dx) * 4;
                d[0] = (uint8_t)(xc & 0xFF); d[1] = (uint8_t)((xc >> 8) & 0xFF);
                d[2] = (uint8_t)((xc >> 16) & 0xFF); d[3] = 255;
            }
        }
    }
}

double CursorMirror::mirrorInto(uint8_t* dst, int posX, int posY, uint32_t W, uint32_t H) {
    if (!curVisible || !shapeValid) return 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    drawBGRA(dst, posX, posY, W, H);
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - t0).count();
    drawUsSum_ += us;
    ++drawFrames_;
    return us;
}

void CursorMirror::markDrawn() {
    lastDrawnX_ = curX; lastDrawnY_ = curY; lastDrawnVis_ = curVisible;
    curDirty = false;
    if (drawFrames_ > 0 && (drawFrames_ % 200) == 0)
        std::printf("[cursor] avg draw %.1f us over %ld drawn frames (shape %ux%u type %d)\n",
                    drawUsSum_ / (double)drawFrames_, drawFrames_,
                    shapeW, shapeH, shapeType);
}
