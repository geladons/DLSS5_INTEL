// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - byte-level IO primitives: BMP stills (selftest/validation dumps)
// and blocking socket transfers with a whole-request deadline.
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------- BMP I/O --
bool ReadBmpBGRA(const std::string& path, std::vector<uint8_t>& bgra,
                 uint32_t& w, uint32_t& h);
bool WriteBmpBGRA(const std::string& path, const std::vector<uint8_t>& bgra,
                  uint32_t w, uint32_t h);

// ---------------------------------------------------------------- sockets --
bool readAll(SOCKET s, uint8_t* dst, size_t n);

// Deadline variant: SO_RCVTIMEO is per-recv, so a client dribbling one byte
// per 19 s would pin the single-threaded accept loop forever (frames and
// NRCT probes pile into the backlog). Bound the whole request instead.
bool readAllDeadline(SOCKET s, uint8_t* dst, size_t n, ULONGLONG deadlineMs);

bool writeAll(SOCKET s, const uint8_t* src, size_t n);
