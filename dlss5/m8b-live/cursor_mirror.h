// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - CursorMirror: DXGI pointer-shape cache + position tracking +
// BGRA compositing (extracted from main.cpp, stage 3b). Owns all the former
// g_cur*/g_shape* file-scope state as members.
//
// BUG #3 (2026-09-21, REPRODUCED): the painted cursor NEVER erases. It enters
// bufLastPresented -> the next capture echoes it -> fbcancel sees
// echo(cursorOld) - lastPresented(cursorOld) = 0 -> nothing subtracts it ->
// every cursor position paints a PERMANENT ghost (measured: a 5x5 grid of 25
// ghost cursors after one wiggle pass). The user sees the REAL hardware
// cursor anyway (DWM draws it above our topmost overlay), so the paint is
// pure harm. DEFAULT IS NOW OFF; --cursor-draw keeps the legacy path.
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <vector>

class CursorMirror {
public:
    bool drawEnabled = false;                // --cursor-draw enables (default OFF, bug #3)

    // -------- DDA pointer-shape cache -------------------------------------
    std::vector<uint8_t> shapeBuf;           // raw DXGI pointer-shape buffer
    uint32_t shapeW = 0, shapeH = 0, shapePitch = 0;
    int  shapeType = 0;                      // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*
    bool shapeValid = false;

    // -------- pointer position/visibility ---------------------------------
    int  curX = -1000000, curY = -1000000;   // last-known pointer pos (desktop px)
    bool curVisible = false;
    bool curDirty = false;                   // pointer state changed since last processed frame

    // Pull the pointer shape + position from the duplication session when a
    // frame signals a new one (fi.PointerShapeBufferSize > 0). Must be called
    // between a successful AcquireNextFrame and ReleaseFrame.
    void fetchPointerState(IDXGIOutputDuplication* dup,
                           const DXGI_OUTDUPL_FRAME_INFO& fi);

    // GetCursorInfo fallback path for the 100 ms idle wake (DDA never fires
    // on cursor-only movement). Returns true when the pointer state changed.
    bool pollCursorFallback();

    // Alpha-blend the cached shape over the upload buffer. posX/posY is the
    // pointer position relative to the captured frame origin
    // (cur - output desktop offset). Returns CPU draw time in microseconds
    // (0 when nothing was drawn).
    double mirrorInto(uint8_t* dst, int posX, int posY, uint32_t W, uint32_t H);

    // Latch the drawn state so curDirty tracks real changes, and print the
    // periodic draw-overhead stat line.
    void markDrawn();

    bool needsRepaint() const { return curDirty; }

private:
    int  lastDrawnX_ = -1000000, lastDrawnY_ = -1000000;
    bool lastDrawnVis_ = false;
    long drawFrames_ = 0;
    double drawUsSum_ = 0.0;                 // avg CPU draw overhead accounting

    void drawBGRA(uint8_t* dst, int posX, int posY, uint32_t W, uint32_t H);
    void updateDirty() {
        curDirty = (curX != lastDrawnX_ || curY != lastDrawnY_ ||
                    curVisible != lastDrawnVis_);
    }
};
