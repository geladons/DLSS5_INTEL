/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * daemon_client - TCP exchange with the m11d daemon (16-byte header,
 * payload in, fixed reply out; Unix socket in the Linux original).
 * Extracted stage 4. `nr_port` is the layer-wide daemon port config,
 * owned here because every consumer of it is in this module plus the
 * env-parse write.
 * ===================================================================== */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_DEFAULT_PORT 48990

extern long nr_port;

/* 0 = ok; -1 = transport failure (connected, send/recv timed out or broke -
 * the daemon is alive but busy); -2 = connect refused (daemon absent). The
 * flap policy in nr_degrade.c treats the two differently. */
int exchange(const void *header, size_t header_size, const void *payload,
             size_t payload_size, void *reply, size_t reply_size);

/* Fire a 16-byte NRCT control command through the same bounded exchange
 * (header-only, 16-byte reply). 0 = daemon ok; <0 transport; 1 = rejected. */
int nr_ctrl(uint32_t cmd, uint32_t payload_arg);

#ifdef __cplusplus
}
#endif
