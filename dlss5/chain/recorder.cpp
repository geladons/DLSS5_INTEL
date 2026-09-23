// ============================================================================
// dlss5/chain — ChainRecorder: pipeline setup + per-kernel dispatchers.
// ============================================================================
#include "recorder.h"

namespace d5c {

static void chainMakePipe(const VkCtx& c, const std::string& spv, ChainPipe& out) {
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout dsl;
    D5C_VK_FATAL(vkCreateDescriptorSetLayout(c.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    D5C_VK_FATAL(vkCreatePipelineLayout(c.dev, &plci, nullptr, &out.layout));
    VkShaderModule mod = LoadSpv(c, spv);
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = out.layout;
    D5C_VK_FATAL(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out.pipe));
    vkDestroyShaderModule(c.dev, mod, nullptr);
    out.dsl = dsl;   // NOT destroyed: pipeline layout references it (device-lost on Arc otherwise)
}

void ChainRecorder::init(const VkCtx& c, const ChainArena& arena, const WeightsStore& ws,
                         const Stage st[7], const std::string& shaderDir) {
    c_ = &c;
    ar_ = &arena;
    ws_ = &ws;
    for (int i = 0; i < 7; ++i) st_[i] = st[i];
    T6_ = st[6].TOK;
    T6P_ = (st[6].TOK + 31) & ~31u;
    chainMakePipe(c, shaderDir + "gemm.spv", pGemm);
    chainMakePipe(c, shaderDir + "gemm_rn1.spv", pGemm1);
    chainMakePipe(c, shaderDir + "cosine.spv", pCos);
    chainMakePipe(c, shaderDir + "cosine_win.spv", pCosW);
    chainMakePipe(c, shaderDir + "softmax.spv", pSmax);
    chainMakePipe(c, shaderDir + "elementwise.spv", pEw);
    chainMakePipe(c, shaderDir + "partition.spv", pPart);
    chainMakePipe(c, shaderDir + "transpose_we.spv", pTrans);
    chainMakePipe(c, shaderDir + "gather_residual.spv", pGather);
    chainMakePipe(c, shaderDir + "merge.spv", pMerge);
    chainMakePipe(c, shaderDir + "featpack.spv", pFeatpack);
    chainMakePipe(c, shaderDir + "headpack.spv", pHeadpack);
    chainMakePipe(c, shaderDir + "pool2.spv", pPool);
    chainMakePipe(c, shaderDir + "upmerge.spv", pUpM);
    std::printf("[chain] 14 pipelines up (10 kernels + featpack/headpack + pool2/upmerge), %zu BDA chunk bases, chunk0 0x%llx\n",
                arena.chunks.size(), (unsigned long long)arena.chunks[0].baseA);
}

void ChainRecorder::destroy(const VkCtx& c) {
    for (ChainPipe* cp : {&pGemm, &pGemm1, &pCos, &pCosW, &pSmax, &pEw, &pPart, &pTrans,
                          &pGather, &pMerge, &pFeatpack, &pHeadpack, &pPool, &pUpM}) {
        vkDestroyPipeline(c.dev, cp->pipe, nullptr);
        vkDestroyPipelineLayout(c.dev, cp->layout, nullptr);
        vkDestroyDescriptorSetLayout(c.dev, cp->dsl, nullptr);
    }
}

void ChainRecorder::bar(VkCommandBuffer cb) {
    // M9b: full compute barrier across every arena chunk. M10 finding: on
    // this Arc driver each ADDITIONAL buffer barrier in the call costs
    // ~143 us regardless of size, so the arena is allocated as ONE sparse
    // buffer whenever possible (see arena.cpp) and this loop emits exactly
    // one VkBufferMemoryBarrier in the hot path.
    VkBufferMemoryBarrier bs[8];
    uint32_t n = 0;
    for (auto& ch : ar_->chunks) {
        if (n >= 8) break;
        bs[n] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                 VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                 ch.b.buf, 0, VK_WHOLE_SIZE};
        n++;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         n, bs, 0, nullptr);
}

void ChainRecorder::dGemm(VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
                          uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                          uint32_t sa, uint32_t sb, uint32_t sc,
                          uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
    (void)fam;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
    GemmPush p{a, b, c2, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc};
    vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, n / 32, m / 16, batch);
    pm(cb, "gemm", "blk", m, n, k, n / 32, m / 16, batch);
}

void ChainRecorder::dEw(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                        uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
    EWPush p{a, b, c2, d, h, n, kind, ch};
    vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pm(cb, "ew", "", 0, 0, 0, (n + 255) / 256, 1, 1);
}

void ChainRecorder::dPart(VkCommandBuffer cb, uint64_t a, uint64_t c2, const Stage& s, uint32_t C,
                          uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
    uint32_t hp8 = (s.H + padTop + 7) / 8;
    PartPush p{a, c2, s.H, s.W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
    vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
    pm(cb, "part", "", 0, C, 0, hp8 * wp8 * 64, 1, 1);
}

void ChainRecorder::dGather(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c2,
                            uint64_t h, const Stage& s, uint32_t C, uint32_t padTop, uint32_t padLeft,
                            uint32_t wp8, bool pub, bool b16) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGather.pipe);
    GatherPush p{a, b, d, c2, h, s.H, s.W, C, padTop, padLeft, wp8,
                 (pub ? 1u : 0u) | (b16 ? 2u : 0u)};
    vkCmdPushConstants(cb, pGather.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    // M10: warp-per-token mapping. C in {32,64,128,256,512}: S=C/4 vec4 slots
    // per token; S>=32 -> one token per workgroup, else 128/C tokens per
    // workgroup (lane -> (token, slot), fully coalesced across the warp).
    uint32_t ttw = (C >= 128u) ? 1u : (128u / C);   // tokens per workgroup
    uint32_t grid = (s.TOK + ttw - 1) / ttw;
    vkCmdDispatch(cb, grid, 1, 1);
    pm(cb, "gather", "", 0, C, 0, grid, 1, 1);
}

void ChainRecorder::dTrans(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pTrans.pipe);
    TransWePush p{a, c2, Ww, H};
    vkCmdPushConstants(cb, pTrans.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, Ww * 64, 1, 1);
    pm(cb, "trans", "", 0, Ww, 0, Ww * 64, 1, 1);
}

void ChainRecorder::dCosW(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc,
                          uint32_t Ww, uint32_t H, uint32_t C, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCosW.pipe);
    CosWinPush p{a, c2, sc, Ww * 64 * H, kind, C, H};
    vkCmdPushConstants(cb, pCosW.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (Ww * 64 * H + 31) / 32, 1, 1);
    pm(cb, "cosw", "", 0, C, 0, (Ww * 64 * H + 31) / 32, 1, 1);
}

void ChainRecorder::dCosG(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
    CosPush p{a, c2, sc, T6P_ * 32, kind};
    vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (T6P_ * 32 + 31) / 32, 1, 1);
    pm(cb, "cosg", "", 0, 0, 0, (T6P_ * 32 + 31) / 32, 1, 1);
}

void ChainRecorder::dSmaxW(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H,
                           uint64_t bias) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
    SMaxPush p{a, c2, Ww * H * 64, 64, 0.0f, bias, H, 0};
    vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (Ww * H * 64 + 31) / 32, 1, 1);
    pm(cb, "smaxw", "", 0, 64, 0, (Ww * H * 64 + 31) / 32, 1, 1);
}

void ChainRecorder::dSmaxG(VkCommandBuffer cb, uint64_t a, uint64_t c2) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
    SMaxPush p{a, c2, 32 * T6P_, T6P_, 3.0f, 0, 0, st_[6].TOK};
    vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, 32 * T6P_, 1, 1);
    pm(cb, "smaxg", "", 0, T6P_, 0, 32 * T6P_, 1, 1);
}

void ChainRecorder::dMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                           uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pMerge.pipe);
    MergePush p{a, b, c2, d, e, h, n, ch, kind};
    vkCmdPushConstants(cb, pMerge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pm(cb, "merge", "", 0, ch, 0, (n + 255) / 256, 1, 1);
}

namespace {
struct PoolPush { uint64_t a, c; uint32_t Hin, Win, Wpad, C, n, flags; };
struct UpMergePush {
    uint64_t a, b, c, d, e, h;
    uint32_t Wt, Ht, Wl, C, kind;
};
} // namespace

void ChainRecorder::dPool(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Hin, uint32_t Win,
                          uint32_t Wpad, uint32_t Hpad, uint32_t C, uint32_t flags) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPool.pipe);
    uint32_t n = (Hpad / 2) * (Wpad / 2);
    PoolPush p{a, c2, Hin, Win, Wpad, C, n, flags};
    vkCmdPushConstants(cb, pPool.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pm(cb, "pool", "", 0, C, 0, (n + 255) / 256, 1, 1);
}

void ChainRecorder::dUpMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t sin,
                             uint64_t cos, uint64_t h, const Stage& tgt, const Stage& low,
                             uint32_t C, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pUpM.pipe);
    UpMergePush p{a, b, c2, sin, cos, h, tgt.W, tgt.H, low.W, C, kind};
    vkCmdPushConstants(cb, pUpM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, (tgt.TOK + 255) / 256, 1, 1);
    pm(cb, "upm", "", 0, C, 0, (tgt.TOK + 255) / 256, 1, 1);
}

} // namespace d5c
