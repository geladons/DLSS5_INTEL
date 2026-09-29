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
    chainMakePipe(c, shaderDir + "gemm_i8.spv", pGemmI8);
    chainMakePipe(c, shaderDir + "quant_act.spv", pQuantAct);
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
    std::printf("[chain] 16 pipelines up (12 kernels + featpack/headpack + pool2/upmerge), %zu BDA chunk bases, chunk0 0x%llx\n",
                arena.chunks.size(), (unsigned long long)arena.chunks[0].baseA);
}

void ChainRecorder::destroy(const VkCtx& c) {
    for (ChainPipe* cp : {&pGemm, &pGemm1, &pGemmI8, &pQuantAct, &pCos, &pCosW, &pSmax, &pEw, &pPart, &pTrans,
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
                          uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc,
                          uint64_t res, uint64_t skip) {
    (void)fam;
    constexpr uint32_t GEMM_BM = 32; // TM=8 * RM=4 (optimal Battlemage occupancy sweet spot)
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
    GemmPush p{a, b, c2, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc, 0, res, skip};
    vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint32_t epi = (flags >> 8) & 0xFu;
    bool res_is_16 = (epi == EPI_RESIDUAL_F16);
    uint64_t bcount = (batch == 0) ? 1 : (uint64_t)batch;
    uint64_t bytes = bcount * (
        (uint64_t)m * k * 2 +
        (uint64_t)k * n * 2 +
        (uint64_t)m * n * ((flags & 0x1000) ? 2 : 4) +
        (res != 0 ? ((uint64_t)m * n * (res_is_16 ? 2 : 4)) : 0)
    );

    pmBegin(cb);
    vkCmdDispatch(cb, n / 32, (m + GEMM_BM - 1) / GEMM_BM, batch);
    pmEnd(cb, "gemm", "blk", m, n, k, n / 32, (m + GEMM_BM - 1) / GEMM_BM, batch, bytes);
}

void ChainRecorder::dGemmI8(VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
                            uint64_t b_scale, uint64_t a_scale, float scale_a,
                            uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                            uint32_t sa, uint32_t sb, uint32_t sc,
                            uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
    (void)fam;
    constexpr uint32_t GEMM_BM = 32;
    constexpr uint32_t GEMM_BN = 32;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemmI8.pipe);
    GemmI8Push p{a, b, c2, b_scale, a_scale, scale_a, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc, 0};
    vkCmdPushConstants(cb, pGemmI8.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint64_t bcount = (batch == 0) ? 1 : (uint64_t)batch;
    // Memory traffic: int8 input A (M*K*1), int8 weights B (K*N*1), float weights scale (N*4),
    // float act scale ((M+31)/32 * 4 if dynamic), fp16 output C (M*N*2)
    uint64_t actScaleBytes = (a_scale != 0) ? (((uint64_t)m + 31) / 32 * 4) : 0;
    uint64_t bytes = bcount * (
        (uint64_t)m * k * 1 +
        (uint64_t)k * n * 1 +
        (uint64_t)n * 4 +
        actScaleBytes +
        (uint64_t)m * n * 2
    );

    pmBegin(cb);
    vkCmdDispatch(cb, (n + GEMM_BN - 1) / GEMM_BN, (m + GEMM_BM - 1) / GEMM_BM, batch);
    pmEnd(cb, "gemm_i8", "blk", m, n, k, (n + GEMM_BN - 1) / GEMM_BN, (m + GEMM_BM - 1) / GEMM_BM, batch, bytes);
}

void ChainRecorder::dQuantAct(VkCommandBuffer cb, uint64_t a, uint64_t aq, uint64_t scales,
                              uint32_t m, uint32_t k) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pQuantAct.pipe);
    QuantActPush p{a, aq, scales, m, k};
    vkCmdPushConstants(cb, pQuantAct.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint32_t numBlocks = (m + 31) / 32;
    // Traffic: read FP16 A (M*K*2), write INT8 Aq (M*K*1), write scales (numBlocks*4)
    uint64_t bytes = (uint64_t)m * k * 2 + (uint64_t)m * k * 1 + (uint64_t)numBlocks * 4;

    pmBegin(cb);
    vkCmdDispatch(cb, numBlocks, 1, 1);
    pmEnd(cb, "qact", "", m, k, 0, numBlocks, 1, 1, bytes);
}

void ChainRecorder::dEw(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                        uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
    EWPush p{a, b, c2, d, h, n, kind, ch};
    vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint64_t bytes = 0;
    if (kind == 2 || kind == 3) {
        bytes = (uint64_t)n * 6; // read fp32 (4) + write f16 (2)
    } else if (kind == 6) {
        bytes = (uint64_t)n * 4; // read f16 (2) + write f16 (2)
    } else if (kind == 0) {
        bytes = (uint64_t)n * 14 + (uint64_t)ch * 4; // read a(4)+b(4)+d(4) + write c(4)+h(2)
    } else if (kind == 1) {
        bytes = (uint64_t)n * 12 + (uint64_t)ch * 4; // read a(4)+b(4)+d(4) + write c(4)
    } else if (kind == 4) {
        bytes = (uint64_t)n * 10 + (uint64_t)ch * 4; // read a(4)+b(2)+d(4) + write c(4)
    } else if (kind == 5) {
        bytes = (uint64_t)n * 12 + (uint64_t)ch * 4; // read a(4)+b(2)+d(4) + write c(4)+h(2)
    }

    pmBegin(cb);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pmEnd(cb, "ew", "", 0, 0, 0, (n + 255) / 256, 1, 1, bytes);
}

void ChainRecorder::dPart(VkCommandBuffer cb, uint64_t a, uint64_t c2, const Stage& s, uint32_t C,
                          uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
    uint32_t hp8 = (s.H + padTop + 7) / 8;
    PartPush p{a, c2, s.H, s.W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
    vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint64_t bytes = (uint64_t)s.H * s.W * C * (in16 ? 2 : 4) +
                     (uint64_t)hp8 * wp8 * 64 * C * 2;

    pmBegin(cb);
    vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
    pmEnd(cb, "part", "", 0, C, 0, hp8 * wp8 * 64, 1, 1, bytes);
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

    uint64_t bytes = (uint64_t)s.TOK * C * (4 + (b16 ? 2 : 4) + 4 + (pub ? 2 : 0)) +
                     (uint64_t)C * 4;

    pmBegin(cb);
    vkCmdDispatch(cb, grid, 1, 1);
    pmEnd(cb, "gather", "", 0, C, 0, grid, 1, 1, bytes);
}

void ChainRecorder::dTrans(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pTrans.pipe);
    TransWePush p{a, c2, Ww, H};
    vkCmdPushConstants(cb, pTrans.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint32_t C = H * 32;
    uint64_t bytes = (uint64_t)Ww * 64 * C * 6; // read fp32 [W,H,64,32] (4) + write f16 [W,64,C] (2)

    pmBegin(cb);
    vkCmdDispatch(cb, Ww * 64, 1, 1);
    pmEnd(cb, "trans", "", 0, Ww, 0, Ww * 64, 1, 1, bytes);
}

void ChainRecorder::dCosW(VkCommandBuffer cb, uint64_t a, uint64_t q, uint64_t k, uint64_t v,
                          uint64_t sc, uint32_t Ww, uint32_t H, uint32_t C) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCosW.pipe);
    CosWinPush p{a, q, k, v, sc, Ww * 64 * H, C, H, 0};
    vkCmdPushConstants(cb, pCosW.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    // read proj fp32 [W*64, 3C] (12*C) + write Q/K/V f16 [W*64, C] each (3 * 2*C = 6*C) + read sc (H*4)
    uint64_t bytes = (uint64_t)Ww * 64 * C * 18 + (uint64_t)H * 4;

    pmBegin(cb);
    vkCmdDispatch(cb, (Ww * 64 * H + 31) / 32, 1, 1);
    pmEnd(cb, "cosw", "", 0, C, 0, (Ww * 64 * H + 31) / 32, 1, 1, bytes);
}

void ChainRecorder::dCosG(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
    CosPush p{a, c2, sc, T6P_ * 32, kind};
    vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    // vector count = T6P_ * 32, each 32 dims. Read 32 floats (128B) + write 32 f16 (64B) = 192B.
    uint64_t bytes = (uint64_t)T6P_ * 32 * 192;

    pmBegin(cb);
    vkCmdDispatch(cb, (T6P_ * 32 + 31) / 32, 1, 1);
    pmEnd(cb, "cosg", "", 0, 0, 0, (T6P_ * 32 + 31) / 32, 1, 1, bytes);
}

void ChainRecorder::dSmaxW(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H,
                           uint64_t bias) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
    SMaxPush p{a, c2, Ww * H * 64, 64, 0.0f, bias, H, 0};
    vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    // m = Ww * H * 64 rows, n = 64 cols. Read fp32 (4) + write f16 (2) + optional bias f16 (2)
    uint64_t bytes = (uint64_t)Ww * H * 64 * 64 * (bias != 0 ? 8 : 6);

    pmBegin(cb);
    vkCmdDispatch(cb, (Ww * H * 64 + 31) / 32, 1, 1);
    pmEnd(cb, "smaxw", "", 0, 64, 0, (Ww * H * 64 + 31) / 32, 1, 1, bytes);
}

void ChainRecorder::dSmaxG(VkCommandBuffer cb, uint64_t a, uint64_t c2) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
    SMaxPush p{a, c2, 32 * T6P_, T6P_, 3.0f, 0, 0, st_[6].TOK};
    vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    // m = 32 * T6P_, n = T6P_. Read fp32 (4) + write f16 (2)
    uint64_t bytes = (uint64_t)32 * T6P_ * T6P_ * 6;

    pmBegin(cb);
    vkCmdDispatch(cb, 32 * T6P_, 1, 1);
    pmEnd(cb, "smaxg", "", 0, T6P_, 0, 32 * T6P_, 1, 1, bytes);
}

void ChainRecorder::dMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                           uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pMerge.pipe);
    MergePush p{a, b, c2, d, e, h, n, ch, kind};
    vkCmdPushConstants(cb, pMerge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);

    uint64_t bytes = (kind == 0)
        ? ((uint64_t)n * 12 + (uint64_t)ch * 4)   // read a(4)+b(2)+d(4) + write c(4)+h(2)
        : ((uint64_t)n * 8 + (uint64_t)ch * 8);    // read a(2)+b(2)+d(4)+e(4) + write c(4)

    pmBegin(cb);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pmEnd(cb, "merge", "", 0, ch, 0, (n + 255) / 256, 1, 1, bytes);
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

    uint64_t bytes = (uint64_t)Hin * Win * C * ((flags & 1) ? 2 : 4) +
                     (uint64_t)n * C * 2;

    pmBegin(cb);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    pmEnd(cb, "pool", "", 0, C, 0, (n + 255) / 256, 1, 1, bytes);
}

void ChainRecorder::dUpMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t sin,
                             uint64_t cos, uint64_t h, const Stage& tgt, const Stage& low,
                             uint32_t C, uint32_t kind) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pUpM.pipe);
    UpMergePush p{a, b, c2, sin, cos, h, tgt.W, tgt.H, low.W, C, kind};
    vkCmdPushConstants(cb, pUpM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    (void)low;

    uint64_t bytes = (kind == 0)
        ? ((uint64_t)tgt.TOK * C * 12 + (uint64_t)C * 4) // read a(4)+b(2)+sin(4) + write c(4)+h(2)
        : ((uint64_t)tgt.TOK * C * 8 + (uint64_t)C * 8);  // read a(2)+b(2)+sin(4)+cos(4) + write c(4)

    pmBegin(cb);
    vkCmdDispatch(cb, (tgt.TOK + 255) / 256, 1, 1);
    pmEnd(cb, "upm", "", 0, C, 0, (tgt.TOK + 255) / 256, 1, 1, bytes);
}

} // namespace d5c
