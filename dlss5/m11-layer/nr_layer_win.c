/*
 * nr_layer_win - Windows port of nr_layer.c (dlss-nr-on-intel, src/layer).
 *
 * A Vulkan implicit layer that sits at vkQueuePresentKHR: forces
 * TRANSFER_SRC|DST onto swapchain images at vkCreateSwapchainKHR, copies the
 * presented frame out, hands it to a local daemon over TCP, and copies the
 * processed frame back before the present. No desktop capture, no overlay,
 * no feedback loop: the game's own pixels are what changes.
 *
 * Port notes (Windows): pthread -> CRITICAL_SECTION, Unix socket -> TCP
 * 127.0.0.1:NR_LAYER_PORT (default 48990), access() -> _access(), stderr ->
 * %TEMP%\nr_layer_win.log. Logic mirrors the Linux original 1:1; the default
 * sync model stays "idle" (vkQueueWaitIdle), NR_LAYER_SYNC=semaphore enables
 * the present-semaphore ring.
 *
 * Env: ENABLE_NR_LAYER=1 arms the layer (manifest enable_environment).
 *   NR_LAYER_PORT     daemon TCP port (default 48990)
 *   NR_LAYER_TRIGGER  while this file exists: process + hold (photo mode)
 *   NR_LAYER_LIVE N   every Nth present processed; between frames re-blit the
 *                     last result (slideshow mode). N=1 = every present.
 *                     Arms CTRL+ALT+X (pause/resume passthrough) and
 *                     CTRL+ALT+Q (layer off) hotkeys.
 *   NR_LAYER_NOPATCH  1 = do not touch swapchains (pure passthrough; bisect)
 *   NR_LAYER_FREEZE   while this file exists (live mode only): the last raw
 *                     frame is HELD - every present re-blits the processed
 *                     result so the picture freezes while the game runs on
 *   NR_LAYER_REPROC   mtime change of this file while frozen: re-send the
 *                     held RAW frame to the daemon (it reprocesses with the
 *                     current gain) and hold the fresh result. This is the
 *                     in-game "photo mode with live knobs" (m13 overlay).
 *   NR_LAYER_CAPTURE  write the next captured frame (header+pixels) here
 *   NR_LAYER_EVERY N  capture every Nth present even without a trigger
 *   NR_LAYER_SYNC     "semaphore" = present-semaphore ring; default = idle
 *   NR_LAYER_UI_MASK  0 = disable exact-repeat reuse of the last processed
 *                     result (default on; legacy name)
 *
 * Build: see CMakeLists.txt (x64; x86 build needed for 32-bit games).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "chain_c_api.h"

#define MAX_SWAPCHAINS 8
#define MAX_IMAGES 8
#define SYNC_SLOTS 4
#define NR_MAX_WAITS 16
#define NR_DEFAULT_PORT 48990

static int sync_semaphores;

typedef struct device_data {
    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkDeviceMemory staging_memory;
    VkBuffer staging;
    VkDeviceSize staging_size;
    void *mapped;
    unsigned char *result;
    VkDeviceSize result_size;
    int holding;
    VkSwapchainKHR holding_chain; /* the swapchain the held result belongs to */
    unsigned char *earlier;
    VkDeviceSize earlier_size;
    VkCommandBuffer ring_commands[SYNC_SLOTS];
    VkFence ring_fence[SYNC_SLOTS];
    VkSemaphore ring_done[SYNC_SLOTS];
    int ring_used[SYNC_SLOTS];
    unsigned ring_next;
    int ring_ready;
    const VkSemaphore *present_wait;
    uint32_t present_wait_count;
    VkSemaphore present_signal;
    int present_plain;
    int have_earlier;
    unsigned char *raw;          /* freeze mode: the held UNPROCESSED frame */
    VkDeviceSize raw_size;
    int raw_held;
    FILETIME reproc_stamp;       /* last seen NR_LAYER_REPROC mtime */
    int reproc_seen;
    unsigned char *interp;
    VkDeviceSize interp_size;
    unsigned char *prev_processed;
    VkDeviceSize prev_processed_size;
    int have_prev_processed;
    PFN_vkGetDeviceProcAddr get_device_proc;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetDeviceQueue2 get_device_queue2;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkDestroyDevice destroy_device;
} device_data;

typedef struct swapchain_data {
    VkSwapchainKHR swapchain;
    VkDevice device;
    VkImage images[MAX_IMAGES];
    uint32_t image_count;
    VkFormat format;
    VkExtent2D extent;
} swapchain_data;

typedef struct queue_data {
    VkQueue queue;
    VkDevice device;
    uint32_t family;
    int capture_ok;
} queue_data;

#define MAX_QUEUES 16

static device_data devices[8];
static swapchain_data swapchains[MAX_SWAPCHAINS];
static queue_data queues[MAX_QUEUES];

static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static PFN_vkGetInstanceProcAddr next_instance_proc;
static VkInstance layer_instance;
static unsigned long frame_counter;
static const char *capture_path;
static const char *capture_out_path;
static long capture_every;
static long live_every;
static long nr_port = NR_DEFAULT_PORT;
static const char *trigger_path;
static const char *freeze_path;
static const char *reproc_path;
static char default_freeze_path[MAX_PATH];
static char default_reproc_path[MAX_PATH];
static int static_reuse;
static int no_patch;            /* NR_LAYER_NOPATCH=1: leave swapchains alone */
static volatile LONG g_paused;  /* CTRL+ALT+X: passthrough, full fps */
static volatile LONG g_off;     /* CTRL+ALT+Q: layer off for good */
static volatile LONG g_hotkeys_started;

static void init_freeze_paths(void)
{
    if (default_freeze_path[0] == '\0') {
        char temp_dir[MAX_PATH];
        DWORD len = GetTempPathA(sizeof(temp_dir), temp_dir);
        if (len > 0 && len < sizeof(temp_dir)) {
            if (temp_dir[len - 1] != '\\' && temp_dir[len - 1] != '/') {
                snprintf(default_freeze_path, sizeof(default_freeze_path), "%s\\m13_freeze.flag", temp_dir);
                snprintf(default_reproc_path, sizeof(default_reproc_path), "%s\\m13_reproc.flag", temp_dir);
            } else {
                snprintf(default_freeze_path, sizeof(default_freeze_path), "%sm13_freeze.flag", temp_dir);
                snprintf(default_reproc_path, sizeof(default_reproc_path), "%sm13_reproc.flag", temp_dir);
            }
        } else {
            const char *t = getenv("TEMP");
            snprintf(default_freeze_path, sizeof(default_freeze_path), "%s\\m13_freeze.flag", t ? t : ".");
            snprintf(default_reproc_path, sizeof(default_reproc_path), "%s\\m13_reproc.flag", t ? t : ".");
        }
    }
    if (!freeze_path || !freeze_path[0]) {
        const char *env_f = getenv("NR_LAYER_FREEZE");
        freeze_path = (env_f && env_f[0]) ? env_f : default_freeze_path;
    }
    if (!reproc_path || !reproc_path[0]) {
        const char *env_r = getenv("NR_LAYER_REPROC");
        reproc_path = (env_r && env_r[0]) ? env_r : default_reproc_path;
    }
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static void nr_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    char path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, path)) {
        strncat(path, "nr_layer_win.log", MAX_PATH - strlen(path) - 1);
        FILE *f = fopen(path, "a");
        if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
    }
    OutputDebugStringA("[nr_layer] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

/* ------------------------------------------------------------------ */
/* In-process DLSS 5 engine integration (Stage 3: TCP elimination).    */
/* ------------------------------------------------------------------ */
static HMODULE g_chain_dll = NULL;
static void *g_engine = NULL;
static PFN_dlss5_create pfn_dlss5_create = NULL;
static PFN_dlss5_destroy pfn_dlss5_destroy = NULL;
static PFN_dlss5_process_frame pfn_dlss5_process_frame = NULL;
static PFN_dlss5_set_scale pfn_dlss5_set_scale = NULL;
static PFN_dlss5_set_gain pfn_dlss5_set_gain = NULL;
static PFN_dlss5_set_blend pfn_dlss5_set_blend = NULL;
static PFN_dlss5_set_sharpness pfn_dlss5_set_sharpness = NULL;
static PFN_dlss5_interpolate_frame pfn_dlss5_interpolate_frame = NULL;
static PFN_dlss5_set_framegen pfn_dlss5_set_framegen = NULL;
static PFN_dlss5_get_framegen pfn_dlss5_get_framegen = NULL;
static PFN_dlss5_set_phase30 pfn_dlss5_set_phase30 = NULL;
static PFN_dlss5_set_intensity pfn_dlss5_set_intensity = NULL;
static PFN_dlss5_set_local_tone pfn_dlss5_set_local_tone = NULL;
static PFN_dlss5_set_local_structure pfn_dlss5_set_local_structure = NULL;
static PFN_dlss5_set_skin_structure pfn_dlss5_set_skin_structure = NULL;
static PFN_dlss5_set_auto_mask pfn_dlss5_set_auto_mask = NULL;
static PFN_dlss5_set_style_index pfn_dlss5_set_style_index = NULL;
static PFN_dlss5_set_color_strength pfn_dlss5_set_color_strength = NULL;
static int g_inprocess_attempted = 0;
static int g_inprocess_active = 0;
static float g_cur_scale = -1.0f;
static float g_cur_gain = -1.0f;
static float g_cur_blend = -1.0f;
static float g_cur_sharpness = -1.0f;
static float g_cur_intensity = -1.0f;
static float g_cur_local_tone = -1.0f;
static float g_cur_local_structure = -1.0f;
static float g_cur_skin_structure = -999.0f;
static int g_cur_auto_mask = -1;
static int g_cur_style_index = -1;
static float g_cur_color_strength = -1.0f;

static int is_framegen_enabled(void)
{
    const char *fg = getenv("NR_LAYER_FRAMEGEN");
    if (!fg) fg = getenv("DLSS5_FRAMEGEN");
    if (fg) {
        if (fg[0] == '1' || !strcmp(fg, "true") || !strcmp(fg, "on")) return 1;
        if (fg[0] == '0' || !strcmp(fg, "false") || !strcmp(fg, "off")) return 0;
    }
    if (g_engine && pfn_dlss5_get_framegen) {
        return pfn_dlss5_get_framegen(g_engine) == 1;
    }
    return 0;
}

static void update_live_knobs(void)
{
    if (!g_engine) return;

    const char *scale_s = getenv("NR_LAYER_SCALE");
    if (!scale_s) scale_s = getenv("DLSS5_SCALE");
    if (scale_s) {
        float val = (float)atof(scale_s);
        float diff = val - g_cur_scale;
        if (diff < 0.0f) diff = -diff;
        if (val > 0.0f && diff > 1e-4f) {
            g_cur_scale = val;
            if (pfn_dlss5_set_scale) {
                pfn_dlss5_set_scale(g_engine, val);
                nr_log("[nr_layer] live scale updated: %.2f", val);
            }
        }
    }

    const char *gain_s = getenv("NR_LAYER_GAIN");
    if (!gain_s) gain_s = getenv("DLSS5_GAIN");
    if (gain_s) {
        float val = (float)atof(gain_s);
        float diff = val - g_cur_gain;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && diff > 1e-4f) {
            g_cur_gain = val;
            if (pfn_dlss5_set_gain) {
                pfn_dlss5_set_gain(g_engine, val);
                nr_log("[nr_layer] live gain updated: %.2f", val);
            }
        }
    }

    const char *blend_s = getenv("NR_LAYER_BLEND");
    if (!blend_s) blend_s = getenv("DLSS5_BLEND");
    if (blend_s) {
        float val = (float)atof(blend_s);
        float diff = val - g_cur_blend;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && val <= 1.0f && diff > 1e-4f) {
            g_cur_blend = val;
            if (pfn_dlss5_set_blend) {
                pfn_dlss5_set_blend(g_engine, val);
                nr_log("[nr_layer] live blend updated: %.2f", val);
            }
        }
    }

    const char *sharp_s = getenv("NR_LAYER_SHARPNESS");
    if (!sharp_s) sharp_s = getenv("DLSS5_SHARPNESS");
    if (sharp_s) {
        float val = (float)atof(sharp_s);
        float diff = val - g_cur_sharpness;
        if (diff < 0.0f) diff = -diff;
        if (val >= 0.0f && diff > 1e-4f) {
            g_cur_sharpness = val;
            if (pfn_dlss5_set_sharpness) {
                pfn_dlss5_set_sharpness(g_engine, val);
                nr_log("[nr_layer] live sharpness updated: %.2f", val);
            }
        }
    }

    const char *fg_s = getenv("NR_LAYER_FRAMEGEN");
    if (!fg_s) fg_s = getenv("DLSS5_FRAMEGEN");
    if (fg_s && pfn_dlss5_set_framegen) {
        int enable = (fg_s[0] == '1' || !strcmp(fg_s, "true") || !strcmp(fg_s, "on")) ? 1 : 0;
        pfn_dlss5_set_framegen(g_engine, enable);
    }

    const char *intensity_s = getenv("NR_LAYER_INTENSITY");
    if (!intensity_s) intensity_s = getenv("DLSS5_INTENSITY");
    if (intensity_s && pfn_dlss5_set_intensity) {
        float val = (float)atof(intensity_s);
        float diff = val - g_cur_intensity;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_intensity = val;
            pfn_dlss5_set_intensity(g_engine, val);
            nr_log("[nr_layer] live intensity updated: %.2f", val);
        }
    }

    const char *lt_s = getenv("NR_LAYER_LOCAL_TONE");
    if (!lt_s) lt_s = getenv("DLSS5_LOCAL_TONE");
    if (lt_s && pfn_dlss5_set_local_tone) {
        float val = (float)atof(lt_s);
        float diff = val - g_cur_local_tone;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_local_tone = val;
            pfn_dlss5_set_local_tone(g_engine, val);
            nr_log("[nr_layer] live local_tone updated: %.2f", val);
        }
    }

    const char *ls_s = getenv("NR_LAYER_LOCAL_STRUCTURE");
    if (!ls_s) ls_s = getenv("DLSS5_LOCAL_STRUCTURE");
    if (ls_s && pfn_dlss5_set_local_structure) {
        float val = (float)atof(ls_s);
        float diff = val - g_cur_local_structure;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_local_structure = val;
            pfn_dlss5_set_local_structure(g_engine, val);
            nr_log("[nr_layer] live local_structure updated: %.2f", val);
        }
    }

    const char *ss_s = getenv("NR_LAYER_SKIN_STRUCTURE");
    if (!ss_s) ss_s = getenv("DLSS5_SKIN_STRUCTURE");
    if (ss_s && pfn_dlss5_set_skin_structure) {
        float val = (float)atof(ss_s);
        float diff = val - g_cur_skin_structure;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_skin_structure = val;
            pfn_dlss5_set_skin_structure(g_engine, val);
            nr_log("[nr_layer] live skin_structure updated: %.2f", val);
        }
    }

    const char *am_s = getenv("NR_LAYER_AUTO_MASK");
    if (!am_s) am_s = getenv("DLSS5_AUTO_MASK");
    if (am_s && pfn_dlss5_set_auto_mask) {
        int val = (am_s[0] == '1' || !strcmp(am_s, "true") || !strcmp(am_s, "on")) ? 1 : 0;
        if (val != g_cur_auto_mask) {
            g_cur_auto_mask = val;
            pfn_dlss5_set_auto_mask(g_engine, val);
            nr_log("[nr_layer] live auto_mask updated: %d", val);
        }
    }

    const char *si_s = getenv("NR_LAYER_STYLE_INDEX");
    if (!si_s) si_s = getenv("DLSS5_STYLE_INDEX");
    if (si_s && pfn_dlss5_set_style_index) {
        int val = atoi(si_s);
        if (val != g_cur_style_index) {
            g_cur_style_index = val;
            pfn_dlss5_set_style_index(g_engine, val);
            nr_log("[nr_layer] live style_index updated: %d", val);
        }
    }

    const char *cs_s = getenv("NR_LAYER_COLOR_STRENGTH");
    if (!cs_s) cs_s = getenv("DLSS5_COLOR_STRENGTH");
    if (cs_s && pfn_dlss5_set_color_strength) {
        float val = (float)atof(cs_s);
        float diff = val - g_cur_color_strength;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) {
            g_cur_color_strength = val;
            pfn_dlss5_set_color_strength(g_engine, val);
            nr_log("[nr_layer] live color_strength updated: %.2f", val);
        }
    }
}

static void ensure_inprocess(void)
{
    if (g_inprocess_attempted) return;
    g_inprocess_attempted = 1;

    const char *inproc_env = getenv("NR_LAYER_INPROCESS");
    if (inproc_env && !strcmp(inproc_env, "0")) {
        nr_log("in-process mode disabled via NR_LAYER_INPROCESS=0");
        return;
    }

    HMODULE hDll = NULL;
    const char *custom_dll = getenv("NR_LAYER_CHAIN_DLL");
    if (custom_dll && custom_dll[0]) {
        hDll = LoadLibraryA(custom_dll);
    }
    if (!hDll) {
        /* Check directory of this layer DLL */
        char layer_path[MAX_PATH] = {0};
        HMODULE hThis = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ensure_inprocess, &hThis) && hThis) {
            if (GetModuleFileNameA(hThis, layer_path, sizeof(layer_path))) {
                char *slash = strrchr(layer_path, '\\');
                if (!slash) slash = strrchr(layer_path, '/');
                if (slash) {
                    *(slash + 1) = '\0';
                    char candidate[MAX_PATH];
                    _snprintf(candidate, sizeof(candidate), "%sd5c_chain.dll", layer_path);
                    hDll = LoadLibraryA(candidate);
                }
            }
        }
    }
    if (!hDll) {
        /* Check build output directory relative to repo if running in tree */
        const char *candidates[] = {
            "dlss5\\chain\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11d\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11-layer\\build\\Release\\d5c_chain.dll",
            "..\\..\\chain\\build\\Release\\d5c_chain.dll",
            "..\\..\\m11d\\build\\Release\\d5c_chain.dll"
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (_access(candidates[i], 0) == 0) {
                hDll = LoadLibraryA(candidates[i]);
                if (hDll) break;
            }
        }
    }
    if (!hDll) {
        hDll = LoadLibraryA("d5c_chain.dll");
    }

    if (!hDll) {
        nr_log("d5c_chain.dll not found; falling back to TCP socket");
        return;
    }

    pfn_dlss5_create = (PFN_dlss5_create)GetProcAddress(hDll, "dlss5_create");
    pfn_dlss5_destroy = (PFN_dlss5_destroy)GetProcAddress(hDll, "dlss5_destroy");
    pfn_dlss5_process_frame = (PFN_dlss5_process_frame)GetProcAddress(hDll, "dlss5_process_frame");
    pfn_dlss5_set_scale = (PFN_dlss5_set_scale)GetProcAddress(hDll, "dlss5_set_scale");
    pfn_dlss5_set_gain = (PFN_dlss5_set_gain)GetProcAddress(hDll, "dlss5_set_gain");
    pfn_dlss5_set_blend = (PFN_dlss5_set_blend)GetProcAddress(hDll, "dlss5_set_blend");
    pfn_dlss5_set_sharpness = (PFN_dlss5_set_sharpness)GetProcAddress(hDll, "dlss5_set_sharpness");
    pfn_dlss5_interpolate_frame = (PFN_dlss5_interpolate_frame)GetProcAddress(hDll, "dlss5_interpolate_frame");
    pfn_dlss5_set_framegen = (PFN_dlss5_set_framegen)GetProcAddress(hDll, "dlss5_set_framegen");
    pfn_dlss5_get_framegen = (PFN_dlss5_get_framegen)GetProcAddress(hDll, "dlss5_get_framegen");
    pfn_dlss5_set_phase30 = (PFN_dlss5_set_phase30)GetProcAddress(hDll, "dlss5_set_phase30");
    pfn_dlss5_set_intensity = (PFN_dlss5_set_intensity)GetProcAddress(hDll, "dlss5_set_intensity");
    pfn_dlss5_set_local_tone = (PFN_dlss5_set_local_tone)GetProcAddress(hDll, "dlss5_set_local_tone");
    pfn_dlss5_set_local_structure = (PFN_dlss5_set_local_structure)GetProcAddress(hDll, "dlss5_set_local_structure");
    pfn_dlss5_set_skin_structure = (PFN_dlss5_set_skin_structure)GetProcAddress(hDll, "dlss5_set_skin_structure");
    pfn_dlss5_set_auto_mask = (PFN_dlss5_set_auto_mask)GetProcAddress(hDll, "dlss5_set_auto_mask");
    pfn_dlss5_set_style_index = (PFN_dlss5_set_style_index)GetProcAddress(hDll, "dlss5_set_style_index");
    pfn_dlss5_set_color_strength = (PFN_dlss5_set_color_strength)GetProcAddress(hDll, "dlss5_set_color_strength");

    if (!pfn_dlss5_create || !pfn_dlss5_destroy || !pfn_dlss5_process_frame) {
        nr_log("d5c_chain.dll missing exports; falling back to TCP");
        FreeLibrary(hDll);
        return;
    }

    g_chain_dll = hDll;

    const char *weights = getenv("NR_LAYER_WEIGHTS");
    if (!weights) weights = getenv("DLSS5_WEIGHTS_PATH");
    const char *shaders = getenv("NR_LAYER_SHADERS");
    if (!shaders) shaders = getenv("DLSS5_SHADER_DIR");
    const char *scale_s = getenv("NR_LAYER_SCALE");
    if (!scale_s) scale_s = getenv("DLSS5_SCALE");
    const char *gain_s = getenv("NR_LAYER_GAIN");
    if (!gain_s) gain_s = getenv("DLSS5_GAIN");
    const char *blend_s = getenv("NR_LAYER_BLEND");
    if (!blend_s) blend_s = getenv("DLSS5_BLEND");
    const char *sharp_s = getenv("NR_LAYER_SHARPNESS");
    if (!sharp_s) sharp_s = getenv("DLSS5_SHARPNESS");

    float scale = scale_s ? (float)atof(scale_s) : 1.0f;
    float gain = gain_s ? (float)atof(gain_s) : 1.0f;
    float blend = blend_s ? (float)atof(blend_s) : 1.0f;
    float sharpness = sharp_s ? (float)atof(sharp_s) : 0.8f;

    g_engine = pfn_dlss5_create(weights, shaders, scale, gain, blend);
    if (!g_engine) {
        nr_log("dlss5_create failed; falling back to TCP");
        FreeLibrary(g_chain_dll);
        g_chain_dll = NULL;
        return;
    }

    if (pfn_dlss5_set_sharpness) {
        pfn_dlss5_set_sharpness(g_engine, sharpness);
    }

    const char *fg_env = getenv("NR_LAYER_FRAMEGEN");
    if (!fg_env) fg_env = getenv("DLSS5_FRAMEGEN");
    if (fg_env && pfn_dlss5_set_framegen) {
        int enable = (fg_env[0] == '1' || !strcmp(fg_env, "true") || !strcmp(fg_env, "on")) ? 1 : 0;
        pfn_dlss5_set_framegen(g_engine, enable);
    }

    g_cur_scale = scale;
    g_cur_gain = gain;
    g_cur_blend = blend;
    g_cur_sharpness = sharpness;
    g_inprocess_active = 1;
    nr_log("in-process DLSS 5 active (TCP bypassed, scale=%.2f gain=%.2f blend=%.2f sharpness=%.2f)",
           scale, gain, blend, sharpness);
}

/* ------------------------------------------------------------------ */
/* TCP exchange with the daemon (Unix socket in the Linux original).    */
/* Protocol identical: 16-byte header, payload in, fixed reply out.     */
/* ------------------------------------------------------------------ */
static int winsock_up(void)
{
    static int attempted, ok;
    if (attempted) return ok;
    attempted = 1;
    WSADATA wd;
    ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    return ok;
}

static int exchange(const void *header, size_t header_size, const void *payload,
                    size_t payload_size, void *reply, size_t reply_size)
{
    if (!winsock_up()) return -1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    DWORD tv = 1500;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((u_short)nr_port);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        nr_log("[nr_layer] no daemon on 127.0.0.1:%ld", nr_port);
        closesocket(fd);
        return -1;
    }
    const unsigned char *out = (const unsigned char *)header;
    for (size_t sent = 0; sent < header_size; ) {
        int n = send(fd, (const char *)out + sent, (int)(header_size - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (size_t)n;
    }
    out = (const unsigned char *)payload;
    for (size_t sent = 0; sent < payload_size; ) {
        int n = send(fd, (const char *)out + sent, (int)(payload_size - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (size_t)n;
    }
    unsigned char *in = (unsigned char *)reply;
    for (size_t got = 0; got < reply_size; ) {
        int n = recv(fd, (char *)in + got, (int)(reply_size - got), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        got += (size_t)n;
    }
    closesocket(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
static device_data *find_device(VkDevice device)
{
    for (int i = 0; i < 8; i++)
        if (devices[i].device == device) return &devices[i];
    return NULL;
}

static queue_data *find_queue(VkQueue queue)
{
    for (int i = 0; i < MAX_QUEUES; i++)
        if (queues[i].queue == queue) return &queues[i];
    return NULL;
}

/* Whether a copy can be recorded for this family at all (present-only and
 * protected queues cannot). Same guard as the Linux original. */
static int family_can_capture(device_data *data, uint32_t family)
{
    if (!data || !next_instance_proc || !layer_instance) return 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        next_instance_proc(layer_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!properties) return 0;
    uint32_t count = 0;
    properties(data->physical, &count, NULL);
    if (!count || family >= count) return 0;
    VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)calloc(count, sizeof *families);
    if (!families) return 0;
    properties(data->physical, &count, families);
    int ok = (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
                                             | VK_QUEUE_TRANSFER_BIT)) != 0;
    free(families);
    return ok;
}

static void lock(void) { if (g_lock_ready) EnterCriticalSection(&g_lock); }
static void unlock(void) { if (g_lock_ready) LeaveCriticalSection(&g_lock); }

/* CTRL+ALT+X pauses processing (native frames, full fps), CTRL+ALT+Q turns
 * the layer off for good. The thread has its own message loop, so the
 * hotkeys work even inside a game that never pumps messages for us. */
static DWORD WINAPI hotkey_thread(LPVOID param)
{
    (void)param;
    if (!RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X')) {
        nr_log("[nr_layer] pause hotkey unavailable (%lu)", GetLastError());
    }
    if (!RegisterHotKey(NULL, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q')) {
        nr_log("[nr_layer] off hotkey unavailable (%lu)", GetLastError());
    }
    nr_log("[nr_layer] hotkeys armed: CTRL+ALT+X pause/resume, CTRL+ALT+Q layer off");
    MSG msg;
    for (;;) {
        LONG r = GetMessage(&msg, NULL, 0, 0);
        if (r <= 0) break;
        if (msg.message != WM_HOTKEY) continue;
        if (msg.wParam == 1) {
            InterlockedExchange(&g_paused, g_paused ? 0 : 1);
            nr_log("[nr_layer] %s by hotkey", g_paused ? "paused" : "resumed");
        } else if (msg.wParam == 2) {
            InterlockedExchange(&g_off, 1);
            nr_log("[nr_layer] layer off by hotkey");
        }
    }
    return 0;
}

static void hotkeys_ensure(void)
{
    if (InterlockedCompareExchange(&g_hotkeys_started, 1, 0)) return;
    HANDLE t = CreateThread(NULL, 0, hotkey_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else {
        g_hotkeys_started = 0;
        nr_log("[nr_layer] hotkey thread failed (%lu)", GetLastError());
    }
}

static void remember_queue(VkDevice device, uint32_t family, VkQueue queue, int capture_ok)
{
    if (!queue) return;
    lock();
    if (!find_queue(queue))
        for (int i = 0; i < MAX_QUEUES; i++)
            if (!queues[i].queue) {
                queues[i].queue = queue;
                queues[i].device = device;
                queues[i].family = family;
                queues[i].capture_ok = capture_ok;
                break;
            }
    unlock();
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue(VkDevice device, uint32_t family,
                                             uint32_t index, VkQueue *queue)
{
    device_data *data = find_device(device);
    if (!data || !data->get_device_queue) return;
    data->get_device_queue(device, family, index, queue);
    remember_queue(device, family, *queue, family_can_capture(data, family));
}

VKAPI_ATTR void VKAPI_CALL nr_GetDeviceQueue2(VkDevice device,
                                              const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    device_data *data = find_device(device);
    if (!data || !data->get_device_queue2) return;
    data->get_device_queue2(device, info, queue);
    remember_queue(device, info->queueFamilyIndex, *queue,
                   (info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)
                   ? 0 : family_can_capture(data, info->queueFamilyIndex));
}

/* Four bytes a pixel only, exactly like the original. */
static int format_is_four_bytes(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return 1;
    default:
        return 0;
    }
}

static swapchain_data *find_swapchain(VkSwapchainKHR swapchain)
{
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (swapchains[i].swapchain == swapchain) return &swapchains[i];
    return NULL;
}

static VkLayerInstanceCreateInfo *instance_chain(const VkInstanceCreateInfo *info)
{
    VkLayerInstanceCreateInfo *item = (VkLayerInstanceCreateInfo *)info->pNext;
    while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                     && item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerInstanceCreateInfo *)item->pNext;
    return item;
}

static VkLayerDeviceCreateInfo *device_chain(const VkDeviceCreateInfo *info)
{
    VkLayerDeviceCreateInfo *item = (VkLayerDeviceCreateInfo *)info->pNext;
    while (item && !(item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                     && item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerDeviceCreateInfo *)item->pNext;
    return item;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateInstance(const VkInstanceCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkInstance *instance)
{
    VkLayerInstanceCreateInfo *link = instance_chain(info);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    next_instance_proc = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)next_instance_proc(NULL, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    if (info && info->pApplicationInfo && info->pApplicationInfo->pApplicationName) {
        const char *app = info->pApplicationInfo->pApplicationName;
        if (!strcmp(app, "dlss5_inprocess") || !strcmp(app, "m11d") || !strcmp(app, "d5c")) {
            nr_log("[nr_layer] bypass interception for internal process: %s", app);
            return create(info, allocator, instance);
        }
    }

    VkResult r = create(info, allocator, instance);
    if (r == VK_SUCCESS) {
        layer_instance = *instance;
        capture_path = getenv("NR_LAYER_CAPTURE");
        capture_out_path = getenv("NR_LAYER_CAPTURE_OUT");
        trigger_path = getenv("NR_LAYER_TRIGGER");
        freeze_path = getenv("NR_LAYER_FREEZE");
        reproc_path = getenv("NR_LAYER_REPROC");
        init_freeze_paths();
        const char *mask = getenv("NR_LAYER_UI_MASK");
        static_reuse = (!mask || strcmp(mask, "0") != 0);
        const char *every = getenv("NR_LAYER_EVERY");
        capture_every = every ? strtol(every, NULL, 10) : 0;
        const char *sync = getenv("NR_LAYER_SYNC");
        sync_semaphores = sync && !strcmp(sync, "semaphore");
        const char *live = getenv("NR_LAYER_LIVE");
        live_every = live ? strtol(live, NULL, 10) : 0;
        if (live_every < 0) live_every = 0;
        const char *port = getenv("NR_LAYER_PORT");
        nr_port = port ? strtol(port, NULL, 10) : NR_DEFAULT_PORT;
        if (nr_port <= 0) nr_port = NR_DEFAULT_PORT;
        const char *nopatch = getenv("NR_LAYER_NOPATCH");
        no_patch = nopatch && strcmp(nopatch, "0") != 0;
        if (no_patch)
            nr_log("[nr_layer] NOPATCH: swapchains left completely alone");
        hotkeys_ensure();
        if (live_every > 0)
            nr_log("[nr_layer] live: every %ld present processed", live_every);
        if (sync_semaphores)
            nr_log("[nr_layer] sync: the present's own semaphores, not a queue idle");
        nr_log("[nr_layer] active; port=%ld trigger=%s capture=%s every=%ld",
             nr_port, trigger_path ? trigger_path : "(none)",
             capture_path ? capture_path : "(off)", capture_every);
        if (static_reuse)
            nr_log("[nr_layer] static reuse on: exact-repeat frames reuse "
                 "the last processed result");
        ensure_inprocess();
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL nr_DestroyInstance(VkInstance instance,
                                             const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyInstance destroy = NULL;
    if (next_instance_proc)
        destroy = (PFN_vkDestroyInstance)next_instance_proc(instance, "vkDestroyInstance");
    if (instance == layer_instance) {
        layer_instance = VK_NULL_HANDLE;
    }
    if (destroy) destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL nr_CreateDevice(VkPhysicalDevice physical,
                                               const VkDeviceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkDevice *device)
{
    VkLayerDeviceCreateInfo *link = device_chain(info);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_device = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create = NULL;
    if (next_instance && layer_instance)
        create = (PFN_vkCreateDevice)next_instance(layer_instance, "vkCreateDevice");
    if (!create && next_instance)
        create = (PFN_vkCreateDevice)next_instance(NULL, "vkCreateDevice");
    if (!create && next_instance_proc && layer_instance)
        create = (PFN_vkCreateDevice)next_instance_proc(layer_instance, "vkCreateDevice");
    if (!create && next_instance_proc)
        create = (PFN_vkCreateDevice)next_instance_proc(NULL, "vkCreateDevice");
    if (!create) {
        nr_log("[nr_layer] FATAL: vkCreateDevice proc address not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult r = create(physical, info, allocator, device);
    if (r != VK_SUCCESS || !device || !*device) return r;

    lock();
    device_data *data = NULL;
    for (int i = 0; i < 8; i++) if (!devices[i].device) { data = &devices[i]; break; }
    if (data) {
        memset(data, 0, sizeof *data);
        data->device = *device;
        data->physical = physical;
        data->get_device_proc = next_device;
        data->present = (PFN_vkQueuePresentKHR)next_device(*device, "vkQueuePresentKHR");
        data->get_device_queue =
            (PFN_vkGetDeviceQueue)next_device(*device, "vkGetDeviceQueue");
        data->get_device_queue2 =
            (PFN_vkGetDeviceQueue2)next_device(*device, "vkGetDeviceQueue2");
        data->destroy_swapchain =
            (PFN_vkDestroySwapchainKHR)next_device(*device, "vkDestroySwapchainKHR");
        data->create_swapchain =
            (PFN_vkCreateSwapchainKHR)next_device(*device, "vkCreateSwapchainKHR");
        data->get_swapchain_images =
            (PFN_vkGetSwapchainImagesKHR)next_device(*device, "vkGetSwapchainImagesKHR");
        data->destroy_device = (PFN_vkDestroyDevice)next_device(*device, "vkDestroyDevice");
        data->queue_family = info->queueCreateInfoCount
            ? info->pQueueCreateInfos[0].queueFamilyIndex : 0;
    }
    unlock();
    return r;
}

/* Force TRANSFER_SRC|DST onto the swapchain images; without it the copy is
 * invalid and vkCreateSwapchainKHR is the only place it can be added. */
VKAPI_ATTR VkResult VKAPI_CALL nr_CreateSwapchainKHR(VkDevice device,
                                                     const VkSwapchainCreateInfoKHR *info,
                                                     const VkAllocationCallbacks *allocator,
                                                     VkSwapchainKHR *swapchain)
{
    device_data *data = find_device(device);
    if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    if (no_patch)
        return data->create_swapchain(device, info, allocator, swapchain);
    const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainCreateInfoKHR patched = *info;
    patched.imageUsage |= copies;
    int copyable = 1;
    VkResult r = data->create_swapchain(device, &patched, allocator, swapchain);
    if (r != VK_SUCCESS) {
        /* Surface refused transfer usage: create it as asked and do not track,
         * rather than issue an illegal copy every frame. */
        copyable = (info->imageUsage & copies) == copies;
        nr_log("[nr_layer] patched usage refused (0x%x); creating as asked, copyable=%d",
             (unsigned)r, copyable);
        r = data->create_swapchain(device, info, allocator, swapchain);
    }
    if (r != VK_SUCCESS) return r;
    if (!copyable) {
        nr_log("[nr_layer] swapchain refused transfer usage; capture off");
        return r;
    }
    if (info->imageArrayLayers != 1
        || (info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) {
        nr_log("[nr_layer] swapchain is multi-layer or protected; capture off");
        return r;
    }
    if (!format_is_four_bytes(info->imageFormat)) {
        nr_log("[nr_layer] swapchain format %d is not four bytes a pixel; "
             "leaving it alone", info->imageFormat);
        return r;
    }
    lock();
    swapchain_data *entry = NULL;
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (!swapchains[i].swapchain) { entry = &swapchains[i]; break; }
    if (!entry)
        nr_log("[nr_layer] all %d swapchain slots are in use; not tracked", MAX_SWAPCHAINS);
    if (entry) {
        memset(entry, 0, sizeof *entry);
        entry->swapchain = *swapchain;
        entry->device = device;
        entry->format = info->imageFormat;
        entry->extent = info->imageExtent;
        entry->image_count = MAX_IMAGES;
        VkResult got = data->get_swapchain_images(device, *swapchain,
                                                  &entry->image_count, entry->images);
        if (got != VK_SUCCESS || !entry->image_count) {
            memset(entry, 0, sizeof *entry);
            unlock();
            nr_log("[nr_layer] swapchain has more than %d images or none; capture off",
                 MAX_IMAGES);
            return r;
        }
        nr_log("[nr_layer] swapchain %ux%u format %d, %u images",
             entry->extent.width, entry->extent.height, entry->format,
             entry->image_count);
    }
    unlock();
    return r;
}

static void release_device(device_data *data)
{
    if (data->mapped) {
        PFN_vkUnmapMemory unmap =
            (PFN_vkUnmapMemory)data->get_device_proc(data->device, "vkUnmapMemory");
        if (unmap) unmap(data->device, data->staging_memory);
    }
    if (data->staging) {
        PFN_vkDestroyBuffer destroy =
            (PFN_vkDestroyBuffer)data->get_device_proc(data->device, "vkDestroyBuffer");
        if (destroy) destroy(data->device, data->staging, NULL);
    }
    if (data->staging_memory) {
        PFN_vkFreeMemory release =
            (PFN_vkFreeMemory)data->get_device_proc(data->device, "vkFreeMemory");
        if (release) release(data->device, data->staging_memory, NULL);
    }
    if (data->pool) {
        PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)
            data->get_device_proc(data->device, "vkDestroyCommandPool");
        if (destroy) destroy(data->device, data->pool, NULL);
    }
    free(data->result);
    free(data->earlier);
    free(data->raw);
    free(data->interp);
    free(data->prev_processed);
    data->interp = NULL;
    data->interp_size = 0;
    data->prev_processed = NULL;
    data->prev_processed_size = 0;
    data->have_prev_processed = 0;
}

VKAPI_ATTR void VKAPI_CALL nr_DestroyDevice(VkDevice device,
                                            const VkAllocationCallbacks *allocator)
{
    device_data *data = find_device(device);
    PFN_vkDestroyDevice next = data ? data->destroy_device : NULL;
    if (data) {
        lock();
        for (int i = 0; i < MAX_SWAPCHAINS; i++)
            if (swapchains[i].device == device)
                memset(&swapchains[i], 0, sizeof swapchains[i]);
        for (int i = 0; i < MAX_QUEUES; i++)
            if (queues[i].device == device)
                memset(&queues[i], 0, sizeof queues[i]);
        unlock();
        release_device(data);
        memset(data, 0, sizeof *data);
    }
    if (next) next(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL nr_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks *allocator)
{
    device_data *data = find_device(device);
    lock();
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (swapchains[i].swapchain == swapchain)
            memset(&swapchains[i], 0, sizeof swapchains[i]);
    /* A held result belongs to one swapchain; never re-blit it into another
     * (device resets recreate swapchains, sometimes at the same size). */
    for (int i = 0; i < 8; i++)
        if (devices[i].holding && devices[i].holding_chain == swapchain) {
            devices[i].holding = 0;
            devices[i].holding_chain = VK_NULL_HANDLE;
            devices[i].have_prev_processed = 0;
        }
    unlock();
    if (data && data->destroy_swapchain)
        data->destroy_swapchain(device, swapchain, allocator);
}

static uint32_t memory_type(device_data *data, uint32_t bits,
                            VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties properties;
    PFN_vkGetPhysicalDeviceMemoryProperties get =
        (PFN_vkGetPhysicalDeviceMemoryProperties)next_instance_proc(
            layer_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!get) return UINT32_MAX;
    get(data->physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (properties.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int ensure_resources(device_data *data, VkDeviceSize needed)
{
    if (!data->pool) {
        VkCommandPoolCreateInfo info;
        memset(&info, 0, sizeof info);
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = data->queue_family;
        PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)
            data->get_device_proc(data->device, "vkCreateCommandPool");
        if (create(data->device, &info, NULL, &data->pool) != VK_SUCCESS) return -1;
    }
    if (data->staging_size >= needed) return 0;

    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)
        data->get_device_proc(data->device, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements requirements = (PFN_vkGetBufferMemoryRequirements)
        data->get_device_proc(data->device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)
        data->get_device_proc(data->device, "vkAllocateMemory");
    PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)
        data->get_device_proc(data->device, "vkBindBufferMemory");
    PFN_vkMapMemory map = (PFN_vkMapMemory)
        data->get_device_proc(data->device, "vkMapMemory");
    VkBufferCreateInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = needed;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (create_buffer(data->device, &info, NULL, &data->staging) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    requirements(data->device, data->staging, &mr);
    uint32_t type = memory_type(data, mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = memory_type(data, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return -1;
    VkMemoryAllocateInfo allocation;
    memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = mr.size;
    allocation.memoryTypeIndex = type;
    if (allocate(data->device, &allocation, NULL, &data->staging_memory) != VK_SUCCESS)
        return -1;
    bind(data->device, data->staging, data->staging_memory, 0);
    map(data->device, data->staging_memory, 0, VK_WHOLE_SIZE, 0, &data->mapped);
    data->staging_size = needed;
    free(data->result);
    data->result = (unsigned char *)malloc((size_t)needed);
    data->result_size = needed;
    return data->result ? 0 : -1;
}

static int ensure_ring(device_data *data)
{
    if (data->ring_ready) return 0;
    if (!data->pool) return -1;
    PFN_vkAllocateCommandBuffers allocate_commands = (PFN_vkAllocateCommandBuffers)
        data->get_device_proc(data->device, "vkAllocateCommandBuffers");
    PFN_vkCreateFence create_fence = (PFN_vkCreateFence)
        data->get_device_proc(data->device, "vkCreateFence");
    PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)
        data->get_device_proc(data->device, "vkCreateSemaphore");
    if (!allocate_commands || !create_fence || !create_semaphore) return -1;
    VkCommandBufferAllocateInfo alloc;
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = data->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = SYNC_SLOTS;
    if (allocate_commands(data->device, &alloc, data->ring_commands) != VK_SUCCESS)
        return -1;
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < SYNC_SLOTS; i++) {
        if (create_fence(data->device, &fi, NULL, &data->ring_fence[i]) != VK_SUCCESS
            || create_semaphore(data->device, &si, NULL, &data->ring_done[i]) != VK_SUCCESS)
            return -1;
        data->ring_used[i] = 0;
    }
    data->ring_ready = 1;
    return 0;
}

/* Move the presented frame between the swapchain image and the staging
 * buffer. idle sync = vkQueueWaitIdle around the transfer (default, proven);
 * semaphore sync = take over the present's wait semaphores (NR_LAYER_SYNC). */
static int transfer(device_data *data, swapchain_data *chain, VkQueue queue,
                    uint32_t index, int to_image)
{
    PFN_vkAllocateCommandBuffers allocate_commands = (PFN_vkAllocateCommandBuffers)
        data->get_device_proc(data->device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)
        data->get_device_proc(data->device, "vkBeginCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)
        data->get_device_proc(data->device, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer copy_out = (PFN_vkCmdCopyImageToBuffer)
        data->get_device_proc(data->device, "vkCmdCopyImageToBuffer");
    PFN_vkCmdCopyBufferToImage copy_in = (PFN_vkCmdCopyBufferToImage)
        data->get_device_proc(data->device, "vkCmdCopyBufferToImage");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)
        data->get_device_proc(data->device, "vkEndCommandBuffer");
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)
        data->get_device_proc(data->device, "vkQueueSubmit");
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)
        data->get_device_proc(data->device, "vkQueueWaitIdle");
    PFN_vkFreeCommandBuffers free_commands = (PFN_vkFreeCommandBuffers)
        data->get_device_proc(data->device, "vkFreeCommandBuffers");

    int ringed = sync_semaphores && !data->present_plain && ensure_ring(data) == 0;
    unsigned slot = data->ring_next % SYNC_SLOTS;
    VkCommandBuffer commands;
    if (ringed) {
        if (data->ring_used[slot]) {
            PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)
                data->get_device_proc(data->device, "vkWaitForFences");
            PFN_vkResetFences reset_fences = (PFN_vkResetFences)
                data->get_device_proc(data->device, "vkResetFences");
            wait_fences(data->device, 1, &data->ring_fence[slot], VK_TRUE,
                        10ull * 1000000000ull);
            reset_fences(data->device, 1, &data->ring_fence[slot]);
        }
        commands = data->ring_commands[slot];
        data->ring_next++;
    } else {
        VkCommandBufferAllocateInfo alloc;
        memset(&alloc, 0, sizeof alloc);
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = data->pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (allocate_commands(data->device, &alloc, &commands) != VK_SUCCESS) return -1;
    }
    VkCommandBufferBeginInfo beginning;
    memset(&beginning, 0, sizeof beginning);
    beginning.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginning.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin(commands, &beginning);

    VkImageLayout working = to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier into;
    memset(&into, 0, sizeof into);
    into.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    into.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    into.dstAccessMask = to_image ? VK_ACCESS_TRANSFER_WRITE_BIT
                                  : VK_ACCESS_TRANSFER_READ_BIT;
    into.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    into.newLayout = working;
    into.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    into.image = chain->images[index];
    into.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    into.subresourceRange.baseMipLevel = 0;
    into.subresourceRange.levelCount = 1;
    into.subresourceRange.baseArrayLayer = 0;
    into.subresourceRange.layerCount = 1;
    barrier(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &into);

    VkBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = chain->extent.width;
    region.imageExtent.height = chain->extent.height;
    region.imageExtent.depth = 1;
    if (to_image)
        copy_in(commands, data->staging, chain->images[index], working, 1, &region);
    else
        copy_out(commands, chain->images[index], working, data->staging, 1, &region);

    VkImageMemoryBarrier back = into;
    back.srcAccessMask = into.dstAccessMask;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back.oldLayout = working;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    end(commands);

    if (!ringed) {
        wait(queue);
        VkSubmitInfo submission;
        memset(&submission, 0, sizeof submission);
        submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &commands;
        submit(queue, 1, &submission, VK_NULL_HANDLE);
        wait(queue);
        free_commands(data->device, data->pool, 1, &commands);
        return 0;
    }

    /* Semaphore mode: wait on the present's own waits; only the write-back
     * signals (a binary semaphore may not be signalled twice; the readback is
     * fenced by the host instead). */
    VkSemaphore signal = to_image ? data->ring_done[slot] : VK_NULL_HANDLE;
    VkPipelineStageFlags stages[NR_MAX_WAITS];
    uint32_t waits = data->present_wait_count;
    for (uint32_t i = 0; i < waits; i++) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submission;
    memset(&submission, 0, sizeof submission);
    submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submission.waitSemaphoreCount = waits;
    submission.pWaitSemaphores = waits ? data->present_wait : NULL;
    submission.pWaitDstStageMask = waits ? stages : NULL;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    submission.signalSemaphoreCount = signal ? 1 : 0;
    submission.pSignalSemaphores = signal ? &signal : NULL;
    if (submit(queue, 1, &submission, data->ring_fence[slot]) != VK_SUCCESS) {
        data->ring_used[slot] = 0;
        return -1;
    }
    data->ring_used[slot] = 1;
    data->present_wait_count = 0;
    if (signal) data->present_signal = signal;
    if (!to_image) {
        PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)
            data->get_device_proc(data->device, "vkWaitForFences");
        if (wait_fences(data->device, 1, &data->ring_fence[slot], VK_TRUE,
                        10ull * 1000000000ull) != VK_SUCCESS)
            return -1;
    }
    return 0;
}

/* NR_LAYER_CAPTURE_OUT: dump header+processed result for A/B stills. */
static void dump_capture(const char *path, const uint32_t *header,
                         const unsigned char *pixels, VkDeviceSize size)
{
    if (!path) return;
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(header, sizeof(uint32_t), 4, file);
    fwrite(pixels, 1, (size_t)size, file);
    fclose(file);
}

static int process_frame(device_data *data, swapchain_data *chain,
                         VkQueue queue, uint32_t index)
{
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
    if (ensure_resources(data, needed)) return -1;
    if (transfer(data, chain, queue, index, 0)) return -1;

    /* Exact-repeat reuse: a frame identical to the previous one reuses the
     * last processed result instead of running the chain again. */
    if (static_reuse && data->have_earlier && data->earlier_size >= needed &&
        memcmp(data->mapped, data->earlier, (size_t)needed) == 0 &&
        data->have_prev_processed && data->prev_processed &&
        data->prev_processed_size >= needed && data->result &&
        data->result_size >= needed) {
        memcpy(data->mapped, data->prev_processed, (size_t)needed);
        memcpy(data->result, data->prev_processed, (size_t)needed);
        return 0;
    }

    /* Save native input into data->earlier before processing */
    if (data->earlier_size < needed) {
        unsigned char *grown = (unsigned char *)realloc(data->earlier, (size_t)needed);
        if (!grown) return -1;
        data->earlier = grown;
        data->earlier_size = needed;
    }
    memcpy(data->earlier, data->mapped, (size_t)needed);
    data->have_earlier = 1;
    data->earlier_size = needed;

    uint32_t header[4] = { 0x304E524Eu, chain->extent.width,
                           chain->extent.height, (uint32_t)chain->format };
    if (capture_path) {
        FILE *file = fopen(capture_path, "wb");
        if (file) {
            fwrite(header, sizeof header, 1, file);
            fwrite(data->mapped, 1, (size_t)needed, file);
            fclose(file);
        }
    }

    /* In-process DLSS 5 engine execution: bypass TCP socket entirely */
    ensure_inprocess();
    if (g_inprocess_active && pfn_dlss5_process_frame && g_engine) {
        int r = pfn_dlss5_process_frame(g_engine, chain->extent.width, chain->extent.height,
                                        data->mapped, data->mapped, (long)frame_counter);
        if (r == 0) {
            if (data->result && data->result_size >= needed) {
                memcpy(data->result, data->mapped, (size_t)needed);
            }
            dump_capture(capture_out_path, header, (const unsigned char *)data->mapped, needed);
            nr_log("[nr_layer] in-process processed %ux%u (frame %lu)",
                   chain->extent.width, chain->extent.height, frame_counter);
            return 0;
        }
        nr_log("[nr_layer] in-process process_frame returned %d; trying TCP fallback", r);
    }

    /* No daemon configured: capture-only mode (frame passes through). */
    if (!nr_port) return -1;
    if (exchange(header, sizeof header, data->mapped, (size_t)needed, data->result,
                 (size_t)needed)) {
        nr_log("[nr_layer] the daemon did not answer; frame unchanged");
        return -1;
    }
    memcpy(data->mapped, data->result, (size_t)needed);
    dump_capture(capture_out_path, header, data->result, needed);
    nr_log("[nr_layer] processed %ux%u", chain->extent.width, chain->extent.height);
    return 0;
}

/* The present, with its waits replaced by ours if a transfer consumed them. */
static VkResult present_now(device_data *data, VkQueue queue,
                            const VkPresentInfoKHR *info)
{
    if (!data || !data->present) return VK_ERROR_INITIALIZATION_FAILED;
    if (!sync_semaphores || !data->present_signal)
        return data->present(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &data->present_signal;
    return data->present(queue, &patched);
}

/* ------------------------------------------------ freeze + live reprocess --
 * m13 overlay "photo mode": while NR_LAYER_FREEZE exists the layer holds the
 * RAW frame captured at freeze time and re-blits its processed result every
 * present (the game keeps running underneath; the picture stands still).
 * When the manager pushes a new gain to the daemon it bumps NR_LAYER_REPROC's
 * mtime; the next present re-sends the SAME raw frame, so the knob turns are
 * visible on the frozen frame, processed from the untouched original. */
static int reproc_changed(device_data *data)
{
    init_freeze_paths();
    if (!reproc_path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(reproc_path, GetFileExInfoStandard, &fa)) {
        data->reproc_seen = 0;
        return 0;
    }
    if (!data->reproc_seen
        || fa.ftLastWriteTime.dwLowDateTime != data->reproc_stamp.dwLowDateTime
        || fa.ftLastWriteTime.dwHighDateTime != data->reproc_stamp.dwHighDateTime) {
        data->reproc_stamp = fa.ftLastWriteTime;
        data->reproc_seen = 1;
        return 1;
    }
    return 0;
}

/* Send the held raw frame to the daemon; reply lands in data->result. */
static int resend_raw(device_data *data, swapchain_data *chain)
{
    ensure_inprocess();
    update_live_knobs();
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
    if (!data->result || data->result_size < needed) {
        unsigned char *grown = (unsigned char *)realloc(data->result, (size_t)needed);
        if (!grown) return -1;
        data->result = grown;
        data->result_size = needed;
    }
    if (g_inprocess_active && pfn_dlss5_process_frame && g_engine) {
        int r = pfn_dlss5_process_frame(g_engine, chain->extent.width, chain->extent.height,
                                        data->raw, data->result, (long)frame_counter);
        if (r == 0) {
            nr_log("[nr_layer] in-process reprocessed held raw frame (live knob turn)");
            return 0;
        }
    }

    uint32_t header[4] = { 0x304E524Eu, chain->extent.width,
                           chain->extent.height, (uint32_t)chain->format };
    if (exchange(header, sizeof header, data->raw, (size_t)data->raw_size,
                 data->result, (size_t)data->raw_size)) {
        nr_log("[nr_layer] reprocess: the daemon did not answer");
        return -1;
    }
    nr_log("[nr_layer] reprocessed the held frame (live knob turn)");
    return 0;
}

/* Hold the frame being presented right now as the frozen raw frame, process
 * it once (so the frozen picture starts processed at the current gain). */
static int freeze_capture(device_data *data, swapchain_data *chain,
                          VkQueue queue, uint32_t index)
{
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width
                        * chain->extent.height * 4;
    if (ensure_resources(data, needed)) return -1;
    if (transfer(data, chain, queue, index, 0)) return -1;
    if (data->raw_size < needed) {
        unsigned char *grown = (unsigned char *)realloc(data->raw,
                                                        (size_t)needed);
        if (!grown) return -1;
        data->raw = grown;
        data->raw_size = needed;
    }
    memcpy(data->raw, data->mapped, (size_t)needed);
    data->raw_size = needed;
    data->raw_held = 1;
    if (resend_raw(data, chain)) {
        memcpy(data->result, data->raw, (size_t)needed);
        nr_log("[nr_layer] daemon unavailable; freezing the original frame unprocessed");
    }
    memcpy(data->mapped, data->result, (size_t)needed);
    nr_log("[nr_layer] frame frozen (%ux%u); knobs reprocess the original",
           chain->extent.width, chain->extent.height);
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL nr_QueuePresentKHR(VkQueue queue,
                                                  const VkPresentInfoKHR *info)
{
    frame_counter++;
    device_data *data = NULL;
    queue_data *owner = find_queue(queue);
    if (owner && !owner->capture_ok) {
        device_data *host = find_device(owner->device);
        if (host && host->present) return host->present(queue, info);
    }
    if (owner) {
        data = find_device(owner->device);
        if (data && data->queue_family != owner->family) {
            data->queue_family = owner->family;
            if (data->pool) {
                PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)
                    data->get_device_proc(data->device, "vkDestroyCommandPool");
                if (destroy) destroy(data->device, data->pool, NULL);
                data->pool = VK_NULL_HANDLE;
            }
        }
    }
    if (!data)
        for (int i = 0; i < 8; i++) if (devices[i].device) { data = &devices[i]; break; }
    if (!data || !data->present) return VK_ERROR_INITIALIZATION_FAILED;

    data->present_plain = info->waitSemaphoreCount > NR_MAX_WAITS;
    data->present_wait = info->pWaitSemaphores;
    data->present_wait_count = (sync_semaphores && !data->present_plain)
        ? info->waitSemaphoreCount : 0;
    data->present_signal = VK_NULL_HANDLE;

    /* Hotkeys / status checks:
     * - g_off: layer disabled via CTRL+ALT+Q -> present native untouched.
     * - g_paused: toggled by hotkey CTRL+ALT+X -> present native untouched.
     * - no_patch: swapchains left completely alone -> present native untouched.
     */
    if (g_off)
        return data->present(queue, info);
    if (g_paused)
        return data->present(queue, info);
    if (no_patch)
        return data->present(queue, info);

    hotkeys_ensure();
    ensure_inprocess();
    update_live_knobs();
    init_freeze_paths();

    int frozen = freeze_path && _access(freeze_path, 0) == 0;
    if (!frozen) {
        data->raw_held = 0;
        data->reproc_seen = 0;
    }

    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
        if (!chain || info->pImageIndices[i] >= chain->image_count) continue;
        uint32_t index = info->pImageIndices[i];
        VkDeviceSize want = (VkDeviceSize)chain->extent.width
                          * chain->extent.height * 4;

        if (frozen) {
            /* Photo mode: hold the frozen frame; reprocess on demand. */
            if (!data->raw_held || data->raw_size != want) {
                if (freeze_capture(data, chain, queue, index) == 0)
                    transfer(data, chain, queue, index, 1);
            } else {
                if (reproc_changed(data)) {
                    update_live_knobs();
                    if (resend_raw(data, chain) != 0)
                        nr_log("[nr_layer] reprocess failed; keeping the "
                               "previous result");
                }
                if (data->result_size == want) {
                    memcpy(data->mapped, data->result,
                           (size_t)data->result_size);
                    transfer(data, chain, queue, index, 1);
                }
            }
            continue;
        }

        /* Live mode processing:
         * When In-Process DLSS 5 is active (g_inprocess_active) or DLSS 5 is enabled,
         * EVERY presented frame is processed by default.
         * If live_every > 1, process every Nth frame, and between frames present
         * fresh native frames cleanly without overwriting with stale results.
         */
        int should_process = (live_every <= 1) || (frame_counter % (unsigned long)live_every == 0);
        if (should_process) {
            if (process_frame(data, chain, queue, index) == 0) {
                data->holding = 1;
                data->holding_chain = chain->swapchain;
                if (data->prev_processed_size < want) {
                    unsigned char *grown = (unsigned char *)realloc(data->prev_processed, (size_t)want);
                    if (grown) {
                        data->prev_processed = grown;
                        data->prev_processed_size = want;
                    }
                }
                if (data->prev_processed && data->prev_processed_size >= want && data->result) {
                    memcpy(data->prev_processed, data->result, (size_t)want);
                    data->have_prev_processed = 1;
                }
                transfer(data, chain, queue, index, 1);
            }
        } else {
            /* Adaptive Framegen: interpolate between prev_processed and current native frame */
            if (is_framegen_enabled() && data->have_prev_processed && data->prev_processed &&
                data->prev_processed_size == want && g_engine && pfn_dlss5_interpolate_frame) {
                if (ensure_resources(data, want) == 0 && transfer(data, chain, queue, index, 0) == 0) {
                    if (data->raw_size < want) {
                        unsigned char *grown = (unsigned char *)realloc(data->raw, (size_t)want);
                        if (grown) {
                            data->raw = grown;
                            data->raw_size = want;
                        }
                    }
                    if (data->interp_size < want) {
                        unsigned char *grown = (unsigned char *)realloc(data->interp, (size_t)want);
                        if (grown) {
                            data->interp = grown;
                            data->interp_size = want;
                        }
                    }
                    if (data->raw && data->raw_size >= want && data->interp && data->interp_size >= want) {
                        memcpy(data->raw, data->mapped, (size_t)want);
                        data->raw_size = want;
                        int r = pfn_dlss5_interpolate_frame(g_engine, chain->extent.width, chain->extent.height,
                                                            data->prev_processed, data->raw,
                                                            data->interp, 0.5f);
                        if (r == 0) {
                            static unsigned long fg_frame_count = 0;
                            fg_frame_count++;
                            if (fg_frame_count <= 5 || fg_frame_count % 60 == 0) {
                                nr_log("[nr_layer] framegen interpolated %ux%u (frame %lu, fg #%lu)",
                                       chain->extent.width, chain->extent.height, frame_counter, fg_frame_count);
                            }
                            memcpy(data->mapped, data->interp, (size_t)want);
                            transfer(data, chain, queue, index, 1);
                        } else {
                            nr_log("[nr_layer] dlss5_interpolate_frame failed (%d); presenting native frame", r);
                        }
                    }
                }
            }
        }
    }
    return present_now(data, queue, info);
}

#define INTERCEPT(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)nr_##name

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetDeviceProcAddr_(VkDevice device,
                                                               const char *pName)
{
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroyDevice);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    device_data *data = find_device(device);
    return data ? data->get_device_proc(device, pName) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_GetInstanceProcAddr_(VkInstance instance,
                                                                 const char *pName)
{
    INTERCEPT(CreateInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(DestroyInstance);
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)nr_GetInstanceProcAddr_;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)nr_GetDeviceProcAddr_;
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroyDevice);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    return next_instance_proc ? next_instance_proc(instance, pName) : NULL;
}

/* Exported under the canonical Vulkan names so both the manifest "functions"
 * mapping and the loader's direct lookup find them. Compiled as C: the
 * header prototypes then carry C linkage and dllexport keeps clean names. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return nr_GetInstanceProcAddr_(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return nr_GetDeviceProcAddr_(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
    /* ("interface" is an MSVC keyword via windows.h - hence iface) */
    if (iface->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    iface->loaderLayerInterfaceVersion = 2;
    iface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_engine && pfn_dlss5_destroy) {
            pfn_dlss5_destroy(g_engine);
            g_engine = NULL;
        }
        if (g_chain_dll) {
            FreeLibrary(g_chain_dll);
            g_chain_dll = NULL;
        }
        pfn_dlss5_create = NULL;
        pfn_dlss5_destroy = NULL;
        pfn_dlss5_process_frame = NULL;
        pfn_dlss5_set_scale = NULL;
        pfn_dlss5_set_gain = NULL;
        pfn_dlss5_set_blend = NULL;
        pfn_dlss5_set_sharpness = NULL;
        pfn_dlss5_interpolate_frame = NULL;
        pfn_dlss5_set_framegen = NULL;
        pfn_dlss5_get_framegen = NULL;
        pfn_dlss5_set_phase30 = NULL;
        pfn_dlss5_set_intensity = NULL;
        pfn_dlss5_set_local_tone = NULL;
        pfn_dlss5_set_local_structure = NULL;
        pfn_dlss5_set_skin_structure = NULL;
        pfn_dlss5_set_auto_mask = NULL;
        pfn_dlss5_set_style_index = NULL;
        pfn_dlss5_set_color_strength = NULL;
        g_inprocess_active = 0;
        g_inprocess_attempted = 0;
        if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; }
    }
    return TRUE;
}
