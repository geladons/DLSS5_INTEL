/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * nr_util - leaf helpers shared by the layer and its split-out modules:
 * nr_log (%TEMP%\nr_layer_win.log + debugger), nr_now_ms, and the global
 * lock the guard/client modules serialize on (extracted stage 4).
 * ===================================================================== */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nr_log(const char *fmt, ...);
uint64_t nr_now_ms(void);

void nr_lock_init(void);
void nr_lock_destroy(void);
void lock(void);
void unlock(void);

#ifdef __cplusplus
}
#endif
