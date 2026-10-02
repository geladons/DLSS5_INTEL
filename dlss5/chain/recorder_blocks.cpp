// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain — ChainRecorder: block recorders + the 71-block U-Net walk.
// VERBATIM port of the m8b-live lambdas (recordWindowAttn/recordWindowBlock/
// recordDs/recordUp/recordGlobal/recordChain) to member methods.
// ============================================================================
#include "recorder.h"

namespace d5c {

static void winGeo(const Stage& s, int oy, int ox,
                   uint32_t& padTop, uint32_t& padLeft, uint32_t& Ww, uint32_t& wp8) {
    padTop = (uint32_t)(-oy);
    padLeft = (uint32_t)(-ox);
    uint32_t hp8 = (s.H + padTop + 7) / 8;
    wp8 = (s.W + padLeft + 7) / 8;
    Ww = hp8 * wp8;
}


// ---- window attention shared tail (per-stage geometry) -----------------
void ChainRecorder::recordWindowAttn(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                                     int C, int H, int fam, int oy, int ox,
                                     VkDeviceSize ffnOff, bool ffn16,
                                     VkDeviceSize qkvOff, bool qkv16,
                                     VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish) {
    uint32_t padTop, padLeft, Ww, wp8;
    winGeo(s, oy, ox, padTop, padLeft, Ww, wp8);
    auto lay = [&](const char* fmt) { return tn(fmt, idx); };
    std::string qkvN = lay(fam == 2 ? "block%d.layer2.qkv_weight" : "block%d.layer0.qkv_weight");
    std::string scN = lay(fam == 2 ? "block%d.layer2.attn_scale" : "block%d.layer0.attn_scale");
    std::string biasN = lay(fam == 2 ? "block%d.layer2.attn_bias" : "block%d.layer0.attn_bias");
    std::string projN = lay(fam == 2 ? "block%d.layer3.projection_weight" : "block%d.layer0.projection_weight");
    std::string acosN = lay(fam == 2 ? "block%d.layer3.attn_cos_skip" : "block%d.layer0.attn_cos_skip");

    int pilotQKV = parsePilot("D5C_INT8_ATTN_QKV", "D5C_INT8_ATTN", -1);
    int pilotProj = parsePilot("D5C_INT8_ATTN_PROJ", "D5C_INT8_ATTN", -1);

    bool useI8_qkv = isQuantized(qkvN) && (pilotQKV >= 0 && idx <= pilotQKV);
    bool useI8_proj = isQuantized(projN) && (pilotProj >= 0 && idx <= pilotProj);

    // B4: partition is a pure gather (image-order ffn carrier -> window-order
    // f16 oWIN). F_APART replays partition's inverse window map inside the qkv
    // GEMM's A-operand tile load, so the oWIN write + qkv-side re-read DRAM
    // round-trip and the standalone dispatch+barrier disappear. The A source
    // is always f16: fam1's carrier oG2 already is, and fam0/fam2's fp32
    // carriers are duplicated to a dead slot as f16 by the residual epilogue
    // (F_DUP16) - interior windows then take a plain strided coopMatLoad and
    // only boundary windows stage through aStage. The int8 qkv path still
    // needs materialized oWIN for quant_act - keep dPart there.
    // D5C_FUSE_PART=0 restores the split path.
    bool apartQkv = qkvApart(fam, idx);
    if (!apartQkv) {
        dPart(cb, A(ffnOff), A(ar_->off.oWIN), s, (uint32_t)C, padTop, padLeft, wp8, ffn16);
        bar(cb);
    }
    // M12: EPI_COSQKV folds the cosine_win publish into the qkv GEMM epilogue
    // (BN=32 == one head_dim group per N tile), removing the oPROJW fp32 DRAM
    // round-trip plus one dispatch + barrier per block. With epi==COSQKV the
    // gemm's c/res/skip fields carry the oQW/oKW/oVW f16 bases and cosd carries
    // the per-head attn_scale fp32 table. D5C_FUSE_COSW=0 restores the split
    // gemm->oPROJW->cosw path for bisecting.
    int fuseCoswEnv = parsePilot("D5C_FUSE_COSW", nullptr, 1);
    bool fuseCosw = (fuseCoswEnv > 0);
    uint32_t qkvEpi = fuseCosw ? EPI_COSQKV : EPI_NONE;
    uint64_t qkvC  = fuseCosw ? A(ar_->off.oQW)  : A(ar_->off.oPROJW);
    uint64_t qkvK  = fuseCosw ? A(ar_->off.oKW)  : 0;
    uint64_t qkvV  = fuseCosw ? A(ar_->off.oVW)  : 0;
    uint64_t qkvSc = fuseCosw ? A(woff(scN))     : 0;
    if (useI8_qkv) {
        dQuantAct(cb, A(ar_->off.oWIN), A(ar_->off.oQAct), A(ar_->off.oQScale), Ww * 64, (uint32_t)C);
        bar(cb);
        dGemmI8(cb, famIdx, A(ar_->off.oQAct), A(woff(qkvN)), qkvC,
                A(wscale(qkvN)), A(ar_->off.oQScale), 0.0f,
                64, 3 * (uint32_t)C, (uint32_t)C, Ww,
                64 * (uint32_t)C, 0, 64 * 3 * (uint32_t)C,
                gflags(qkvEpi, false), (uint32_t)C, 3 * (uint32_t)C, 3 * (uint32_t)C,
                qkvK, qkvV, 0, qkvSc);
    } else if (apartQkv) {
        // F_APART: A = image-order qkvOff (always f16 - fam1's oG2 carrier or
        // the F_DUP16 duplicate for fp32 carriers); sa is ignored - batch z IS
        // the window index. gGeo0/gGeo1 carry the same packed window geometry
        // as EPI_GATHER_RES (gfl bit0 here means "A source is f16"). lda is
        // unused by the fused load. ffnOff still feeds the gather_res
        // residual below, unchanged.
        uint32_t pGeo0 = (uint32_t)s.H | ((uint32_t)s.W << 16);
        uint32_t pGeo1 = padTop | (padLeft << 6) | ((qkv16 ? 1u : 0u) << 12) | (wp8 << 16);
        dGemm(cb, famIdx, A(qkvOff), A(woff(qkvN)), qkvC, 64, 3 * C, C, Ww,
              0, 0, 64 * 3 * C, gflags(qkvEpi, false) | F_APART, C, 3 * C, 3 * C,
              qkvK, qkvV, qkvSc, pGeo0, pGeo1);
    } else {
        dGemm(cb, famIdx, A(ar_->off.oWIN), A(woff(qkvN)), qkvC, 64, 3 * C, C, Ww,
              64 * C, 0, 64 * 3 * C, gflags(qkvEpi, false), C, 3 * C, 3 * C,
              qkvK, qkvV, qkvSc);
    }
    bar(cb);
    if (!fuseCosw) {
        // M10: fused Q/K/V dispatch reads oPROJW once and writes oQW, oKW, oVW.
        dCosW(cb, A(ar_->off.oPROJW), A(ar_->off.oQW), A(ar_->off.oKW), A(ar_->off.oVW),
              A(woff(scN)), Ww, H, C);
        bar(cb);
    }

    // --- Fused or unfused attention core (QK^T + Softmax + PV) ---
    int fusedAttn = parsePilot("D5C_INT8_ATTN_FUSED", nullptr, 1);
    bool useFused = (fusedAttn >= 0 && (fusedAttn == 0 ? false : true));

    // M13: transpose_we is pure data movement (oMGW fp32 window/head-major ->
    // oATW e4m3-f16 token-major). The fused attn kernel stores its widened fp32
    // O tile through shared memory and emits the exact e4m3 f16 token-major
    // layout transpose_we would produce, so the standalone pass is skipped.
    // D5C_FUSE_TRANS=0 restores the split oMGW -> dTrans path for bisecting.
    int fuseTransEnv = parsePilot("D5C_FUSE_TRANS", nullptr, 1);
    bool fuseTrans = useFused && (fuseTransEnv > 0);

    if (useFused) {
        // Flash-Attention fusion: 1 dispatch replaces 3 (GEMM QK^T + smaxw + GEMM PV).
        // Score (oSCW) and probability (oPRW) matrices stay on-chip in shared memory (20 KB).
        // Zero DRAM traffic for intermediate scores/probs.
        dAttnFused(cb, A(ar_->off.oQW), A(ar_->off.oKW), A(ar_->off.oVW),
                   A(woff(biasN)), fuseTrans ? A(ar_->off.oATW) : A(ar_->off.oMGW),
                   Ww, H, fuseTrans);
    } else {
        dGemm(cb, famIdx, A(ar_->off.oQW), A(ar_->off.oKW), A(ar_->off.oSCW), 64, 64, 32, Ww * H,
              64 * 32, 64 * 32, 64 * 64, gflags(EPI_NONE, false) | F_TRANSPOSE, 32, 32, 64);
        bar(cb);
        dSmaxW(cb, A(ar_->off.oSCW), A(ar_->off.oPRW), Ww, H, A(woff(biasN)));
        bar(cb);
        dGemm(cb, famIdx, A(ar_->off.oPRW), A(ar_->off.oVW), A(ar_->off.oMGW), 64, 32, 64, Ww * H,
              64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
    }
    bar(cb);
    if (!fuseTrans) {
        dTrans(cb, A(ar_->off.oMGW), A(ar_->off.oATW), Ww, H);
        bar(cb);
    }
    // M13: gather_residual is pure data movement too (oABW window-order ->
    // oRAW image-order + ffn_residual*attn_cos_skip + optional e4m3 publish).
    // EPI_GATHER_RES replays the gather's inverse window->image map inside the
    // proj GEMM epilogue and writes oRAW (+e4m3 pub) directly, skipping the
    // oABW round-trip. gGeo* pack the geometry pushes; D5C_FUSE_GATHER=0
    // restores the oABW -> dGather path for bisecting.
    int fuseGatherEnv = parsePilot("D5C_FUSE_GATHER", nullptr, 1);
    bool fuseGather = (fuseGatherEnv > 0);
    // A2: publish blocks' fp32 oRAW is dead traffic - only the publish=false
    // blocks' oRAW feeds the ds-pool / b70 head path. gfl bit2 tells the
    // EPI_GATHER_RES epilogue to store ONLY the e4m3 output when publishing.
    // D5C_SKIP_RAW=0 restores the fp32 store.
    int skipRawEnv = parsePilot("D5C_SKIP_RAW", nullptr, 1);
    uint32_t gfl = (publish ? 1u : 0u) | (ffn16 ? 2u : 0u)
                 | ((publish && skipRawEnv > 0) ? 4u : 0u);
    uint32_t gGeo0 = (uint32_t)s.H | ((uint32_t)s.W << 16);
    uint32_t gGeo1 = padTop | (padLeft << 6) | (gfl << 12) | (wp8 << 16);
    uint32_t projEpi = fuseGather ? EPI_GATHER_RES : EPI_NONE;
    uint64_t projC = fuseGather ? A(rawOff) : A(ar_->off.oABW);
    uint64_t gRes = fuseGather ? A(ffnOff) : 0;
    uint64_t gSkip = fuseGather ? A(vec(acosN)) : 0;
    uint64_t gPub = (fuseGather && publish) ? A(pubOff) : 0;
    if (useI8_proj) {
        dQuantAct(cb, A(ar_->off.oATW), A(ar_->off.oQAct), A(ar_->off.oQScale), Ww * 64, (uint32_t)C);
        bar(cb);
        dGemmI8(cb, famIdx, A(ar_->off.oQAct), A(woff(projN)), projC,
                A(wscale(projN)), A(ar_->off.oQScale), 0.0f,
                64, (uint32_t)C, (uint32_t)C, Ww,
                64 * (uint32_t)C, 0, 64 * (uint32_t)C,
                gflags(projEpi, false), (uint32_t)C, (uint32_t)C, (uint32_t)C,
                gRes, gSkip, 0, gPub, gGeo0, gGeo1);
    } else {
        dGemm(cb, famIdx, A(ar_->off.oATW), A(woff(projN)), projC, 64, C, C, Ww,
              64 * C, 0, 64 * C, gflags(projEpi, false), C, C, C,
              gRes, gSkip, gPub, gGeo0, gGeo1);
    }
    bar(cb);
    if (!fuseGather) {
        dGather(cb, A(ar_->off.oABW), A(ffnOff), A(vec(acosN)), A(rawOff), A(publish ? pubOff : rawOff),
                s, (uint32_t)C, padTop, padLeft, wp8, publish, ffn16);
        bar(cb);
    }
}

// FFN + attention for one window block; writes raw (always) + pub (if publish).
void ChainRecorder::recordWindowBlock(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                                      int C, int H, int fam, int oy, int ox, bool publish, Cur& cur,
                                      VkDeviceSize rawOff, VkDeviceSize pubOff) {
    const ArenaOffsets& O = ar_->off;
    uint32_t TOKS = s.TOK;
    uint32_t G = C / 32;
    VkDeviceSize x16 = cur.is16 ? cur.off : O.oG2;
    if (!cur.is16) { dEw(cb, A(cur.off), 0, 0, 0, A(O.oG2), TOKS * C, 2, 0); bar(cb); }
    if (fam == 0) {
        int pilotW1 = parsePilot("D5C_INT8_W1", "D5C_INT8_PILOT", 70);
        int pilotW2 = parsePilot("D5C_INT8_W2", "D5C_INT8_PILOT", 70);

        const char* envFusion = std::getenv("D5C_INT8_FUSION");
        bool enableFusion = !envFusion || (std::strcmp(envFusion, "0") != 0 && std::strcmp(envFusion, "off") != 0);

        std::string w1 = tn("block%d.layer0.weight1", idx);
        bool useI8_w1 = isQuantized(w1) && (pilotW1 >= 0 && idx <= pilotW1);

        std::string w2 = tn("block%d.layer0.weight2", idx);
        bool useI8_w2 = isQuantized(w2) && (pilotW2 >= 0 && idx <= pilotW2);
        uint32_t epi = cur.is16 ? EPI_RESIDUAL_F16 : EPI_RESIDUAL_FP32;

        bool doFusion = useI8_w1 && useI8_w2 && enableFusion;

        // B4: the fused qkv A-load is cheapest on an f16 source (interior
        // windows become a plain strided coopMatLoad). The fp32 carrier oG4
        // is therefore duplicated to oG2 as f16 by the w2 epilogue
        // (F_DUP16, identical half_round bits to partition's fp32->f16 pass);
        // oG2 is dead after w1 consumed it. Only when the partition fusion
        // actually runs - the int8 qkv path still partitions oG4.
        bool apart = qkvApart(fam, idx);
        uint64_t dup = apart ? A(O.oG2) : 0;
        uint32_t dupFl = apart ? F_DUP16 : 0u;

        // ffn_fused (ffn_fused.comp): one 128-lane workgroup (4 subgroups)
        // per 32-token tile runs the whole i8 FFN - input quant + w1 expand
        // + w2 project with residual - with the hidden tile (oG3 i8 +
        // oQScale2) kept in SLM.
        // Bit-exact vs the trio it replaces (same coopmat tiles, same dequant
        // and scale math; quant_act's scale window IS the tile). Eliminates
        // the oQAct/oG3/oQScale/oQScale2 DRAM round-trips + 2 barriers.
        // D5C_FFN_FUSED=0 restores the 3-dispatch path for bisecting.
        int ffnFusedEnv = parsePilot("D5C_FFN_FUSED", nullptr, 1);
        if (doFusion && ffnFusedEnv > 0 && C == 32) {
            uint32_t ffl = ((epi == EPI_RESIDUAL_F16) ? 1u : 0u) | (dupFl != 0u ? 2u : 0u);
            dFfnFused(cb, famIdx, A(x16), A(woff(w1)), A(woff(w2)),
                      A(wscale(w1)), A(wscale(w2)), A(O.oG4),
                      A(cur.off), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))),
                      dup, TOKS, (uint32_t)C, ffl);
        } else if (doFusion) {
            dQuantAct(cb, A(x16), A(O.oQAct), A(O.oQScale), TOKS, (uint32_t)C);
            bar(cb);
            dGemmI8(cb, famIdx, A(O.oQAct), A(woff(w1)), A(O.oG3),
                    A(wscale(w1)), A(O.oQScale), 0.0f,
                    TOKS, 4 * C, C, 1, 0, 0, 0,
                    gflags(EPI_GATE_E4M3, false) | F_FUSED_I8_PRODUCER,
                    C, 4 * C, 4 * C, 0, 0, A(O.oQScale2));
            bar(cb);
            dGemmI8(cb, famIdx, A(O.oG3), A(woff(w2)), A(O.oG4),
                    A(wscale(w2)), A(O.oQScale2), 0.0f,
                    TOKS, (uint32_t)C, 4 * (uint32_t)C, 1, 0, 0, 0,
                    gflags(epi, false) | F_FUSED_I8_CONSUMER | dupFl,
                    4 * (uint32_t)C, (uint32_t)C, (uint32_t)C,
                    A(cur.off), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))),
                    0, dup);
        } else {
            if (useI8_w1) {
                dQuantAct(cb, A(x16), A(O.oQAct), A(O.oQScale), TOKS, (uint32_t)C);
                bar(cb);
                dGemmI8(cb, famIdx, A(O.oQAct), A(woff(w1)), A(O.oG3),
                        A(wscale(w1)), A(O.oQScale), 0.0f,
                        TOKS, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
            } else {
                dGemm(cb, famIdx, A(x16), A(woff(w1)), A(O.oG3),
                      TOKS, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
            }
            bar(cb);

            if (useI8_w2) {
                dQuantAct(cb, A(O.oG3), A(O.oQAct), A(O.oQScale), TOKS, 4 * (uint32_t)C);
                bar(cb);
                dGemmI8(cb, famIdx, A(O.oQAct), A(woff(w2)), A(O.oG4),
                        A(wscale(w2)), A(O.oQScale), 0.0f,
                        TOKS, (uint32_t)C, 4 * (uint32_t)C, 1, 0, 0, 0,
                        gflags(epi, false) | dupFl, 4 * (uint32_t)C, (uint32_t)C, (uint32_t)C,
                        A(cur.off), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))),
                        0, dup);
            } else {
                dGemm(cb, famIdx, A(O.oG3), A(woff(w2)), A(O.oG4),
                      TOKS, C, 4 * C, 1, 0, 0, 0, gflags(epi, false) | dupFl, 4 * C, C, C,
                      A(cur.off), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))), dup);
            }
        }
        bar(cb);
        // ffnOff stays the true fp32 carrier oG4 (partition source when
        // unfused, gather_res residual always); qkvOff is the f16 dup oG2
        // the fused A-load reads.
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox,
                         O.oG4, false, apart ? O.oG2 : O.oG4, apart,
                         rawOff, pubOff, publish);
    } else if (fam == 1) {
        uint64_t expBase = A(woff(tn("block%d.layer0.ffn_expand_weight", idx)));
        uint64_t prjBase = A(woff(tn("block%d.layer0.ffn_branch_projection_weight", idx)));
        // B5: collapse the 2G serialized group dispatches into two batched
        // gemms (same pattern as fam2/4/5's group_expand/group_project). The
        // fuse-folded expand weight is already [G][C][128] contiguous (slice
        // stride C*128 elems -> sb) and branch_proj is [G][128][32] (128*32).
        // Expand writes G disjoint [TOKS,128] f16 slabs in oG3 (sc=TOKS*128);
        // proj reads them via sa and writes the 32-wide column slices of oG4
        // (sc=32, ldc=C) - identical addresses as the unrolled loop.
        // D5C_FUSE_FAM1=0 keeps the per-group loop for bisecting.
        int fuseFam1Env = parsePilot("D5C_FUSE_FAM1", nullptr, 1);
        if (fuseFam1Env > 0) {
            dGemm(cb, famIdx, A(x16), expBase, A(O.oG3),
                  TOKS, 128, C, G, 0, (uint32_t)C * 128, TOKS * 128,
                  gflags(EPI_GATE_E4M3, true), C, 128, 128);
            bar(cb);
            dGemm(cb, famIdx, A(O.oG3), prjBase, A(O.oG4),
                  TOKS, 32, 128, G, TOKS * 128, 128 * 32, 32,
                  gflags(EPI_E4M3, true), 128, 32, C);
            bar(cb);
        } else {
            for (uint32_t oh = 0; oh < G; ++oh) {
                dGemm(cb, famIdx, A(x16), expBase + (uint64_t)oh * C * 128 * 2, A(O.oG3),
                      TOKS, 128, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 128, 128);
                bar(cb);
                dGemm(cb, famIdx, A(O.oG3), prjBase + (uint64_t)oh * 128 * 32 * 2, A(O.oG4 + oh * 32 * 2),
                      TOKS, 32, 128, 1, 0, 0, 0, gflags(EPI_E4M3, true), 128, 32, C);
                bar(cb);
            }
        }
        dGemm(cb, famIdx, A(O.oG4), A(woff(tn("block%d.layer0.ffn_output_projection_weight", idx))),
              A(O.oG3), TOKS, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dEw(cb, A(O.oG3), A(cur.off), A(O.oG3), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))),
            A(O.oG2), TOKS * C, 5, C);
        bar(cb);
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox,
                         O.oG2, true, O.oG2, true, rawOff, pubOff, publish);
    } else {
        dGemm(cb, famIdx, A(x16), A(woff(tn("block%d.layer0.first_projection_weight", idx))),
              A(O.oG2), TOKS, C, C, 1, 0, 0, 0, gflags(EPI_E4M3, true), C, C, C);
        bar(cb);
        dGemm(cb, famIdx, A(O.oG2), A(woff(tn("block%d.layer0.group_expand_weight", idx))),
              A(O.oG3), TOKS, 256, 64, 8, 64, 64 * 256, TOKS * 256,
              gflags(EPI_GATE, true), C, 256, 256);
        bar(cb);
        dGemm(cb, famIdx, A(O.oG3), A(woff(tn("block%d.layer0.group_project_weight", idx))),
              A(O.oG4), TOKS, 64, 256, 8, TOKS * 256, 256 * 64, 64,
              gflags(EPI_NONE, false), 256, 64, C);
        bar(cb);
        dEw(cb, A(O.oG4), 0, 0, 0, A(O.oG2), TOKS * C, 3, 0);
        bar(cb);
        uint32_t epi = cur.is16 ? EPI_RESIDUAL_F16 : EPI_RESIDUAL_FP32;
        // B4: same trick as fam0 - duplicate the fp32 carrier oG3 into oG4
        // (dead after kind3 -> oG2) as f16 so the fused qkv A-load runs on
        // the f16 path. Skipped when the int8 qkv path needs oWIN instead.
        bool apart = qkvApart(fam, idx);
        dGemm(cb, famIdx, A(O.oG2), A(woff(tn("block%d.layer1.weight3", idx))), A(O.oG3),
              TOKS, C, C, 1, 0, 0, 0, gflags(epi, false) | (apart ? F_DUP16 : 0u), C, C, C,
              A(cur.off), A(vec(tn("block%d.layer1.ffn_cos_skip", idx))),
              apart ? A(O.oG4) : 0);
        bar(cb);
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox,
                         O.oG3, false, apart ? O.oG4 : O.oG3, apart,
                         rawOff, pubOff, publish);
    }
}

// downsample transition: avgpool2 (pad-to-8 optional) -> e4m3 -> gemm w0 -> e4m3
void ChainRecorder::recordDs(VkCommandBuffer cb, int famIdx, int idx, int C, int Cn,
                             VkDeviceSize rawOff, VkDeviceSize outOff,
                             const Stage& inS, const Stage& outS, bool pad8) {
    const ArenaOffsets& O = ar_->off;
    uint32_t Hp = pad8 ? pad8u(inS.H) : inS.H;
    uint32_t Wp = pad8 ? pad8u(inS.W) : inS.W;
    dPool(cb, A(rawOff), A(O.oPOOL), inS.H, inS.W, Wp, Hp, (uint32_t)C, 2u);
    bar(cb);
    dGemm(cb, famIdx, A(O.oPOOL), A(woff(tn("block%d.layer0.weight0", idx))), A(outOff),
          outS.TOK, (uint32_t)Cn, (uint32_t)C, 1, 0, 0, 0, gflags(EPI_E4M3, true),
          (uint32_t)C, (uint32_t)Cn, (uint32_t)Cn);
    bar(cb);
}

// upsample transition: gemm w0 -> nearest-x2 crop merge with skip*sin -> e4m3
void ChainRecorder::recordUp(VkCommandBuffer cb, int famIdx, int idx, int Ci, int Co,
                             Cur& cur, VkDeviceSize skipOff, VkDeviceSize out16Off,
                             const Stage& lowS, const Stage& tgtS) {
    const ArenaOffsets& O = ar_->off;
    dGemm(cb, famIdx, A(cur.off), A(woff(tn("block%d.layer0.weight0", idx))), A(O.oG3),
          lowS.TOK, (uint32_t)Co, (uint32_t)Ci, 1, 0, 0, 0, gflags(EPI_NONE, false),
          (uint32_t)Ci, (uint32_t)Co, (uint32_t)Co);
    bar(cb);
    dUpMerge(cb, A(O.oG3), A(skipOff), A(O.oG4), A(vec(tn("block%d.layer0.sin", idx))), 0,
             A(out16Off), tgtS, lowS, (uint32_t)Co, 0);
    bar(cb);
    cur = {out16Off, true, (uint32_t)Co};
}

// global block (L6 grid, input f16, padded to T6P rows)
void ChainRecorder::recordGlobal(VkCommandBuffer cb, int famIdx, int idx, Cur& cur,
                                 VkDeviceSize rawOff, VkDeviceSize pubOff) {
    const ArenaOffsets& O = ar_->off;
    uint32_t TOKG = T6P_;
    dGemm(cb, famIdx, A(cur.off), A(woff(tn("block%d.layer0.weight", idx))), A(O.oHG),
          TOKG, 4096, 1024, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), 1024, 4096, 4096);
    bar(cb);
    uint32_t epi = cur.is16 ? EPI_RESIDUAL_F16 : EPI_RESIDUAL_FP32;
    dGemm(cb, famIdx, A(O.oHG), A(woff(tn("block%d.layer1.weight", idx))), A(O.oG3),
          TOKG, 1024, 4096, 1, 0, 0, 0, gflags(epi, false), 4096, 1024, 1024,
          A(cur.off), A(vec(tn("block%d.layer1.ffn_cos_skip", idx))));
    bar(cb);
    dEw(cb, A(O.oG3), 0, 0, 0, A(O.oG2), TOKG * 1024, 2, 0);
    bar(cb);
    dGemm(cb, famIdx, A(O.oG2), A(woff(tn("block%d.layer2.qkv_weight", idx))), A(O.oPROJG),
          TOKG, 3072, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 3072, 3072);
    bar(cb);
    // M10: Q/K/V read the same oPROJG and write disjoint slots -> one bar.
    dCosG(cb, A(O.oPROJG), A(O.oQG), A(vec(tn("block%d.layer2.attn_scale.q32", idx))), 0);
    dCosG(cb, A(O.oPROJG), A(O.oKG), A(vec(tn("block%d.layer2.attn_scale.q32", idx))), 1);
    dCosG(cb, A(O.oPROJG), A(O.oVG), A(vec(tn("block%d.layer2.attn_scale.q32", idx))), 2);
    bar(cb);
    dGemm(cb, famIdx, A(O.oQG), A(O.oKG), A(O.oSCG), TOKG, TOKG, 32, 32,
          32, 32, TOKG * TOKG, gflags(EPI_NONE, false) | F_TRANSPOSE, 1024, 1024, TOKG);
    bar(cb);
    dSmaxG(cb, A(O.oSCG), A(O.oPRG));
    bar(cb);
    dGemm(cb, famIdx, A(O.oPRG), A(O.oVG), A(O.oMGG), TOKG, 32, TOKG, 32,
          TOKG * TOKG, 32, 32, gflags(EPI_NONE, false), TOKG, 1024, 1024);
    bar(cb);
    dEw(cb, A(O.oMGG), 0, 0, 0, A(O.oATG), TOKG * 1024, 3, 0);
    bar(cb);
    dGemm(cb, famIdx, A(O.oATG), A(woff(tn("block%d.layer4.projection_weight", idx))), A(O.oABG),
          TOKG, 1024, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 1024, 1024);
    bar(cb);
    dEw(cb, A(O.oABG), A(O.oG3), A(O.oBRG), A(vec(tn("block%d.layer4.attn_cos_skip", idx))),
        A(pubOff), TOKG * 1024, 0, 1024);
    bar(cb);
    (void)rawOff;
}

std::pair<int, int> ChainRecorder::originOf(int blockIndex) {
    int phase;
    if (blockIndex == 0) phase = 0;
    else if (blockIndex >= 1 && blockIndex <= 4) phase = blockIndex - 1;
    else if (blockIndex >= 5 && blockIndex <= 8) phase = blockIndex - 5;
    else if (blockIndex >= 9 && blockIndex <= 14) phase = blockIndex - 9;
    else if (blockIndex >= 15 && blockIndex <= 22) phase = blockIndex - 15;
    else if (blockIndex >= 23 && blockIndex <= 30) phase = blockIndex - 23;
    else if (blockIndex >= 40 && blockIndex <= 55) phase = blockIndex - (blockIndex < 48 ? 40 : 48);
    else if (blockIndex >= 56 && blockIndex <= 61) phase = blockIndex - 54;
    else if (blockIndex >= 62 && blockIndex <= 69) phase = blockIndex - (blockIndex < 66 ? 62 : 66);
    else if (blockIndex == 70) phase = 1;
    else return {0, 0};
    const int dy[4] = {0, -4, 0, -4};
    const int dx[4] = {0, -4, -4, 0};
    return {dy[phase % 4], dx[phase % 4]};
}

// ---- the 71-block chain (M9 U-Net walk: per-scale extents, pool/up
//      transitions, padded global stage; validated vs torch reference) ----
void ChainRecorder::recordChain(VkCommandBuffer cb) {
    const ArenaOffsets& O = ar_->off;
    Cur cur{O.oADA, false, 32};

    // adapter GEMM at L0: x16 @ input_adapter_weight -> fp32
    dGemm(cb, 0, A(O.oX16), A(woff("block0.layer0.input_adapter_weight")), A(O.oADA),
          st_[0].TOK, 32, 16, 1, 0, 0, 0, gflags(EPI_NONE, false), 16, 32, 32);
    bar(cb);
    // b0 (plain, publish=false) at L0; frs = e4m3(raw); pool -> L1 e4m3
    {
        Cur c0 = cur;
        recordWindowBlock(cb, 0, 0, st_[0], 32, 1, 0, 0, 0, false, c0, O.oB0RAW, 0);
        cur = {O.oB0RAW, false, 32};
        dEw(cb, A(O.oB0RAW), 0, 0, 0, A(O.oFRS), st_[0].TOK * 32, 3, 0);
        bar(cb);
        dPool(cb, A(O.oB0RAW), A(O.oPOOL), st_[0].H, st_[0].W, st_[0].W, st_[0].H, 32, 2u);
        bar(cb);
        cur = {O.oPOOL, true, 32};
    }
    for (int i = 1; i <= 3; ++i) {
        VkDeviceSize out = (i == 3) ? O.oSKIP0 : O.oG1;
        auto o = originOf(i);
        recordWindowBlock(cb, 0, i, st_[1], 32, 1, 0, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 32};
    }
    {   // b4 window at L1 (publish=false) + ds to L2 C=64
        Cur c4 = cur;
        recordWindowBlock(cb, 0, 4, st_[1], 32, 1, 0, -4, 0, false, c4, O.oRAW, 0);
        recordDs(cb, 0, 4, 32, 64, O.oRAW, O.oB4DS, st_[1], st_[2], false);
        cur = {O.oB4DS, true, 64};
    }

    // enc L2: b5-7, b8 ds -> L3 C=128
    for (int i = 5; i <= 7; ++i) {
        VkDeviceSize out = (i == 7) ? O.oSKIP1 : O.oG1;
        auto o = originOf(i);
        recordWindowBlock(cb, 1, i, st_[2], 64, 2, 1, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 64};
    }
    {
        Cur ci = cur;
        auto o = originOf(8);
        recordWindowBlock(cb, 1, 8, st_[2], 64, 2, 1, o.first, o.second, false, ci, O.oRAW, 0);
        recordDs(cb, 1, 8, 64, 128, O.oRAW, O.oG1, st_[2], st_[3], false);
        cur = {O.oG1, true, 128};
    }
    // enc L3: b9-13, b14 ds -> L4 C=256
    for (int i = 9; i <= 13; ++i) {
        VkDeviceSize out = (i == 13) ? O.oSKIP2 : O.oG1;
        auto o = originOf(i);
        recordWindowBlock(cb, 1, i, st_[3], 128, 4, 1, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 128};
    }
    {
        Cur ci = cur;
        auto o = originOf(14);
        recordWindowBlock(cb, 1, 14, st_[3], 128, 4, 1, o.first, o.second, false, ci, O.oRAW, 0);
        recordDs(cb, 1, 14, 128, 256, O.oRAW, O.oG1, st_[3], st_[4], false);
        cur = {O.oG1, true, 256};
    }
    // enc L4: b15-21, b22 ds (pad8) -> L5 C=512
    for (int i = 15; i <= 21; ++i) {
        VkDeviceSize out = (i == 21) ? O.oSKIP3 : O.oG1;
        auto o = originOf(i);
        recordWindowBlock(cb, 1, i, st_[4], 256, 8, 1, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 256};
    }
    {
        Cur ci = cur;
        auto o = originOf(22);
        recordWindowBlock(cb, 1, 22, st_[4], 256, 8, 1, o.first, o.second, false, ci, O.oRAW, 0);
        recordDs(cb, 1, 22, 256, 512, O.oRAW, O.oB22DS, st_[4], st_[5], true);
        cur = {O.oB22DS, true, 512};
    }

    // bottleneck L5: b23-30 split window
    for (int i = 23; i <= 30; ++i) {
        VkDeviceSize out = (i == 30) ? O.oSS : O.oG1;
        auto o = originOf(i);
        recordWindowBlock(cb, 2, i, st_[5], 512, 16, 2, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 512};
    }
    {   // b30 bridge: pad8 + pool(f16) -> half -> gemm layer4.weight -> e4m3 at L6
        uint32_t Hp = pad8u(st_[5].H), Wp = pad8u(st_[5].W);
        dPool(cb, A(O.oSS), A(O.oPOOL), st_[5].H, st_[5].W, Wp, Hp, 512, 1u);
        bar(cb);
        dGemm(cb, 2, A(O.oPOOL), A(woff("block30.layer4.weight")), A(O.oBRIDGE),
              st_[6].TOK, 1024, 512, 1, 0, 0, 0, gflags(EPI_E4M3, true), 512, 1024, 1024);
        bar(cb);
        if (T6P_ > st_[6].TOK) {   // zero the padded global rows (fill + transfer barrier)
            vkCmdFillBuffer(cb, ar_->bufOf(O.oBRIDGE), ar_->localOff(O.oBRIDGE) + (uint64_t)st_[6].TOK * 1024 * 2,
                            (uint64_t)(T6P_ - st_[6].TOK) * 1024 * 2, 0);
            VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fb.buffer = ar_->bufOf(O.oBRIDGE);
            fb.offset = 0;
            fb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb, 0, nullptr);
        }
        cur = {O.oBRIDGE, true, 1024};
    }

    for (int i = 31; i <= 38; ++i) {
        VkDeviceSize out = (i == 38) ? O.oB38 : O.oG1;
        recordGlobal(cb, 3, i, cur, O.oBRG, out);
        cur = {out, true, 1024};
    }

    {   // b39: gemm conv_weight -> nearest-x2 crop merge with split_skip*sin -> e4m3
        dGemm(cb, 4, A(cur.off), A(woff("block39.layer0.conv_weight")), A(O.oG3),
              st_[6].TOK, 512, 1024, 1, 0, 0, 0, gflags(EPI_NONE, false), 1024, 512, 512);
        bar(cb);
        dUpMerge(cb, A(O.oG3), A(O.oSS), A(O.oMRG39), A(vec("block39.layer0.inp_upsample_sin")), 0,
                 A(O.oB39), st_[5], st_[6], 512, 0);
        bar(cb);
        cur = {O.oB39, true, 512};
    }
    for (int i = 40; i <= 47; ++i) {
        auto o = originOf(i);
        recordWindowBlock(cb, 4, i, st_[5], 512, 16, 2, o.first, o.second, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 512};
    }
    {   // b48 up-transition L5->L4 + window branched C=256 H=8
        recordUp(cb, 4, 48, 512, 256, cur, O.oSKIP3, O.oG2, st_[5], st_[4]);
        recordWindowBlock(cb, 4, 48, st_[4], 256, 8, 1, 0, 0, true, cur, O.oRAW, O.oB48);
        cur = {O.oB48, true, 256};
    }
    for (int i = 49; i <= 55; ++i) {
        auto o = originOf(i);
        recordWindowBlock(cb, 4, i, st_[4], 256, 8, 1, o.first, o.second, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 256};
    }
    {   // b56 up L4->L3 + window C=128 H=4
        recordUp(cb, 4, 56, 256, 128, cur, O.oSKIP2, O.oG2, st_[4], st_[3]);
        recordWindowBlock(cb, 4, 56, st_[3], 128, 4, 1, 0, -4, true, cur, O.oRAW, O.oG1); // originOf(56)=(0,-4)
        cur = {O.oG1, true, 128};
    }
    for (int i = 57; i <= 61; ++i) {
        auto o = originOf(i);
        recordWindowBlock(cb, 4, i, st_[3], 128, 4, 1, o.first, o.second, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 128};
    }
    {   // b62 up L3->L2 + window C=64 H=2
        recordUp(cb, 4, 62, 128, 64, cur, O.oSKIP1, O.oG2, st_[3], st_[2]);
        recordWindowBlock(cb, 4, 62, st_[2], 64, 2, 1, 0, 0, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 64};
    }
    for (int i = 63; i <= 65; ++i) {
        auto o = originOf(i);
        recordWindowBlock(cb, 4, i, st_[2], 64, 2, 1, o.first, o.second, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 64};
    }
    {   // b66 up L2->L1 + window plain C=32 H=1
        recordUp(cb, 4, 66, 64, 32, cur, O.oSKIP0, O.oG2, st_[2], st_[1]);
        recordWindowBlock(cb, 4, 66, st_[1], 32, 1, 0, 0, 0, true, cur, O.oRAW, O.oG1);
        cur = {O.oG1, true, 32};
    }
    for (int i = 67; i <= 69; ++i) {
        auto o = originOf(i);
        VkDeviceSize out = (i == 69) ? O.oB69 : O.oG1;
        recordWindowBlock(cb, 4, i, st_[1], 32, 1, 0, o.first, o.second, true, cur, O.oRAW, out);
        cur = {out, true, 32};
    }

    {   // b70: upsample L1->L0, merge up*sin + frs*cos (fp32) -> window plain -> head
        dUpMerge(cb, A(cur.off), A(O.oFRS), A(O.oMRG70), A(vec("block70.layer0.inp_merge_sin")),
                 A(vec("block70.layer0.inp_merge_cos")), 0, st_[0], st_[1], 32, 1);
        bar(cb);
        Cur c70{O.oMRG70, false, 32};
        recordWindowBlock(cb, 5, 70, st_[0], 32, 1, 0, -4, -4, false, c70, O.oRAW, 0);
        dEw(cb, A(O.oRAW), 0, 0, 0, A(O.oG2), st_[0].TOK * 32, 2, 0);
        bar(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm1.pipe);
        // Head output is compact [TOK,4] fp32 (F_CN stores only C cols 0..3,
        // ldc=4). The coopmat tile still computes all 16 columns with the
        // identical k-loop - only the store is truncated, values bit-exact.
        GemmPush p{A(O.oG2), A(O.oHeadW), A(O.oHEAD), st_[0].TOK, 16, 32, 1, 0, 0, 0,
                   gflags(EPI_NONE, false) | F_CN, 32, 16, 4, 4, 0, 0, 0};
        vkCmdPushConstants(cb, pGemm1.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        uint64_t gemm1_bytes = (uint64_t)st_[0].TOK * 32 * 2 + 32 * 16 * 2 + (uint64_t)st_[0].TOK * 4 * 4;
        pmBegin(cb);
        vkCmdDispatch(cb, 1, st_[0].TOK / 16, 1);
        pmEnd(cb, "gemm1", "head", st_[0].TOK, 16, 32, 1, st_[0].TOK / 16, 1, gemm1_bytes);
        bar(cb);
    }
}

} // namespace d5c
