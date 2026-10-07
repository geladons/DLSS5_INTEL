// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * gpuwarp_test - offline numerics harness for the layer-side GPU residual
 * compose (nr_gpuwarp.c). Links the REAL module + the REAL CPU reference
 * (nr_residual.c) against a private Vulkan instance/device, so the
 * comparison exercises exactly the code the layer runs in-process - same
 * shaders, same buffers, same image write-back dance.
 *
 *   gpuwarp_test.exe src.bin proc.bin cur.bin W H [eps] outdir
 *
 *   src.bin/proc.bin/cur.bin : raw w*h*4 BGRA frames (the src/proc pair is
 *                            a real daemon reply pair; cur is a later
 *                            captured frame at the same extent)
 *   out_cpu.bin              : nr_residual_apply() output on cur
 *   out_gpu.bin              : the swapchain-image content nr_gpuwarp_run
 *                              wrote (read back through a second image
 *                              copy - the true presented bytes)
 *   dbg.bin                  : per-pixel applied offsets (u32: int16x2,
 *                              0x7FFF7FFF = rejected, 0 = flat-applied)
 *   counters.txt             : flat/warped/fail counts + coverage pm
 *
 * x64 only (developer-side tool - links vulkan-1.lib; the layer itself
 * resolves via the loader chain and never needs the import lib).
 * ==========================================================================*/
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../nr_residual.h"
#include "../nr_gpuwarp.h"

/* ---------------- tiny vulkan boilerplate ---------------- */

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;

static uint32_t host_mem_type(void *ctx, uint32_t bits,
                              VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    (void)ctx;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    /* host-visible without the cached bit (fall-back for the readback) */
    if (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
        VkMemoryPropertyFlags lo = want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i))
                && (mp.memoryTypes[i].propertyFlags & lo) == lo)
                return i;
    }
    return UINT32_MAX;
}

static int fam_ok(void *ctx, uint32_t family)
{
    uint32_t count = 0;
    (void)ctx;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &count, NULL);
    VkQueueFamilyProperties *f =
        (VkQueueFamilyProperties *)calloc(count, sizeof *f);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &count, f);
    int ok = family < count
             && (f[family].queueFlags & (VK_QUEUE_COMPUTE_BIT
                                         | VK_QUEUE_TRANSFER_BIT)) != 0;
    free(f);
    return ok;
}

static int vk_init(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "gpuwarp_test";
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) return -1;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, NULL);
    if (!n) return -1;
    VkPhysicalDevice *pds =
        (VkPhysicalDevice *)calloc(n, sizeof *pds);
    vkEnumeratePhysicalDevices(g_inst, &n, pds);
    g_phys = VK_NULL_HANDLE;
    /* prefer a real discrete/integrated GPU that has compute */
    for (uint32_t i = 0; i < n && !g_phys; i++) {
        uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &fc, NULL);
        VkQueueFamilyProperties *f =
            (VkQueueFamilyProperties *)calloc(fc, sizeof *f);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &fc, f);
        for (uint32_t j = 0; j < fc && !g_phys; j++) {
            if ((f[j].queueFlags & VK_QUEUE_COMPUTE_BIT)
                && (f[j].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                g_phys = pds[i];
                g_qfam = j;
            }
        }
        free(f);
    }
    free(pds);
    if (!g_phys) return -1;

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g_phys, &pp);
    fprintf(stderr, "[gpuwarp_test] device: %s (qfam %u)\n",
            pp.deviceName, g_qfam);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    if (vkCreateDevice(g_phys, &dci, NULL, &g_dev) != VK_SUCCESS) return -1;
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    return 0;
}

static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkFence g_fence;

static int vk_cmd_init(void)
{
    VkCommandPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g_qfam;
    if (vkCreateCommandPool(g_dev, &pi, NULL, &g_pool) != VK_SUCCESS)
        return -1;
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev, &ai, &g_cmd) != VK_SUCCESS)
        return -1;
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(g_dev, &fi, NULL, &g_fence) != VK_SUCCESS) return -1;
    return 0;
}

static int make_buf(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, VkBuffer *buf,
                    VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(g_dev, &bi, NULL, buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, *buf, &mr);
    uint32_t t = host_mem_type(NULL, mr.memoryTypeBits, props);
    if (t == UINT32_MAX) return -1;
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = t;
    if (vkAllocateMemory(g_dev, &ai, NULL, mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(g_dev, *buf, *mem, 0) != VK_SUCCESS) return -1;
    if (map) {
        if (vkMapMemory(g_dev, *mem, 0, size, 0, map) != VK_SUCCESS)
            return -1;
    }
    return 0;
}

static void rec_begin(void)
{
    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);
}

static void rec_submit_wait(void)
{
    vkEndCommandBuffer(g_cmd);
    VkSubmitInfo sub;
    memset(&sub, 0, sizeof sub);
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &g_cmd;
    vkQueueSubmit(g_queue, 1, &sub, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 10000000000ull);
    vkResetFences(g_dev, 1, &g_fence);
}

static void img_barrier(VkImage img, VkImageLayout old_l,
                        VkImageLayout new_l, VkAccessFlags sa,
                        VkAccessFlags da)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(g_cmd,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static unsigned char *read_file(const char *path, size_t expect)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != (long)expect) {
        fprintf(stderr, "%s: size %ld != expected %zu\n", path, sz, expect);
        fclose(f);
        return NULL;
    }
    unsigned char *p = (unsigned char *)malloc(expect);
    if (fread(p, 1, expect, f) != expect) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

static int write_file(const char *path, const void *data, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, bytes, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: gpuwarp_test src.bin proc.bin cur.bin W H [eps] [outdir]\n");
        return 2;
    }
    const char *srcp = argv[1], *procp = argv[2], *curp = argv[3];
    uint32_t w = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t h = (uint32_t)strtoul(argv[5], NULL, 10);
    long eps = argc > 6 ? strtol(argv[6], NULL, 10) : 12;
    const char *outdir = argc > 7 ? argv[7] : ".";
    size_t fb = (size_t)w * h * 4;
    char path[512];

    unsigned char *src = read_file(srcp, fb);
    unsigned char *proc = read_file(procp, fb);
    unsigned char *cur = read_file(curp, fb);
    if (!src || !proc || !cur) return 2;

    if (vk_init() || vk_cmd_init()) {
        fprintf(stderr, "[gpuwarp_test] vulkan init failed\n");
        return 2;
    }

    /* ---- CPU reference: the production update+apply chain ---- */
    NrResidual *resid = nr_residual_create();
    nr_residual_configure(resid, 400, 3000, 800, eps);
    if (!nr_residual_update(resid, src, proc, w, h, 1, 1, 1, 1000)) {
        fprintf(stderr, "residual_update rejected the pair\n");
        return 2;
    }
    unsigned char *cpu_out = (unsigned char *)malloc(fb);
    memcpy(cpu_out, cur, fb);
    int verdict = nr_residual_apply(resid, cpu_out, w, h, 1, 1100);
    long cpu_cov = nr_residual_last_cov_pm(resid);
    long cpu_rec = nr_residual_last_recov_pm(resid);
    snprintf(path, sizeof path, "%s\\out_cpu.bin", outdir);
    write_file(path, cpu_out, fb);

    /* ---- GPU path: same pair through nr_gpuwarp ---- */
    NrGpuWarp *gw = nr_gpuwarp_create(g_phys, g_dev, g_qfam,
                                    vkGetDeviceProcAddr,
                                    host_mem_type, NULL,
                                    fam_ok, NULL, 16, 1);
    if (!gw) {
        fprintf(stderr, "nr_gpuwarp_create failed\n");
        return 2;
    }
    const unsigned char *psrc;
    const signed char *pres;
    uint32_t pw, ph;
    uint64_t pchain, pstamp;
    if (!nr_residual_pair(resid, &psrc, &pres, &pw, &ph, &pchain, &pstamp)) {
        fprintf(stderr, "nr_residual_pair: no pair\n");
        return 2;
    }
    if (nr_gpuwarp_set_pair(gw, psrc, pres, pw, ph, pchain, pstamp))
        return 2;
    int wr = nr_gpuwarp_will_run(gw, w, h, pchain, 400, 1100);
    if (wr != NR_GPUWARP_OK) {
        fprintf(stderr, "will_run -> %d\n", wr);
        return 2;
    }

    /* staging buffer = the layer's `data->staging` equivalent (current
     * captured frame, host-visible) */
    VkBuffer stage; VkDeviceMemory stage_mem; void *stage_map;
    if (make_buf(fb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stage, &stage_mem, &stage_map))
        return 2;
    memcpy(stage_map, cur, fb);

    /* the "swapchain" image: plain 2D, sits in PRESENT_SRC just like the
     * real path assumes */
    VkImage dst;
    VkDeviceMemory dst_mem;
    {
        VkImageCreateInfo ii;
        memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent.width = w;
        ii.extent.height = h;
        ii.extent.depth = 1;
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(g_dev, &ii, NULL, &dst) != VK_SUCCESS)
            return 2;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_dev, dst, &mr);
        uint32_t t = host_mem_type(NULL, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = t;
        if (vkAllocateMemory(g_dev, &ai, NULL, &dst_mem) != VK_SUCCESS)
            return 2;
        vkBindImageMemory(g_dev, dst, dst_mem, 0);
        rec_begin();
        img_barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0,
                    VK_ACCESS_MEMORY_READ_BIT);
        rec_submit_wait();
    }

    wr = nr_gpuwarp_run(gw, g_queue, stage, dst, w, h, (int)eps, 0);
    if (wr != NR_GPUWARP_OK) {
        fprintf(stderr, "nr_gpuwarp_run -> %d\n", wr);
        return 2;
    }
    vkDeviceWaitIdle(g_dev);

    /* read the image back the same way transfer(0) would */
    VkBuffer rb; VkDeviceMemory rb_mem; void *rb_map;
    if (make_buf(fb, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &rb, &rb_mem, &rb_map))
        return 2;
    rec_begin();
    img_barrier(dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    {
        VkBufferImageCopy r;
        memset(&r, 0, sizeof r);
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = w;
        r.imageExtent.height = h;
        r.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(g_cmd, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               rb, 1, &r);
    }
    rec_submit_wait();

    snprintf(path, sizeof path, "%s\\out_gpu.bin", outdir);
    write_file(path, rb_map, fb);

    unsigned long flat = 0, warped = 0, fail = 0;
    nr_gpuwarp_counters(gw, &flat, &warped, &fail);
    const uint32_t *dbg = nr_gpuwarp_dbg(gw);
    if (dbg) {
        snprintf(path, sizeof path, "%s\\dbg.bin", outdir);
        write_file(path, dbg, fb);
    }
    snprintf(path, sizeof path, "%s\\counters.txt", outdir);
    {
        FILE *f = fopen(path, "w");
        if (f) {
            uint64_t npix = (uint64_t)w * h;
            fprintf(f, "verdict=%d cpu_cov_pm=%ld cpu_warp_pm=%ld\n",
                    verdict, cpu_cov, cpu_rec);
            fprintf(f, "gpu_flat=%lu gpu_warped=%lu gpu_fail=%lu\n",
                    flat, warped, fail);
            fprintf(f, "gpu_cov_pm=%ld gpu_warp_pm=%ld\n",
                    npix ? (long)((flat + warped) * 1000 / npix) : 0,
                    npix ? (long)(warped * 1000 / npix) : 0);
            fclose(f);
        }
    }
    {
        long gcov = (long)((flat + warped) * 1000
                           / ((uint64_t)w * h));
        printf("cpu: verdict=%d cov=%ld.%ld%% warp=+%ld.%ld%% | "
               "gpu: flat=%lu warped=%lu fail=%lu cov=%ld.%ld%%\n",
               verdict, cpu_cov / 10, cpu_cov % 10, cpu_rec / 10,
               cpu_rec % 10, flat, warped, fail, gcov / 10, gcov % 10);
    }
    nr_gpuwarp_destroy(gw);
    nr_residual_destroy(resid);
    return 0;
}
