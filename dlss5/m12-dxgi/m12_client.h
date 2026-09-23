// m12_client.h - TCP exchange with the m11d daemon.
// Wire protocol identical to m11-layer/nr_layer_win.c:
//   16-byte header {magic, w, h, format} (uint32 LE) + w*h*4 BGRA payload,
//   fixed-size reply (same byte count as the pixel payload).
#pragma once

#include <cstddef>
#include <cstdint>

// Returns 0 on success, -1 on any failure (daemon absent, timeout, ...).
int m12_exchange(const uint32_t header[4], const void *payload,
                 std::size_t payload_size, void *reply, std::size_t reply_size);
