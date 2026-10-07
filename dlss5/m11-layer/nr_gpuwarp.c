// SPDX-License-Identifier: Apache-2.0
/* ============================================================================
 * nr_gpuwarp.c - GPU motion-warped residual compose (see nr_gpuwarp.h).
 *
 * Three embedded compute shaders on the game's own device:
 *   gpuwarp_ds      - 4x box downsample (srcDS once per pair, curDS per
 *                     present);
 *   gpuwarp_match   - quarter-res 3x3-SAD block match -> uvec2 field
 *                     (int16 full-res offset + 8.8 block err);
 *   gpuwarp_compose - per-pixel flat mask, then field-guided displaced
 *                     retry (bilinear field sample + +-2 refine + dense
 *                     r<=4 rings), write composited frame to outBuf.
 * The submit tails with a buffer->image copy into the swapchain image, so
 * the caller's usual staging->image write-back is skipped entirely.
 *
 * Sync convention (same as transfer()): idle mode = submit on the game's
 * queue + our own fence; semaphore mode = the submit additionally signals
 * one slot of an internal 4-slot ring which the caller substitutes as
 * the present's wait semaphore. Coverage counters are read BEFORE the
 * record-side fence wait retires the previous submit - the reported
 * coverage therefore trails one present and the present never stalls on
 * the GPU.
 * ==========================================================================*/
#include "nr_gpuwarp.h"
#include "nr_util.h"
#include "shaders/nr_gpuwarp_spv.h"

#include <stdlib.h>
#include <string.h>

#define GW_SEM_SLOTS 4
#define GW_CNT_BYTES 64
#define GW_TIMEOUT_NS 2000000000ull   /* 2 s - a wedged submit poisons */

typedef struct gw_res {           /* one extent-owned buffer + memory */
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
} gw_res;

struct NrGpuWarp {
    VkDevice dev;
    VkPhysicalDevice phys;
    uint32_t qfam;
    PFN_vkGetDeviceProcAddr gd;
    nr_gw_mem_type_fn mem_type;
    void *mem_ctx;
    long radius_ds;
    int write_dbg;

    /* resolved device-level entry points */
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdFillBuffer CmdFillBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;

    /* fixed objects */
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore sem[GW_SEM_SLOTS];
    unsigned sem_next;
    VkDescriptorPool dpool;
    VkDescriptorSetLayout dsl_ds, dsl_match, dsl_comp;
    VkPipelineLayout pl_ds, pl_match, pl_comp;
    VkPipeline pipe_ds, pipe_match, pipe_comp;
    VkDescriptorSet set_ds_src, set_ds_cur, set_match, set_comp;

    /* extent-owned resources (sized to the PAIR extent) */
    gw_res r_src, r_res, r_cur, r_out, r_sds, r_cds, r_field, r_up, r_dbg;
    gw_res r_cnt;                  /* extent-independent, created once */
    uint32_t w, h, dsw, dsh;
    int extent_ok;

    /* pair state (mirrors nr_residual's policy fields). pair_shadow is a
     * module-owned CPU copy (src then res, each w*h*4) because the
     * caller's resid buffers may be overwritten by the next reply before
     * the device upload gets recorded. */
    int pair_valid, pair_dirty;
    uint32_t pair_w, pair_h;
    uint64_t pair_chain, pair_stamp;
    unsigned char *pair_shadow;

    /* coverage read-back (one present late - see header) */
    int cnt_seen;
    unsigned long last_flat, last_warped, last_fail;
    int poisoned;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#define GWPFN(g, n) \
    g->n = (PFN_vk##n)g->gd(g->dev, "vk" #n)

static int gw_resolve(NrGpuWarp *g)
{
    GWPFN(g, CreateCommandPool);
    GWPFN(g, DestroyCommandPool);
    GWPFN(g, AllocateCommandBuffers);
    GWPFN(g, ResetCommandBuffer);
    GWPFN(g, BeginCommandBuffer);
    GWPFN(g, EndCommandBuffer);
    GWPFN(g, CmdBindPipeline);
    GWPFN(g, CmdBindDescriptorSets);
    GWPFN(g, CmdPushConstants);
    GWPFN(g, CmdDispatch);
    GWPFN(g, CmdPipelineBarrier);
    GWPFN(g, CmdCopyBuffer);
    GWPFN(g, CmdCopyBufferToImage);
    GWPFN(g, CmdFillBuffer);
    GWPFN(g, QueueSubmit);
    GWPFN(g, CreateFence);
    GWPFN(g, DestroyFence);
    GWPFN(g, WaitForFences);
    GWPFN(g, ResetFences);
    GWPFN(g, CreateSemaphore);
    GWPFN(g, DestroySemaphore);
    GWPFN(g, CreateBuffer);
    GWPFN(g, DestroyBuffer);
    GWPFN(g, GetBufferMemoryRequirements);
    GWPFN(g, AllocateMemory);
    GWPFN(g, FreeMemory);
    GWPFN(g, BindBufferMemory);
    GWPFN(g, MapMemory);
    GWPFN(g, UnmapMemory);
    GWPFN(g, CreateShaderModule);
    GWPFN(g, DestroyShaderModule);
    GWPFN(g, CreatePipelineLayout);
    GWPFN(g, DestroyPipelineLayout);
    GWPFN(g, CreateDescriptorSetLayout);
    GWPFN(g, DestroyDescriptorSetLayout);
    GWPFN(g, CreateDescriptorPool);
    GWPFN(g, DestroyDescriptorPool);
    GWPFN(g, AllocateDescriptorSets);
    GWPFN(g, UpdateDescriptorSets);
    GWPFN(g, CreateComputePipelines);
    GWPFN(g, DestroyPipeline);
    return g->CreateCommandPool && g->AllocateCommandBuffers
        && g->BeginCommandBuffer && g->CmdBindPipeline
        && g->CmdBindDescriptorSets && g->CmdPushConstants
        && g->CmdDispatch && g->CmdPipelineBarrier
        && g->CmdCopyBuffer && g->CmdCopyBufferToImage
        && g->CmdFillBuffer && g->QueueSubmit && g->CreateFence
        && g->WaitForFences && g->ResetFences && g->CreateSemaphore
        && g->CreateBuffer && g->GetBufferMemoryRequirements
        && g->AllocateMemory && g->BindBufferMemory && g->MapMemory
        && g->CreateShaderModule && g->CreatePipelineLayout
        && g->CreateDescriptorSetLayout && g->CreateDescriptorPool
        && g->AllocateDescriptorSets && g->UpdateDescriptorSets
        && g->CreateComputePipelines && g->ResetCommandBuffer
        ? 0 : -1;
}

static void gw_res_free(NrGpuWarp *g, gw_res *r)
{
    if (r->map && g->UnmapMemory) {
        g->UnmapMemory(g->dev, r->mem);
        r->map = NULL;
    }
    if (r->buf && g->DestroyBuffer) {
        g->DestroyBuffer(g->dev, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
    }
    if (r->mem && g->FreeMemory) {
        g->FreeMemory(g->dev, r->mem, NULL);
        r->mem = VK_NULL_HANDLE;
    }
    r->size = 0;
}

static int gw_res_alloc(NrGpuWarp *g, gw_res *r, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                        int map)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai;
    uint32_t type;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g->CreateBuffer(g->dev, &bi, NULL, &r->buf) != VK_SUCCESS)
        return -1;
    memset(&mr, 0, sizeof mr);
    g->GetBufferMemoryRequirements(g->dev, r->buf, &mr);
    type = g->mem_type(g->mem_ctx, mr.memoryTypeBits, props);
    if (type == UINT32_MAX) {
        gw_res_free(g, r);
        return -1;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (g->AllocateMemory(g->dev, &ai, NULL, &r->mem) != VK_SUCCESS
        || g->BindBufferMemory(g->dev, r->buf, r->mem, 0) != VK_SUCCESS) {
        gw_res_free(g, r);
        return -1;
    }
    r->size = size;
    if (map) {
        if (g->MapMemory(g->dev, r->mem, 0, size, 0, &r->map) != VK_SUCCESS
            || !r->map) {
            gw_res_free(g, r);
            return -1;
        }
    }
    return 0;
}

static int gw_shader(NrGpuWarp *g, const uint32_t *spv, size_t bytes,
                     VkShaderModule *out)
{
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = spv;
    if (g->CreateShaderModule(g->dev, &ci, NULL, out) != VK_SUCCESS)
        return -1;
    return 0;
}

static VkDescriptorSetLayout gw_dsl(NrGpuWarp *g, int nbind)
{
    VkDescriptorSetLayoutBinding b[8];
    VkDescriptorSetLayoutCreateInfo ci;
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    int i;
    for (i = 0; i < nbind; i++) {
        memset(&b[i], 0, sizeof b[i]);
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = (uint32_t)nbind;
    ci.pBindings = b;
    if (g->CreateDescriptorSetLayout(g->dev, &ci, NULL, &l) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return l;
}

static VkPipelineLayout gw_pl(NrGpuWarp *g, VkDescriptorSetLayout dsl,
                              uint32_t pc_bytes)
{
    VkPipelineLayoutCreateInfo ci;
    VkPushConstantRange pc;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    memset(&pc, 0, sizeof pc);
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = pc_bytes;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &dsl;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &pc;
    if (g->CreatePipelineLayout(g->dev, &ci, NULL, &pl) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pl;
}

static VkPipeline gw_pipe(NrGpuWarp *g, VkPipelineLayout pl,
                          VkShaderModule mod)
{
    VkComputePipelineCreateInfo ci;
    VkPipeline p = VK_NULL_HANDLE;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = pl;
    if (g->CreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p)
        != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

static void gw_bind(VkWriteDescriptorSet *wr, VkDescriptorBufferInfo *bi,
                    VkDescriptorSet set, uint32_t binding, VkBuffer buf,
                    VkDeviceSize size)
{
    memset(wr, 0, sizeof *wr);
    memset(bi, 0, sizeof *bi);
    bi->buffer = buf;
    bi->offset = 0;
    bi->range = size;
    wr->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr->dstSet = set;
    wr->dstBinding = binding;
    wr->descriptorCount = 1;
    wr->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr->pBufferInfo = bi;
}

static int gw_write_descs(NrGpuWarp *g)
{
    /* sets: ds_src{src->sds}, ds_cur{cur->cds}, match{sds,cds,field},
     * comp{cur,src,res,field,out,cnt,dbg} - 16 writes total. */
    VkWriteDescriptorSet wr[16];
    VkDescriptorBufferInfo bi[16];
    int n = 0;
    gw_bind(&wr[n], &bi[n], g->set_ds_src, 0, g->r_src.buf, g->r_src.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_ds_src, 1, g->r_sds.buf, g->r_sds.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_ds_cur, 0, g->r_cur.buf, g->r_cur.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_ds_cur, 1, g->r_cds.buf, g->r_cds.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_match, 0, g->r_sds.buf, g->r_sds.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_match, 1, g->r_cds.buf, g->r_cds.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_match, 2, g->r_field.buf,
            g->r_field.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 0, g->r_cur.buf, g->r_cur.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 1, g->r_src.buf, g->r_src.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 2, g->r_res.buf, g->r_res.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 3, g->r_field.buf,
            g->r_field.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 4, g->r_out.buf, g->r_out.size);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 5, g->r_cnt.buf, GW_CNT_BYTES);
    n++;
    gw_bind(&wr[n], &bi[n], g->set_comp, 6, g->r_dbg.buf, g->r_dbg.size);
    n++;
    g->UpdateDescriptorSets(g->dev, (uint32_t)n, wr, 0, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* create / destroy                                                    */
/* ------------------------------------------------------------------ */

NrGpuWarp *nr_gpuwarp_create(VkPhysicalDevice phys, VkDevice dev,
                             uint32_t queue_family,
                             PFN_vkGetDeviceProcAddr gdproc,
                             nr_gw_mem_type_fn mem_type, void *mem_ctx,
                             nr_gw_fam_ok_fn fam_ok, void *fam_ctx,
                             long radius_ds, int write_dbg)
{
    NrGpuWarp *g;
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo cai;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo di;
    VkDescriptorSetLayout dsls[4];
    VkDescriptorSet sets[4];
    VkDescriptorSetAllocateInfo dai;
    VkShaderModule m_ds = VK_NULL_HANDLE, m_match = VK_NULL_HANDLE,
                  m_comp = VK_NULL_HANDLE;
    int i;

    if (!dev || !gdproc || !mem_type) return NULL;
    if (fam_ok && !fam_ok(fam_ctx, queue_family)) return NULL;
    g = (NrGpuWarp *)calloc(1, sizeof *g);
    if (!g) return NULL;
    g->dev = dev;
    g->phys = phys;
    g->qfam = queue_family;
    g->gd = gdproc;
    g->mem_type = mem_type;
    g->mem_ctx = mem_ctx;
    g->radius_ds = radius_ds > 0 ? radius_ds : 16;
    g->write_dbg = write_dbg;
    if (gw_resolve(g) != 0) {
        nr_log("[gpuwarp] entry points missing; disabled");
        goto fail;
    }

    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = queue_family;
    if (g->CreateCommandPool(g->dev, &pi, NULL, &g->pool) != VK_SUCCESS)
        goto fail;

    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (g->AllocateCommandBuffers(g->dev, &cai, &g->cmd) != VK_SUCCESS)
        goto fail;

    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    /* signalled at create so the first fence wait returns at once */
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (g->CreateFence(g->dev, &fi, NULL, &g->fence) != VK_SUCCESS)
        goto fail;

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < GW_SEM_SLOTS; i++)
        if (g->CreateSemaphore(g->dev, &si, NULL, &g->sem[i])
            != VK_SUCCESS)
            goto fail;

    memset(&psz, 0, sizeof psz);
    psz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    psz.descriptorCount = 16;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    di.maxSets = 4;
    di.poolSizeCount = 1;
    di.pPoolSizes = &psz;
    if (g->CreateDescriptorPool(g->dev, &di, NULL, &g->dpool)
        != VK_SUCCESS)
        goto fail;

    g->dsl_ds = gw_dsl(g, 2);
    g->dsl_match = gw_dsl(g, 3);
    g->dsl_comp = gw_dsl(g, 7);
    if (!g->dsl_ds || !g->dsl_match || !g->dsl_comp) goto fail;
    g->pl_ds = gw_pl(g, g->dsl_ds, 16);
    g->pl_match = gw_pl(g, g->dsl_match, 16);
    g->pl_comp = gw_pl(g, g->dsl_comp, 32);
    if (!g->pl_ds || !g->pl_match || !g->pl_comp) goto fail;

    dsls[0] = dsls[1] = g->dsl_ds;
    dsls[2] = g->dsl_match;
    dsls[3] = g->dsl_comp;
    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = g->dpool;
    dai.descriptorSetCount = 4;
    dai.pSetLayouts = dsls;
    if (g->AllocateDescriptorSets(g->dev, &dai, sets) != VK_SUCCESS)
        goto fail;
    g->set_ds_src = sets[0];
    g->set_ds_cur = sets[1];
    g->set_match = sets[2];
    g->set_comp = sets[3];

    /* compile + pipeline per shader; each module is dropped right after
     * its pipeline is built (no handle leak on a mid-way failure). */
    if (gw_shader(g, gpuwarp_ds_spv, gpuwarp_ds_spv_bytes, &m_ds))
        goto fail;
    g->pipe_ds = gw_pipe(g, g->pl_ds, m_ds);
    g->DestroyShaderModule(g->dev, m_ds, NULL);
    if (!g->pipe_ds) goto fail;
    if (gw_shader(g, gpuwarp_match_spv, gpuwarp_match_spv_bytes,
                  &m_match))
        goto fail;
    g->pipe_match = gw_pipe(g, g->pl_match, m_match);
    g->DestroyShaderModule(g->dev, m_match, NULL);
    if (!g->pipe_match) goto fail;
    if (gw_shader(g, gpuwarp_compose_spv, gpuwarp_compose_spv_bytes,
                  &m_comp))
        goto fail;
    g->pipe_comp = gw_pipe(g, g->pl_comp, m_comp);
    g->DestroyShaderModule(g->dev, m_comp, NULL);
    if (!g->pipe_comp) goto fail;

    /* the coverage buffer is extent-independent - allocate once */
    if (gw_res_alloc(g, &g->r_cnt, GW_CNT_BYTES,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                     | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1))
        goto fail;
    memset(g->r_cnt.map, 0, GW_CNT_BYTES);

    nr_log("[gpuwarp] compute compose ready (qfam %u, radius %ld ds px)",
           queue_family, g->radius_ds);
    return g;

fail:
    nr_log("[gpuwarp] init failed; CPU path stays in charge");
    nr_gpuwarp_destroy(g);
    return NULL;
}

void nr_gpuwarp_destroy(NrGpuWarp *g)
{
    if (!g) return;
    /* free path used both for partial init and teardown; NULL-safe */
    if (g->WaitForFences && g->fence)
        g->WaitForFences(g->dev, 1, &g->fence, VK_TRUE, GW_TIMEOUT_NS);
    gw_res_free(g, &g->r_src);
    gw_res_free(g, &g->r_res);
    gw_res_free(g, &g->r_cur);
    gw_res_free(g, &g->r_out);
    gw_res_free(g, &g->r_sds);
    gw_res_free(g, &g->r_cds);
    gw_res_free(g, &g->r_field);
    gw_res_free(g, &g->r_up);
    gw_res_free(g, &g->r_dbg);
    gw_res_free(g, &g->r_cnt);
    if (g->pipe_ds) g->DestroyPipeline(g->dev, g->pipe_ds, NULL);
    if (g->pipe_match) g->DestroyPipeline(g->dev, g->pipe_match, NULL);
    if (g->pipe_comp) g->DestroyPipeline(g->dev, g->pipe_comp, NULL);
    if (g->pl_ds) g->DestroyPipelineLayout(g->dev, g->pl_ds, NULL);
    if (g->pl_match) g->DestroyPipelineLayout(g->dev, g->pl_match, NULL);
    if (g->pl_comp) g->DestroyPipelineLayout(g->dev, g->pl_comp, NULL);
    if (g->dsl_ds) g->DestroyDescriptorSetLayout(g->dev, g->dsl_ds, NULL);
    if (g->dsl_match)
        g->DestroyDescriptorSetLayout(g->dev, g->dsl_match, NULL);
    if (g->dsl_comp)
        g->DestroyDescriptorSetLayout(g->dev, g->dsl_comp, NULL);
    if (g->dpool) g->DestroyDescriptorPool(g->dev, g->dpool, NULL);
    if (g->fence) g->DestroyFence(g->dev, g->fence, NULL);
    {
        int i;
        for (i = 0; i < GW_SEM_SLOTS; i++)
            if (g->sem[i]) g->DestroySemaphore(g->dev, g->sem[i], NULL);
    }
    if (g->pool) g->DestroyCommandPool(g->dev, g->pool, NULL);
    free(g->pair_shadow);
    free(g);
}

/* ------------------------------------------------------------------ */
/* extent sizing + pair upload                                         */
/* ------------------------------------------------------------------ */

/* Allocate every extent-owned buffer for (w,h). Called on the present
 * thread AFTER the fence wait, so no prior submit still reads them. */
static int gw_ensure_extent(NrGpuWarp *g, uint32_t w, uint32_t h)
{
    const VkDeviceSize fb = (VkDeviceSize)w * h * 4;
    const uint32_t dsw = (w + 3) / 4, dsh = (h + 3) / 4;
    const VkDeviceSize dsb = (VkDeviceSize)dsw * dsh * 4;
    const VkDeviceSize fld = (VkDeviceSize)dsw * dsh * 8;
    const VkMemoryPropertyFlags dev = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                     | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags rw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (g->extent_ok && g->w == w && g->h == h) return 0;
    gw_res_free(g, &g->r_src);
    gw_res_free(g, &g->r_res);
    gw_res_free(g, &g->r_cur);
    gw_res_free(g, &g->r_out);
    gw_res_free(g, &g->r_sds);
    gw_res_free(g, &g->r_cds);
    gw_res_free(g, &g->r_field);
    gw_res_free(g, &g->r_up);
    gw_res_free(g, &g->r_dbg);
    if (gw_res_alloc(g, &g->r_src, fb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_res, fb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_cur, fb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_out, fb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_sds, dsb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_cds, dsb, rw, dev, 0)
        || gw_res_alloc(g, &g->r_field, fld, rw, dev, 0)
        || gw_res_alloc(g, &g->r_up, fb * 2,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, 1)
        || gw_res_alloc(g, &g->r_dbg, fb,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, 1)) {
        nr_log("[gpuwarp] extent alloc failed %ux%u", w, h);
        g->extent_ok = 0;
        return -1;
    }
    g->w = w;
    g->h = h;
    g->dsw = dsw;
    g->dsh = dsh;
    g->extent_ok = 1;
    gw_write_descs(g);
    nr_log("[gpuwarp] extent %ux%u (ds %ux%u)", w, h, dsw, dsh);
    return 0;
}

int nr_gpuwarp_set_pair(NrGpuWarp *g, const unsigned char *src,
                        const signed char *res, uint32_t w, uint32_t h,
                        uint64_t chain, uint64_t stamp_ms)
{
    const size_t fb = (size_t)w * h * 4;
    if (!g || g->poisoned || !src || !res || !w || !h) return -1;
    /* keep only (w,h,chain,stamp) + the bytes; the device upload is
     * memcpy'd into r_up inside run() once the extent buffers exist. */
    if (!g->pair_shadow || g->pair_w != w || g->pair_h != h) {
        unsigned char *ns = (unsigned char *)malloc(fb * 2);
        if (!ns) return -1;
        free(g->pair_shadow);
        g->pair_shadow = ns;
        g->pair_w = w;
        g->pair_h = h;
    }
    memcpy(g->pair_shadow, src, fb);
    memcpy(g->pair_shadow + fb, res, fb);
    g->pair_chain = chain;
    g->pair_stamp = stamp_ms;
    g->pair_valid = 1;
    g->pair_dirty = 1;
    return 0;
}

void nr_gpuwarp_invalidate(NrGpuWarp *g)
{
    if (g) g->pair_valid = 0;
}

/* ------------------------------------------------------------------ */
/* per-present compose                                                 */
/* ------------------------------------------------------------------ */

int nr_gpuwarp_will_run(NrGpuWarp *g, uint32_t w, uint32_t h,
                        uint64_t chain, long max_age_ms, uint64_t now_ms)
{
    if (!g || g->poisoned) return NR_GPUWARP_FAIL;
    if (!g->pair_valid) return NR_GPUWARP_SKIP;
    if (w != g->pair_w || h != g->pair_h || chain != g->pair_chain)
        return NR_GPUWARP_SKIP;
    if (max_age_ms > 0
        && now_ms - g->pair_stamp > (uint64_t)max_age_ms)
        return NR_GPUWARP_SKIP;
    return NR_GPUWARP_OK;
}

static void gw_barrier(NrGpuWarp *g, VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = src;
    mb.dstAccessMask = dst;
    g->CmdPipelineBarrier(g->cmd, ss, ds, 0, 1, &mb, 0, NULL, 0, NULL);
}

int nr_gpuwarp_run(NrGpuWarp *g, VkQueue queue, VkBuffer cur_buf,
                   VkImage dst, uint32_t w, uint32_t h, int px_eps,
                   int want_signal)
{
    VkResult vr;
    VkCommandBufferBeginInfo bi;
    uint32_t dsw, dsh;
    size_t fb;

    if (!g || g->poisoned) return NR_GPUWARP_FAIL;
    if (w != g->pair_w || h != g->pair_h || !g->pair_valid)
        return NR_GPUWARP_SKIP;
    fb = (size_t)w * h * 4;
    dsw = (w + 3) / 4;
    dsh = (h + 3) / 4;

    /* retire the previous submit before re-recording/binding buffers;
     * this also publishes the coverage counters it wrote (lag-1 read). */
    vr = g->WaitForFences(g->dev, 1, &g->fence, VK_TRUE, GW_TIMEOUT_NS);
    if (vr != VK_SUCCESS) {
        nr_log("[gpuwarp] fence wait failed (%d); GPU path off",
               (int)vr);
        g->poisoned = 1;
        return NR_GPUWARP_FAIL;
    }
    {
        const uint32_t *c = (const uint32_t *)g->r_cnt.map;
        g->last_flat = c[0];
        g->last_warped = c[1];
        g->last_fail = c[2];
        g->cnt_seen = 1;
    }
    if (g->ResetFences(g->dev, 1, &g->fence) != VK_SUCCESS)
        goto fail;

    /* extent buffers follow the PAIR extent (checked above). */
    if (gw_ensure_extent(g, w, h) != 0) goto fail;

    if (g->ResetCommandBuffer(g->cmd, 0) != VK_SUCCESS) goto fail;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g->BeginCommandBuffer(g->cmd, &bi) != VK_SUCCESS) goto fail;

    /* pair upload (once per reply): CPU shadow -> upBuf already memcpy'd
     * in set_pair; copy up->src/res on device. */
    if (g->pair_dirty) {
        /* push the CPU shadow into the upload buffer (host-coherent) */
        memcpy(g->r_up.map, g->pair_shadow, fb);
        memcpy((unsigned char *)g->r_up.map + fb, g->pair_shadow + fb,
               fb);
        {
            VkBufferCopy cp;
            memset(&cp, 0, sizeof cp);
            cp.size = fb;
            g->CmdCopyBuffer(g->cmd, g->r_up.buf, g->r_src.buf, 1, &cp);
            cp.srcOffset = fb;
            cp.dstOffset = 0;
            cp.size = fb;
            g->CmdCopyBuffer(g->cmd, g->r_up.buf, g->r_res.buf, 1, &cp);
        }
        gw_barrier(g, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        /* srcDS once per pair */
        {
            uint32_t pc[4] = { w, h, dsw, dsh };
            g->CmdBindPipeline(g->cmd,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               g->pipe_ds);
            g->CmdBindDescriptorSets(g->cmd,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     g->pl_ds, 0, 1, &g->set_ds_src,
                                     0, NULL);
            g->CmdPushConstants(g->cmd, g->pl_ds,
                                VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                sizeof pc, pc);
            g->CmdDispatch(g->cmd, (dsw + 15) / 16, (dsh + 15) / 16, 1);
        }
        g->pair_dirty = 0;
    }

    /* current frame: staging -> curBuf */
    {
        VkBufferCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.size = fb;
        g->CmdCopyBuffer(g->cmd, cur_buf, g->r_cur.buf, 1, &cp);
    }
    /* zero the coverage counters for this dispatch */
    g->CmdFillBuffer(g->cmd, g->r_cnt.buf, 0, GW_CNT_BYTES, 0);
    gw_barrier(g, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    /* curDS */
    {
        uint32_t pc[4] = { w, h, dsw, dsh };
        g->CmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           g->pipe_ds);
        g->CmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 g->pl_ds, 0, 1, &g->set_ds_cur, 0, NULL);
        g->CmdPushConstants(g->cmd, g->pl_ds,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof pc, pc);
        g->CmdDispatch(g->cmd, (dsw + 15) / 16, (dsh + 15) / 16, 1);
    }
    gw_barrier(g, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    /* block-match field */
    {
        uint32_t pc[4] = { dsw, dsh, (uint32_t)g->radius_ds, 0 };
        g->CmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           g->pipe_match);
        g->CmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 g->pl_match, 0, 1, &g->set_match,
                                 0, NULL);
        g->CmdPushConstants(g->cmd, g->pl_match,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof pc, pc);
        g->CmdDispatch(g->cmd, (dsw + 15) / 16, (dsh + 15) / 16, 1);
    }
    gw_barrier(g, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    /* compose + coverage */
    {
        uint32_t pc[8] = { w, h, dsw, dsh,
                           (uint32_t)px_eps,
                           g->write_dbg ? 1u : 0u, 0u, 0u };
        g->CmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           g->pipe_comp);
        g->CmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 g->pl_comp, 0, 1, &g->set_comp,
                                 0, NULL);
        g->CmdPushConstants(g->cmd, g->pl_comp,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof pc, pc);
        g->CmdDispatch(g->cmd, (w + 15) / 16, (h + 15) / 16, 1);
    }
    gw_barrier(g, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* outBuf -> swapchain image (same layout dance as transfer()) */
    {
        VkImageMemoryBarrier into;
        memset(&into, 0, sizeof into);
        into.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        into.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        into.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        into.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        into.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        into.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        into.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        into.image = dst;
        into.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        into.subresourceRange.baseMipLevel = 0;
        into.subresourceRange.levelCount = 1;
        into.subresourceRange.baseArrayLayer = 0;
        into.subresourceRange.layerCount = 1;
        g->CmdPipelineBarrier(g->cmd,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, NULL, 0, NULL, 1, &into);

        {
            VkBufferImageCopy region;
            memset(&region, 0, sizeof region);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = w;
            region.imageExtent.height = h;
            region.imageExtent.depth = 1;
            g->CmdCopyBufferToImage(g->cmd, g->r_out.buf, dst,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &region);
        }

        {
            VkImageMemoryBarrier back = into;
            back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            g->CmdPipelineBarrier(g->cmd,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                  0, NULL, 0, NULL, 1, &back);
        }
    }
    if (g->EndCommandBuffer(g->cmd) != VK_SUCCESS) goto fail;
    {
        VkSubmitInfo sub;
        VkSemaphore sig = VK_NULL_HANDLE;
        memset(&sub, 0, sizeof sub);
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &g->cmd;
        if (want_signal) {
            sig = g->sem[g->sem_next % GW_SEM_SLOTS];
            g->sem_next++;
            sub.signalSemaphoreCount = 1;
            sub.pSignalSemaphores = &sig;
        }
        vr = g->QueueSubmit(queue, 1, &sub, g->fence);
        if (vr != VK_SUCCESS) goto fail;
    }
    return NR_GPUWARP_OK;

fail:
    nr_log("[gpuwarp] submit/record failed; GPU path off");
    g->poisoned = 1;
    return NR_GPUWARP_FAIL;
}

VkSemaphore nr_gpuwarp_done_sem(const NrGpuWarp *g)
{
    if (!g || !g->sem_next) return VK_NULL_HANDLE;
    return g->sem[(g->sem_next - 1) % GW_SEM_SLOTS];
}

long nr_gpuwarp_last_cov_pm(const NrGpuWarp *g)
{
    const uint64_t npix = g && g->extent_ok
                          ? (uint64_t)g->w * g->h : 0;
    if (!g || !g->cnt_seen || !npix) return -1;
    return (long)(((uint64_t)(g->last_flat + g->last_warped) * 1000)
                  / npix);
}

long nr_gpuwarp_last_warp_pm(const NrGpuWarp *g)
{
    const uint64_t npix = g && g->extent_ok
                          ? (uint64_t)g->w * g->h : 0;
    if (!g || !g->cnt_seen || !npix) return -1;
    return (long)(((uint64_t)g->last_warped * 1000) / npix);
}

int nr_gpuwarp_poisoned(const NrGpuWarp *g)
{
    return g ? g->poisoned : 1;
}

int nr_gpuwarp_counters(const NrGpuWarp *g, unsigned long *flat,
                        unsigned long *warped, unsigned long *fail)
{
    const uint32_t *c;
    if (!g || !g->r_cnt.map) return -1;
    c = (const uint32_t *)g->r_cnt.map;
    if (flat) *flat = c[0];
    if (warped) *warped = c[1];
    if (fail) *fail = c[2];
    return 0;
}

const uint32_t *nr_gpuwarp_dbg(const NrGpuWarp *g)
{
    if (!g || !g->write_dbg || !g->extent_ok || !g->r_dbg.map)
        return NULL;
    return (const uint32_t *)g->r_dbg.map;
}
