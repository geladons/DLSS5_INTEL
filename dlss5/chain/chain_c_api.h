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
//   frame_index: presentation frame counter (used for vendor noise seed)
// Returns 0 on success, non-zero on failure.
DLSS5_API int dlss5_process_frame(void* handle, uint32_t w, uint32_t h,
                                  const void* in_bgra, void* out_bgra,
                                  long frame_index);

// Runtime parameter updates:
DLSS5_API void dlss5_set_scale(void* handle, float scale);
DLSS5_API void dlss5_set_gain(void* handle, float gain);
DLSS5_API void dlss5_set_blend(void* handle, float blend);

// Frame Generation (FSR 3.1 / Optical Flow Interpolation):
// Interpolate an intermediate frame between frame0 and frame1 using GPU optical flow.
DLSS5_API int dlss5_interpolate_frame(void* handle, uint32_t w, uint32_t h,
                                      const void* frame0_bgra, const void* frame1_bgra,
                                      void* out_interp_bgra, float factor);
DLSS5_API void dlss5_set_framegen(void* handle, int enable);
DLSS5_API int dlss5_get_framegen(void* handle);
DLSS5_API float dlss5_get_framegen_gpu_ms(void* handle);

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
typedef int (*PFN_dlss5_interpolate_frame)(void* handle, uint32_t w, uint32_t h,
                                           const void* frame0_bgra, const void* frame1_bgra,
                                           void* out_interp_bgra, float factor);
typedef void (*PFN_dlss5_set_framegen)(void* handle, int enable);
typedef int (*PFN_dlss5_get_framegen)(void* handle);
typedef float (*PFN_dlss5_get_framegen_gpu_ms)(void* handle);

#ifdef __cplusplus
}
#endif

#endif // DLSS5_CHAIN_C_API_H
