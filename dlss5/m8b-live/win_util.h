// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Win32 helpers: window find, HRESULT/LUID names, wiggle thread,
// the Region rect type (extracted from main.cpp).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <string>

struct Region { int top, bottom, left, right; };

const char*  HrName(HRESULT hr);
std::string  LuidStr(const LUID& l);
bool         StrContainsI(const char* hay, const char* needle);
HWND         FindWindowByTitle(const std::string& needle);

// Wiggle thread: keeps DDA delivering frames on an idle desktop by
// jiggling the cursor via SendInput (host-verified input path).
extern std::atomic<bool> g_wiggleStop;
void WiggleThread();

// ============================================================================
