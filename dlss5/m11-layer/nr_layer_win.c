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
 * 127.0.0.1:NR_LAYER_PORT (default 47990), access() -> _access(), stderr ->
 * %TEMP%\nr_layer_win.log. Logic mirrors the Linux original 1:1; the default
 * sync model stays "idle" (vkQueueWaitIdle), NR_LAYER_SYNC=semaphore enables
 * the present-semaphore ring.
 *
 * Env: ENABLE_NR_LAYER=1 arms the layer (manifest enable_environment).
 *   NR_LAYER_PORT     daemon TCP port (default 47990)
 *   NR_LAYER_TRIGGER  while this file exists: process + hold (photo mode)
 *   NR_LAYER_LIVE N   every Nth present processed; between frames re-blit the
 *                     last result (slideshow mode). N=1 = every present.
 *                     Arms CTRL+ALT+X (pause/resume passthrough) and
 *                     CTRL+ALT+Q (layer off) hotkeys.
 *   NR_LAYER_NOPATCH  1 = do not touch swapchains (pure passthrough; bisect)
 *   NR_LAYER_CAPTURE  write the next captured frame (header+pixels) here
 *   NR_LAYER_EVERY N  capture every Nth present even without a trigger
 *   NR_LAYER_SYNC     "semaphore" = present-semaphore ring; default = idle
 *   NR_LAYER_UI_MASK  1 = send a held-still mask with the frame
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

#define MAX_SWAPCHAINS 8
#define MAX_IMAGES 8
#define SYNC_SLOTS 4
#define NR_MAX_WAITS 16
#define NR_DEFAULT_PORT 47990

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
    unsigned char *outgoing;
    VkDeviceSize outgoing_size;
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
static long capture_every;
static long live_every;
static long nr_port = NR_DEFAULT_PORT;
static const char *trigger_path;
static int ui_mask;
static int no_patch;            /* NR_LAYER_NOPATCH=1: leave swapchains alone */
static volatile LONG g_paused;  /* CTRL+ALT+X: passthrough, full fps */
static volatile LONG g_off;     /* CTRL+ALT+Q: layer off for good */
static volatile LONG g_hotkeys_started;

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
    DWORD tv = 60000;
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
    VkResult r = create(info, allocator, instance);
    if (r == VK_SUCCESS) {
        layer_instance = *instance;
        capture_path = getenv("NR_LAYER_CAPTURE");
        trigger_path = getenv("NR_LAYER_TRIGGER");
        const char *mask = getenv("NR_LAYER_UI_MASK");
        ui_mask = mask && strcmp(mask, "0") != 0;
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
        if (live_every > 0)
            hotkeys_ensure();
            nr_log("[nr_layer] live: every %ld present goes through the network, "
                 "the frames between hold the last result", live_every);
        if (sync_semaphores)
            nr_log("[nr_layer] sync: the present's own semaphores, not a queue idle");
        nr_log("[nr_layer] active; port=%ld trigger=%s capture=%s every=%ld",
             nr_port, trigger_path ? trigger_path : "(none)",
             capture_path ? capture_path : "(off)", capture_every);
        if (ui_mask)
            nr_log("[nr_layer] ui mask on: the first present after the trigger "
                 "is kept to find what held still");
    }
    return r;
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

    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_instance(NULL, "vkCreateDevice");
    VkResult r = create(physical, info, allocator, device);
    if (r != VK_SUCCESS) return r;

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
    free(data->outgoing);
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

/* Which pixels are the interface: the ones that held still between two
 * presents while the scene moved. Refused when nearly nothing moved (a mask
 * cannot be told from a still scene) or when almost everything did. */
static uint32_t settled(const unsigned char *now, const unsigned char *before,
                        uint32_t pixels, unsigned char *mask)
{
    uint32_t held = 0;
    for (uint32_t i = 0; i < pixels; i++) {
        const unsigned char *a = now + 4 * i, *b = before + 4 * i;
        int moved = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 6;
        mask[i] = moved ? 0u : 0xFFu;
        held += !moved;
    }
    return held;
}

static int mask_worth_sending(uint32_t held, uint32_t pixels)
{
    return held < (uint32_t)((uint64_t)pixels * 9 / 10) && held > pixels / 50;
}

static int process_frame(device_data *data, swapchain_data *chain,
                         VkQueue queue, uint32_t index)
{
    VkDeviceSize needed = (VkDeviceSize)chain->extent.width * chain->extent.height * 4;
    if (ensure_resources(data, needed)) return -1;
    if (transfer(data, chain, queue, index, 0)) return -1;

    uint32_t pixels = chain->extent.width * chain->extent.height;
    int masked = 0;
    if (ui_mask && data->have_earlier && data->earlier_size >= needed) {
        if (data->outgoing_size < needed + pixels) {
            unsigned char *grown = (unsigned char *)realloc(data->outgoing,
                                                            (size_t)needed + pixels);
            if (!grown) return -1;
            data->outgoing = grown;
            data->outgoing_size = needed + pixels;
        }
        memcpy(data->outgoing, data->mapped, (size_t)needed);
        uint32_t held = settled(data->mapped, data->earlier, pixels,
                                data->outgoing + needed);
        masked = mask_worth_sending(held, pixels);
        nr_log("[nr_layer] %u%% of the frame held still; ui mask %s",
             100u * held / pixels, masked ? "sent" : "refused");
    }

    uint32_t header[4] = { masked ? 0x314E524Eu : 0x304E524Eu, chain->extent.width,
                           chain->extent.height, (uint32_t)chain->format };
    if (capture_path) {
        FILE *file = fopen(capture_path, "wb");
        if (file) {
            fwrite(header, sizeof header, 1, file);
            fwrite(data->mapped, 1, (size_t)needed, file);
            fclose(file);
        }
    }
    /* No daemon configured: capture-only mode (frame passes through). */
    if (!nr_port) return -1;
    if (masked) {
        if (exchange(header, sizeof header, data->outgoing,
                     (size_t)needed + pixels, data->result, (size_t)needed)) {
            nr_log("[nr_layer] the daemon did not answer; frame unchanged");
            return -1;
        }
        memcpy(data->mapped, data->result, (size_t)needed);
        nr_log("[nr_layer] processed %ux%u with a ui mask",
             chain->extent.width, chain->extent.height);
        return 0;
    }
    if (exchange(header, sizeof header, data->mapped, (size_t)needed, data->result,
                 (size_t)needed)) {
        nr_log("[nr_layer] the daemon did not answer; frame unchanged");
        return -1;
    }
    memcpy(data->mapped, data->result, (size_t)needed);
    nr_log("[nr_layer] processed %ux%u", chain->extent.width, chain->extent.height);
    return 0;
}

/* The present, with its waits replaced by ours if a transfer consumed them. */
static VkResult present_now(device_data *data, VkQueue queue,
                            const VkPresentInfoKHR *info)
{
    if (!sync_semaphores || !data || !data->present_signal)
        return data->present(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &data->present_signal;
    return data->present(queue, &patched);
}

VKAPI_ATTR VkResult VKAPI_CALL nr_QueuePresentKHR(VkQueue queue,
                                                  const VkPresentInfoKHR *info)
{
    frame_counter++;
    device_data *data = NULL;
    queue_data *owner = find_queue(queue);
    if (owner && !owner->capture_ok) {
        device_data *host = find_device(owner->device);
        if (host) return host->present(queue, info);
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
    if (!data) return VK_ERROR_INITIALIZATION_FAILED;

    data->present_plain = info->waitSemaphoreCount > NR_MAX_WAITS;
    data->present_wait = info->pWaitSemaphores;
    data->present_wait_count = (sync_semaphores && !data->present_plain)
        ? info->waitSemaphoreCount : 0;
    data->present_signal = VK_NULL_HANDLE;

    /* Hotkeys: CTRL+ALT+Q = layer off for good. Paused (CTRL+ALT+X) means
     * plain passthrough: native frames, full fps, the held result survives
     * so resume continues the slideshow. */
    if (g_off)
        return data->present(queue, info);
    if (g_paused && live_every > 0)
        return data->present(queue, info);

    /* Live mode: every Nth present through the network, the frames between
     * re-blit the last result so the picture is steady. */
    if (live_every > 0) {
        int on = !trigger_path || _access(trigger_path, 0) == 0;
        if (!on) data->holding = 0;
        for (uint32_t i = 0; on && i < info->swapchainCount; i++) {
            swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
            if (!chain || info->pImageIndices[i] >= chain->image_count) continue;
            uint32_t index = info->pImageIndices[i];
            VkDeviceSize want = (VkDeviceSize)chain->extent.width
                              * chain->extent.height * 4;
            if (frame_counter % (unsigned long)live_every == 0) {
                if (process_frame(data, chain, queue, index) == 0) {
                    data->holding = 1;
                    data->holding_chain = chain->swapchain;
                    transfer(data, chain, queue, index, 1);
                }
            } else if (data->holding && data->holding_chain == chain->swapchain
                       && data->result_size == want) {
                memcpy(data->mapped, data->result, (size_t)data->result_size);
                transfer(data, chain, queue, index, 1);
            }
        }
        return present_now(data, queue, info);
    }

    /* Photo mode: a file is the trigger; while it exists the processed frame
     * is held on screen. */
    int wanted = trigger_path && _access(trigger_path, 0) == 0;
    if (!wanted && capture_every > 0)
        wanted = frame_counter % (unsigned long)capture_every == 0;

    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        swapchain_data *chain = find_swapchain(info->pSwapchains[i]);
        if (!chain || info->pImageIndices[i] >= chain->image_count) continue;
        uint32_t index = info->pImageIndices[i];
        if (wanted && ui_mask && !data->holding && !data->have_earlier) {
            VkDeviceSize needed = (VkDeviceSize)chain->extent.width
                                  * chain->extent.height * 4;
            if (ensure_resources(data, needed) == 0
                && transfer(data, chain, queue, index, 0) == 0) {
                if (data->earlier_size < needed) {
                    unsigned char *grown = (unsigned char *)realloc(data->earlier,
                                                                    (size_t)needed);
                    if (grown) { data->earlier = grown; data->earlier_size = needed; }
                }
                if (data->earlier_size >= needed) {
                    memcpy(data->earlier, data->mapped, (size_t)needed);
                    data->have_earlier = 1;
                }
            }
        } else if (wanted && !data->holding) {
            if (process_frame(data, chain, queue, index) == 0) {
                data->holding = 1;
                transfer(data, chain, queue, index, 1);
            }
        } else if (wanted && data->holding
                   && data->result_size == (VkDeviceSize)chain->extent.width
                                            * chain->extent.height * 4) {
            memcpy(data->mapped, data->result, (size_t)data->result_size);
            transfer(data, chain, queue, index, 1);
        } else if (!wanted) {
            data->holding = 0;
            data->have_earlier = 0;
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
        if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; }
    }
    return TRUE;
}
