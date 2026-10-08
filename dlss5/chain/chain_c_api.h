// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - C API for in-process DLSS 5 engine integration.
// Exposes a clean extern "C" ABI so consumers (like nr_layer_win) can drive
// DLSS 5 directly in-process without network overhead or C++ symbol dependencies.
// ============================================================================
#ifndef DLSS5_CHAIN_C_API_H
#define DLSS5_CHAIN_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #if defined(DLSS5_EXPORTS)
    #define DLSS5_API __declspec(dllexport)
  #elif defined(DLSS5_IMPORTS)
    #define DLSS5_API __declspec(dllimport)
  #else
    #define DLSS5_API
  #endif
#else
  #define DLSS5_API
#endif

// Create a DLSS 5 engine context.
// Parameters:
//   weights_path: path to dlssnr-logical.safetensors (or NULL for default / env lookup)
//   shader_dir: directory containing *.spv compute shaders (or NULL for default / env lookup)
//   scale: render scaling factor (0.33 to 1.0, 1.0 = native)
//   gain: vendor residual scale (default 1.0)
//   blend: vendor mix factor (default 1.0)
// Returns opaque handle to DLSS 5 context, or NULL on error.
DLSS5_API void* dlss5_create(const char* weights_path, const char* shader_dir,
                             float scale, float gain, float blend);

// Destroy DLSS 5 engine context and release all GPU resources.
DLSS5_API void dlss5_destroy(void* handle);

// Process a single BGRA8 frame in-place or out-of-place.
// Parameters:
//   handle: engine handle returned by dlss5_create
//   w, h: frame width and height in pixels
//   in_bgra: source pixel buffer (w * h * 4 bytes)
//   out_bgra: destination pixel buffer (w * h * 4 bytes; can be == in_bgra)
//   frame_index: presentation frame counter (profiling/dump naming; the vendor noise seed is pinned)
// Returns 0 on success, non-zero on failure.
DLSS5_API int dlss5_process_frame(void* handle, uint32_t w, uint32_t h,
                                  const void* in_bgra, void* out_bgra,
                                  long frame_index);

// Runtime parameter updates:
DLSS5_API void dlss5_set_scale(void* handle, float scale);
DLSS5_API void dlss5_set_gain(void* handle, float gain);
DLSS5_API void dlss5_set_blend(void* handle, float blend);
DLSS5_API void dlss5_set_sharpness(void* handle, float sharpness);

// NVIDIA Phase 30 vendor parameter controls:
DLSS5_API void dlss5_set_phase30(void* handle,
                                 float intensity,
                                 float local_tone,
                                 float local_structure,
                                 float skin_structure,
                                 int auto_mask,
                                 int style_index,
                                 float color_strength);
DLSS5_API void dlss5_set_intensity(void* handle, float intensity);
DLSS5_API void dlss5_set_local_tone(void* handle, float local_tone);
DLSS5_API void dlss5_set_local_structure(void* handle, float local_structure);
DLSS5_API void dlss5_set_skin_structure(void* handle, float skin_structure);
DLSS5_API void dlss5_set_auto_mask(void* handle, int auto_mask);
DLSS5_API void dlss5_set_style_index(void* handle, int style_index);
DLSS5_API void dlss5_set_color_strength(void* handle, float color_strength);

// Temporal conditioning (features ch 7-9 = previous output + temporal_nr
// compose). Default on; set_temporal(0) or env DLSS5_TEMPORAL=0 disables.
// reset_history drops the anchor: next frame runs the first-frame path.
DLSS5_API void dlss5_set_temporal(void* handle, int enable);
DLSS5_API int dlss5_get_temporal(void* handle);
DLSS5_API void dlss5_reset_history(void* handle);
// Diagnostics (non-contract): dump internal buffer `which` to `path` after the
// last frame. 0=history fp32[TOK*3], 1=featV fp32[TOK*16], 2=head fp32[TOK*4],
// 3=x16 fp16[TOK*16], 4=stats u32[8], 5=netRgb fp32[netW*netH*3].
DLSS5_API int dlss5_debug_fetch(void* handle, int which, const char* path);

// Frame Generation (FSR 3.1 / Optical Flow Interpolation):
// Interpolate an intermediate frame between frame0 and frame1 using GPU optical flow.
DLSS5_API int dlss5_interpolate_frame(void* handle, uint32_t w, uint32_t h,
                                      const void* frame0_bgra, const void* frame1_bgra,
                                      void* out_interp_bgra, float factor);
// Frame generation (1:2 cadence): builds a generated frame from the current
// raw present using the engine's own copy of the last processed frame and its
// raw input (residual transfer along raw->raw motion). Only cur_raw is uploaded.
// Returns 0 on success, non-zero if no processed frame exists yet or size mismatch.
DLSS5_API int dlss5_framegen_transfer(void* handle, uint32_t w, uint32_t h,
                                      const void* cur_raw_bgra, void* out_bgra);
DLSS5_API void dlss5_set_framegen(void* handle, int enable);
DLSS5_API int dlss5_get_framegen(void* handle);
DLSS5_API float dlss5_get_framegen_gpu_ms(void* handle);
// Frame-generation mode: 1 = residual transfer (+0 latency), 2 = anchor
// interpolation (+1 frame, Lossless-Scaling-class). Mode 2 synthesizes the
// midpoint of the two most recent processed anchors; mid_raw_bgra is the
// REAL captured middle frame used as the confidence fallback (may be NULL -
// unwarped anchor blend instead). factor is the interval position (0.5 for
// the 1:2 cadence). Mode 2 requires >=1 processed anchor before it can
// return a frame (copies the newest anchor until a second pair exists).
DLSS5_API int dlss5_framegen_interpolate(void* handle, uint32_t w, uint32_t h,
                                         const void* mid_raw_bgra,
                                         void* out_bgra, float factor);
DLSS5_API void dlss5_set_framegen_mode(void* handle, int mode);
DLSS5_API int dlss5_get_framegen_mode(void* handle);

// Function pointer types for dynamic loading via LoadLibraryA / GetProcAddress:
typedef void* (*PFN_dlss5_create)(const char* weights_path, const char* shader_dir,
                                  float scale, float gain, float blend);
typedef void (*PFN_dlss5_destroy)(void* handle);
typedef int (*PFN_dlss5_process_frame)(void* handle, uint32_t w, uint32_t h,
                                       const void* in_bgra, void* out_bgra,
                                       long frame_index);
typedef void (*PFN_dlss5_set_scale)(void* handle, float scale);
typedef void (*PFN_dlss5_set_gain)(void* handle, float gain);
typedef void (*PFN_dlss5_set_blend)(void* handle, float blend);
typedef void (*PFN_dlss5_set_sharpness)(void* handle, float sharpness);
typedef void (*PFN_dlss5_set_phase30)(void* handle,
                                      float intensity,
                                      float local_tone,
                                      float local_structure,
                                      float skin_structure,
                                      int auto_mask,
                                      int style_index,
                                      float color_strength);
typedef void (*PFN_dlss5_set_intensity)(void* handle, float intensity);
typedef void (*PFN_dlss5_set_local_tone)(void* handle, float local_tone);
typedef void (*PFN_dlss5_set_local_structure)(void* handle, float local_structure);
typedef void (*PFN_dlss5_set_skin_structure)(void* handle, float skin_structure);
typedef void (*PFN_dlss5_set_auto_mask)(void* handle, int auto_mask);
typedef void (*PFN_dlss5_set_style_index)(void* handle, int style_index);
typedef void (*PFN_dlss5_set_color_strength)(void* handle, float color_strength);
typedef int (*PFN_dlss5_interpolate_frame)(void* handle, uint32_t w, uint32_t h,
                                           const void* frame0_bgra, const void* frame1_bgra,
                                           void* out_interp_bgra, float factor);
typedef int (*PFN_dlss5_framegen_transfer)(void* handle, uint32_t w, uint32_t h,
                                           const void* cur_raw_bgra, void* out_bgra);
typedef void (*PFN_dlss5_set_framegen)(void* handle, int enable);
typedef int (*PFN_dlss5_get_framegen)(void* handle);
typedef float (*PFN_dlss5_get_framegen_gpu_ms)(void* handle);
typedef int (*PFN_dlss5_framegen_interpolate)(void* handle, uint32_t w, uint32_t h,
                                              const void* mid_raw_bgra,
                                              void* out_bgra, float factor);
typedef void (*PFN_dlss5_set_framegen_mode)(void* handle, int mode);
typedef int (*PFN_dlss5_get_framegen_mode)(void* handle);
typedef void (*PFN_dlss5_set_temporal)(void* handle, int enable);
typedef int (*PFN_dlss5_get_temporal)(void* handle);
typedef void (*PFN_dlss5_reset_history)(void* handle);

#ifdef __cplusplus
}
#endif

#endif // DLSS5_CHAIN_C_API_H
