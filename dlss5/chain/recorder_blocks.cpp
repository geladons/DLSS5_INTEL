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
                                     VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish) {
    uint32_t padTop, padLeft, Ww, wp8;
    winGeo(s, oy, ox, padTop, padLeft, Ww, wp8);
    auto lay = [&](const char* fmt) { return tn(fmt, idx); };
    std::string qkvN = lay(fam == 2 ? "block%d.layer2.qkv_weight" : "block%d.layer0.qkv_weight");
    std::string scN = lay(fam == 2 ? "block%d.layer2.attn_scale" : "block%d.layer0.attn_scale");
    std::string biasN = lay(fam == 2 ? "block%d.layer2.attn_bias" : "block%d.layer0.attn_bias");
    std::string projN = lay(fam == 2 ? "block%d.layer3.projection_weight" : "block%d.layer0.projection_weight");
    std::string acosN = lay(fam == 2 ? "block%d.layer3.attn_cos_skip" : "block%d.layer0.attn_cos_skip");

    dPart(cb, A(ffnOff), A(ar_->off.oWIN), s, (uint32_t)C, padTop, padLeft, wp8, ffn16);
    bar(cb);
    dGemm(cb, famIdx, A(ar_->off.oWIN), A(woff(qkvN)), A(ar_->off.oPROJW), 64, 3 * C, C, Ww,
          64 * C, 0, 64 * 3 * C, gflags(EPI_NONE, false), C, 3 * C, 3 * C);
    bar(cb);
    // M10: Q/K/V read the same oPROJW and write disjoint slots -> one bar.
    dCosW(cb, A(ar_->off.oPROJW), A(ar_->off.oQW), A(woff(scN)), Ww, H, C, 0);
    dCosW(cb, A(ar_->off.oPROJW), A(ar_->off.oKW), A(woff(scN)), Ww, H, C, 1);
    dCosW(cb, A(ar_->off.oPROJW), A(ar_->off.oVW), A(woff(scN)), Ww, H, C, 2);
    bar(cb);
    dGemm(cb, famIdx, A(ar_->off.oQW), A(ar_->off.oKW), A(ar_->off.oSCW), 64, 64, 32, Ww * H,
          64 * 32, 64 * 32, 64 * 64, gflags(EPI_NONE, false) | F_TRANSPOSE, 32, 32, 64);
    bar(cb);
    dSmaxW(cb, A(ar_->off.oSCW), A(ar_->off.oPRW), Ww, H, A(woff(biasN)));
    bar(cb);
    dGemm(cb, famIdx, A(ar_->off.oPRW), A(ar_->off.oVW), A(ar_->off.oMGW), 64, 32, 64, Ww * H,
          64 * 64, 64 * 32, 64 * 32, gflags(EPI_NONE, false), 64, 32, 32);
    bar(cb);
    dTrans(cb, A(ar_->off.oMGW), A(ar_->off.oATW), Ww, H);
    bar(cb);
    dGemm(cb, famIdx, A(ar_->off.oATW), A(woff(projN)), A(ar_->off.oABW), 64, C, C, Ww,
          64 * C, 0, 64 * C, gflags(EPI_NONE, false), C, C, C);
    bar(cb);
    dGather(cb, A(ar_->off.oABW), A(ffnOff), A(vec(acosN)), A(rawOff), A(publish ? pubOff : rawOff),
            s, (uint32_t)C, padTop, padLeft, wp8, publish, ffn16);
    bar(cb);
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
        dGemm(cb, famIdx, A(x16), A(woff(tn("block%d.layer0.weight1", idx))), A(O.oG3),
              TOKS, 4 * C, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 4 * C, 4 * C);
        bar(cb);
        dGemm(cb, famIdx, A(O.oG3), A(woff(tn("block%d.layer0.weight2", idx))), A(O.oG4),
              TOKS, C, 4 * C, 1, 0, 0, 0, gflags(EPI_NONE, false), 4 * C, C, C);
        bar(cb);
        if (cur.is16)
            dEw(cb, A(O.oG4), A(cur.off), A(O.oG4), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))), 0,
                TOKS * C, 4, C);
        else
            dEw(cb, A(O.oG4), A(cur.off), A(O.oG4), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))), 0,
                TOKS * C, 1, C);
        bar(cb);
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, O.oG4, false, rawOff, pubOff, publish);
    } else if (fam == 1) {
        uint64_t expBase = A(woff(tn("block%d.layer0.ffn_expand_weight", idx)));
        uint64_t prjBase = A(woff(tn("block%d.layer0.ffn_branch_projection_weight", idx)));
        for (uint32_t oh = 0; oh < G; ++oh) {
            dGemm(cb, famIdx, A(x16), expBase + (uint64_t)oh * C * 128 * 2, A(O.oG3),
                  TOKS, 128, C, 1, 0, 0, 0, gflags(EPI_GATE_E4M3, true), C, 128, 128);
            bar(cb);
            dGemm(cb, famIdx, A(O.oG3), prjBase + (uint64_t)oh * 128 * 32 * 2, A(O.oG4 + oh * 32 * 2),
                  TOKS, 32, 128, 1, 0, 0, 0, gflags(EPI_E4M3, true), 128, 32, C);
            bar(cb);
        }
        dGemm(cb, famIdx, A(O.oG4), A(woff(tn("block%d.layer0.ffn_output_projection_weight", idx))),
              A(O.oG3), TOKS, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dEw(cb, A(O.oG3), A(cur.off), A(O.oG3), A(vec(tn("block%d.layer0.ffn_cos_skip", idx))),
            A(O.oG2), TOKS * C, 5, C);
        bar(cb);
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, O.oG2, true, rawOff, pubOff, publish);
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
        dGemm(cb, famIdx, A(O.oG2), A(woff(tn("block%d.layer1.weight3", idx))), A(O.oG3),
              TOKS, C, C, 1, 0, 0, 0, gflags(EPI_NONE, false), C, C, C);
        bar(cb);
        dEw(cb, A(O.oG3), A(cur.off), A(O.oG3), A(vec(tn("block%d.layer1.ffn_cos_skip", idx))),
            0, TOKS * C, 4, C);
        bar(cb);
        recordWindowAttn(cb, famIdx, idx, s, C, H, fam, oy, ox, O.oG3, false, rawOff, pubOff, publish);
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
    dGemm(cb, famIdx, A(O.oHG), A(woff(tn("block%d.layer1.weight", idx))), A(O.oG3),
          TOKG, 1024, 4096, 1, 0, 0, 0, gflags(EPI_NONE, false), 4096, 1024, 1024);
    bar(cb);
    dEw(cb, A(O.oG3), A(cur.off), A(O.oG3), A(vec(tn("block%d.layer1.ffn_cos_skip", idx))),
        0, TOKG * 1024, 4, 1024);
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
        GemmPush p{A(O.oG2), A(O.oHeadW), A(O.oHEAD), st_[0].TOK, 16, 32, 1, 0, 0, 0,
                   gflags(EPI_NONE, false), 32, 16, 16};
        vkCmdPushConstants(cb, pGemm1.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, 1, st_[0].TOK / 16, 1);
        bar(cb);
    }
}

} // namespace d5c
