// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain — ChainRecorder: the 71-block DLSSNR U-Net command recording.
// Chain pipelines (push-constant/BDA, descriptor-less) + per-block record
// methods. Extracted VERBATIM from dlss5/m8b-live/main.cpp section 11.
// Validated bit-exact vs the torch reference (see work/_m9b_cmp).
// ============================================================================
#pragma once

#include "vk_util.h"
#include "arena.h"
#include "weights.h"
#include "prof.h"

namespace d5c {

inline int parsePilot(const char* envName, const char* legacyEnv, int defaultVal) {
    if (const char* p = std::getenv(envName)) {
        if (std::strcmp(p, "none") == 0 || std::strcmp(p, "off") == 0) return -1;
        return std::atoi(p);
    }
    if (legacyEnv) {
        if (const char* p = std::getenv(legacyEnv)) {
            if (std::strcmp(p, "none") == 0 || std::strcmp(p, "off") == 0) return -1;
            return std::atoi(p);
        }
    }
    return defaultVal;
}

// push blocks (scalar layout) — verbatim from m8-full-chain main.cpp
#pragma pack(push, 1)
struct GemmPush {
    uint64_t a, b, c;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
    uint32_t pad;
    uint64_t res;
    uint64_t skip;
    uint64_t cosd;   // EPI_COSQKV only: per-head attn_scale fp32 [H]
                     // (epi==COSQKV also reuses c/res/skip as oQW/oKW/oVW f16 bases)
                     // EPI_GATHER_RES: reused as the e4m3 f16 publish dst (h);
                     //  c = image-order fp32 oRAW, res = ffn residual, skip = cos
    uint32_t gGeo0;  // EPI_GATHER_RES: Himg | (Wimg << 16)
    uint32_t gGeo1;  // EPI_GATHER_RES: padTop | padLeft<<6 | gflags<<12 | wp8<<16
                     //  (gflags bit0 publish, bit1 residual is f16)
};

struct GemmI8Push {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t b_scale;
    uint64_t a_scale;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
    float scale_a;
    uint64_t res;
    uint64_t skip;
    uint64_t out_scale;
    uint64_t cosd;   // EPI_COSQKV only (same field reuse as GemmPush);
                     // EPI_GATHER_RES: e4m3 f16 publish dst (h), same as GemmPush
    uint32_t gGeo0;  // EPI_GATHER_RES: Himg | (Wimg << 16)
    uint32_t gGeo1;  // EPI_GATHER_RES: padTop | padLeft<<6 | gflags<<12 | wp8<<16
};

struct QuantActPush {
    uint64_t a;       // input FP16 [M, K]
    uint64_t aq;      // output INT8 [M, K]
    uint64_t scales;  // output float scales[ceil(M/32)]
    uint32_t m, k;    // matrix dimensions
};

struct CosPush { uint64_t a, c, d; uint32_t m, flags; };

struct CosWinPush {
    uint64_t a;          // proj fp32 [W*64, 3C]
    uint64_t q;          // out Q f16 [W, H, 64, 32]
    uint64_t k;          // out K f16 [W, H, 64, 32]
    uint64_t v;          // out V f16 [W, H, 64, 32]
    uint64_t d;          // per-head fp32 scale (for Q)
    uint32_t m;          // vector count = W*64*H
    uint32_t C;          // channel count = H*32
    uint32_t hcount;     // H (heads)
    uint32_t pad;
};

#pragma pack(pop)  // SMaxPush: scalar layout aligns uint64 `bias` to 8 (offset 32);
                   // pack(1) would place it at 28 -> shader reads garbage nonzero
                   // bias -> wild BDA deref -> async device lost (M8a run-5 root cause)
struct SMaxPush { uint64_t a, c; uint32_t m, n; float p0; uint64_t bias; uint32_t hcount; uint32_t nReal; };
#pragma pack(push, 1)

struct EWPush { uint64_t a, b, c, d, h; uint32_t n, kind, ch; };

struct PartPush { uint64_t a, c; uint32_t Himg, Wimg, C, padTop, padLeft, wp8, flags; };

struct TransWePush { uint64_t a, c; uint32_t W, hcount; };

struct GatherPush {
    uint64_t a, b, d, c, h;
    uint32_t Himg, Wimg, C, padTop, padLeft, wp8, flags;
};

struct MergePush { uint64_t a, b, c, d, e, h; uint32_t n, ch, kind; };

struct PackPush { uint64_t src, dst; uint32_t n; };
// headpack two-pass push: mode 0 accumulates per-channel DC, mode 1 writes
// the calibrated matched residual (see shaders/m8/headpack.comp).
struct HeadPush { uint64_t src, dst, dc; uint32_t ntok; float gain; uint32_t mode; uint32_t cols; };

struct AttnFusedPush {
    uint64_t q;        // oQW f16 [Ww*H, 64, 32]
    uint64_t k;        // oKW f16 [Ww*H, 64, 32]
    uint64_t v;        // oVW f16 [Ww*H, 64, 32]
    uint64_t bias;     // attn_bias f16 [H, 64, 64] or 0
    uint64_t dst;      // oMGW fp32 output; flags bit0: oATW f16 token-major
    uint32_t Ww;       // number of windows
    uint32_t H;        // number of heads
    uint32_t flags;    // bit0: fused transpose store (transpose_we folded in)
};

// ffn_fused: SLM-resident fam0 FFN (one 32-token tile per workgroup; hidden
// activations stay in shared memory). Mirrors quant_act + gemm_i8 producer +
// gemm_i8 consumer bit-exactly. flags bit0: residual is f16; bit1: f16 dup.
struct FfnFusedPush {
    uint64_t a;      // x16 f16 input [m, C]
    uint64_t w1;     // i8 expand weight [C, 4C]
    uint64_t w2;     // i8 project weight [4C, C]
    uint64_t w1s;    // f32 per-channel w1 scales [4C]
    uint64_t w2s;    // f32 per-channel w2 scales [C]
    uint64_t c;      // oG4 fp32 output [m, C]
    uint64_t res;    // residual carrier (fp32 or f16 per flags bit0)
    uint64_t skip;   // ffn_cos_skip f32 [C]
    uint64_t dup;    // f16 duplicate dst (oG2) when flags bit1 set
    uint32_t m;      // tokens
    uint32_t cn;     // C (32)
    uint32_t flags;  // F_RES16 | F_DUP16
};

// ffn1_fused: SLM-resident fam1 (branched) FFN, one 32-token tile per
// 128-lane workgroup. Replaces expand+branch+out gemms + kind5 ew; hidden
// (oG3 [TOKS,4C] f16) and branch (oG4 [TOKS,C] f16) slabs stay in SLM.
struct Ffn1FusedPush {
    uint64_t a;      // x16 f16 input [m, C] (== the f16 carrier)
    uint64_t wexp;   // f16 [G, C, 128]
    uint64_t wprj;   // f16 [G, 128, 32]
    uint64_t wout;   // f16 [C, C]
    uint64_t res;    // residual carrier f16 [m, C] (== a for fam1)
    uint64_t skip;   // ffn_cos_skip f32 [C]
    uint64_t dst;    // oG2 f16 out [m, C]
    uint32_t m;      // tokens
    uint32_t cn;     // C (64/128/256)
    uint32_t flags;  // reserved, 0
};

// attn_global: one dispatch per L6 block = cosg x3 + QK^T + masked softmax
// + PV + ew3 publish. proj fp32 [T6P,3072] -> dst f16 [T6P,1024].
struct AttnGlobalPush {
    uint64_t proj;   // oPROJG fp32
    uint64_t scale;  // per-head attn_scale fp32 [32] (x sqrt(32), Q only)
    uint64_t dst;    // oATG f16
    uint32_t t6p;    // padded token count (<= 512, multiple of 32)
    uint32_t nReal;  // real tokens (softmax col mask)
    uint32_t hcount; // heads (32)
};

// winblock (winblock.comp): one 256-lane workgroup per 8x8 window runs the
// whole attention half of a C=32/H=1 fam0 block - window gather + QKV gemm +
// cosine publish + merged window attention + projection + residual +
// publish, with block 0's fused e4m3+pool or block 70's head GEMM as tail
// variants. Bit-exact vs the F_APART/COSQKV/attn_fused/GATHER_RES chain
// (f16 accumulators, verbatim element math). Replaces the oQW/oKW/oVW/oATW/
// oPROJW/oABW DRAM round-trips + 4 barriers with shared memory.
struct WinBlockPush {
    uint64_t a;      // fp32 image carrier (gather source + residual skip)
    uint64_t b;      // qkv_weight f16 [32,96]
    uint64_t c;      // fp32 raw image-order out (WB_RAW32), else unused
    uint64_t d;      // attn_bias f16 [64,64]
    uint64_t proj;   // projection_weight f16 [32,32]
    uint64_t aux;    // WB_POOL: pooled f16 dst; WB_HEAD: headW f16 [32,16]
    uint64_t resCos; // attn_cos_skip f32 [32]
    uint64_t qscale; // attn_scale f32 [H] (Q scale at [0])
    uint64_t pub;    // e4m3 f16 publish dst (WB_PUB / WB_POOL)
    uint64_t head;   // oHEAD fp32 [TOK,4] (WB_HEAD)
    uint32_t m;      // window count
    uint32_t flags;  // WB_* bits
    uint32_t image_h, image_w;
    uint32_t window_cols;  // padded windows per row (wp8)
    uint32_t window_pad;   // padTop | padLeft << 16
};
enum { WB_PUB = 1u, WB_RAW32 = 2u, WB_POOL = 4u, WB_HEAD = 8u, WB_E4SRC = 0x10u };
// recordWindowBlock tail selector for the fused kernel. WB_TAIL_POOL (block 0)
// publishes e4m3 to pubOff2 and pools into auxOff, replacing the standalone
// ew3+dPool pair; WB_TAIL_HEAD (block 70) runs the head GEMM in-kernel
// (weights at auxOff, compact fp32 [TOK,4] at headOff), replacing the
// ew2+gemm tail. keepRaw forces the fp32 image-order store alongside WB_PUB.
struct WinBlkTail {
    uint32_t mode = 0;             // WB_TAIL_*
    VkDeviceSize pubOff2 = 0;
    VkDeviceSize auxOff = 0;
    VkDeviceSize headOff = 0;
    bool keepRaw = false;
};
enum { WB_TAIL_NONE = 0u, WB_TAIL_POOL = 1u, WB_TAIL_HEAD = 2u };
#pragma pack(pop)

enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4,
       EPI_RESIDUAL_FP32 = 5, EPI_RESIDUAL_F16 = 6, EPI_COSQKV = 7,
       EPI_GATHER_RES = 8 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1, F_FUSED_I8_PRODUCER = 0x2000, F_FUSED_I8_CONSUMER = 0x4000,
       F_CN = 0x2,           // gemm.comp only: C stores just cols [0,pad) at
                             // ldc (compact head [TOK,4]); tile compute is
                             // identical, values bit-exact
       F_APART = 0x8000,     // gemm.comp only: A operand gathers through the
                             // inverse window map (partition fused; gGeo0/gGeo1
                             // carry the window geometry, gfl bit0 = f16 src)
       F_DUP16 = 0x10000 };  // residual epilogue also writes an f16 duplicate
                             // (half_round of the fp32 result, partition's exact
                             // bits) to the image-order buffer in `cosd` - lets
                             // the fused qkv A-load take the cheap f16 path.

struct ChainPipe {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;   // layouts reference it — keep alive
};

class ChainRecorder {
public:
    // shaderDir: directory containing the 14 chain .spv (trailing slash).
    void init(const VkCtx& c, const ChainArena& arena, const WeightsStore& ws,
              const Stage st[7], const std::string& shaderDir);
    void destroy(const VkCtx& c);

    // Optional per-dispatch profiler (M10). Not owned; nullptr = disabled.
    void setProf(ChainProf* p) { prof_ = p; }

    // Records the full 71-block chain (~1400 dispatches) into cb.
    // Input: oX16 (f16 features via featpack); output: oHEAD (fp32 [TOK0,16]).
    void recordChain(VkCommandBuffer cb);

    // front-end bridge: featpack (features fp32 -> chain input f16 at oX16)
    const ChainPipe& featpackPipe() const { return pFeatpack; }

private:
    void bar(VkCommandBuffer cb);
    // profiler marks (no-op when profiling is disabled)
    void pmBegin(VkCommandBuffer cb) {
        if (prof_) prof_->markBegin(cb);
    }
    void pmEnd(VkCommandBuffer cb, const char* fam, const char* note,
               uint32_t m, uint32_t n, uint32_t k, uint32_t gx, uint32_t gy, uint32_t gz,
               uint64_t bytes = 0) {
        if (prof_) prof_->markEnd(cb, fam, note, m, n, k, gx, gy, gz, bytes);
    }
    void pm(VkCommandBuffer cb, const char* fam, const char* note,
            uint32_t m, uint32_t n, uint32_t k, uint32_t gx, uint32_t gy, uint32_t gz,
            uint64_t bytes = 0) {
        if (prof_) {
            prof_->markBegin(cb);
            prof_->markEnd(cb, fam, note, m, n, k, gx, gy, gz, bytes);
        }
    }
    static uint32_t gflags(uint32_t epi, bool narrow) { return (epi << 8) | (narrow ? F_NARROW : 0u); }

    // dispatchers (one per chain kernel)
    void dGemm(VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
               uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
               uint32_t sa, uint32_t sb, uint32_t sc,
               uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc,
               uint64_t res = 0, uint64_t skip = 0, uint64_t cosd = 0,
               uint32_t gGeo0 = 0, uint32_t gGeo1 = 0);
    void dGemmI8(VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
                 uint64_t b_scale, uint64_t a_scale, float scale_a,
                 uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                 uint32_t sa, uint32_t sb, uint32_t sc,
                 uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc,
                 uint64_t res = 0, uint64_t skip = 0, uint64_t out_scale = 0,
                 uint64_t cosd = 0, uint32_t gGeo0 = 0, uint32_t gGeo1 = 0);
    void dQuantAct(VkCommandBuffer cb, uint64_t a, uint64_t aq, uint64_t scales,
                   uint32_t m, uint32_t k);
    void dEw(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
             uint64_t h, uint32_t n, uint32_t kind, uint32_t ch);
    void dPart(VkCommandBuffer cb, uint64_t a, uint64_t c2, const Stage& s, uint32_t C,
               uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16);
    void dGather(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c2,
                 uint64_t h, const Stage& s, uint32_t C, uint32_t padTop, uint32_t padLeft,
                 uint32_t wp8, bool pub, bool b16);
    void dTrans(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H);
    void dCosW(VkCommandBuffer cb, uint64_t a, uint64_t q, uint64_t k, uint64_t v,
               uint64_t sc, uint32_t Ww, uint32_t H, uint32_t C);
    void dCosG(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc, uint32_t kind);
    void dSmaxW(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H,
                uint64_t bias);
    void dSmaxG(VkCommandBuffer cb, uint64_t a, uint64_t c2);
    void dMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind);
    void dPool(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Hin, uint32_t Win,
               uint32_t Wpad, uint32_t Hpad, uint32_t C, uint32_t flags);
    void dUpMerge(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t sin,
                  uint64_t cos, uint64_t h, const Stage& tgt, const Stage& low,
                  uint32_t C, uint32_t kind);
    void dAttnFused(VkCommandBuffer cb, uint64_t q, uint64_t k, uint64_t v, uint64_t bias,
                    uint64_t out, uint32_t Ww, uint32_t H, bool fuseT = false);
    void dFfnFused(VkCommandBuffer cb, int fam, uint64_t a, uint64_t w1, uint64_t w2,
                   uint64_t w1s, uint64_t w2s, uint64_t c2,
                   uint64_t res, uint64_t skip, uint64_t dup,
                   uint32_t m, uint32_t C, uint32_t flags);
    void dFfn1Fused(VkCommandBuffer cb, int fam, uint64_t a, uint64_t wexp,
                    uint64_t wprj, uint64_t wout, uint64_t res, uint64_t skip,
                    uint64_t dst, uint32_t m, uint32_t C);
    void dAttnGlobal(VkCommandBuffer cb, uint64_t proj, uint64_t scale,
                     uint64_t dst);
    void dWinBlock(VkCommandBuffer cb, const WinBlockPush& p);

    // block recorders
    // off/is16 = the block input carrier (FFN GEMM operand); resOff/res16,
    // when resOff != 0, give the FFN residual a different source - the vendor
    // graph splits them for the 32-channel merge blocks (b66: published input,
    // raw fp32 skip).
    struct Cur { VkDeviceSize off; bool is16; uint32_t C; VkDeviceSize resOff; bool res16; };
    // ffnOff/ffn16 = the FFN carrier (partition source + gather_res residual).
    // qkvOff/qkv16 = the A source the fused qkv partition-load reads; differs
    // from ffnOff for fp32 carriers duplicated to f16 by F_DUP16 (fam0/2).
    // Both return true when the fused winblock kernel consumed the attention
    // half (incl. the caller's publish/pool/head tail when wb.mode != 0), so
    // recordWindowBlock / recordChain can skip the replaced passes.
    bool recordWindowAttn(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                          int C, int H, int fam, int oy, int ox,
                          VkDeviceSize ffnOff, bool ffn16,
                          VkDeviceSize qkvOff, bool qkv16,
                          VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish,
                          const WinBlkTail& wb);
    bool recordWindowBlock(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                           int C, int H, int fam, int oy, int ox, bool publish, Cur& cur,
                           VkDeviceSize rawOff, VkDeviceSize pubOff,
                           const WinBlkTail& wb = WinBlkTail{});
    void recordDs(VkCommandBuffer cb, int famIdx, int idx, int C, int Cn,
                  VkDeviceSize rawOff, VkDeviceSize outOff,
                  const Stage& inS, const Stage& outS, bool pad8);
    void recordUp(VkCommandBuffer cb, int famIdx, int idx, int Ci, int Co,
                  Cur& cur, VkDeviceSize skipOff, VkDeviceSize out16Off,
                  const Stage& lowS, const Stage& tgtS);
    void recordGlobal(VkCommandBuffer cb, int famIdx, int idx, Cur& cur,
                      VkDeviceSize rawOff, VkDeviceSize pubOff);
    static std::pair<int, int> originOf(int blockIndex);

    const VkCtx* c_ = nullptr;
    const ChainArena* ar_ = nullptr;
    const WeightsStore* ws_ = nullptr;
    ChainProf* prof_ = nullptr;
    Stage st_[7]{};
    uint64_t T6_ = 0;
    uint32_t T6P_ = 0;

    ChainPipe pGemm, pGemm1, pGemmI8, pQuantAct, pCos, pCosW, pSmax, pEw, pPart, pTrans, pGather,
              pMerge, pFeatpack, pHeadpack, pPool, pUpM, pAttnFused, pFfnFused,
              pFfn1Fused, pAttnGlobal, pWinBlock;
    ChainPipe pGemmM8;   // gemm_m8.spv: slim BM=8 tile for narrow-out large-m gemms
    bool slimGemm_ = false;  // D5C_GEMM_SLIM (default on): dispatch pGemmM8 for
                             // narrow-out gemms at m>=2048 (or >=512 batched)

    uint64_t A(VkDeviceSize o) const { return ar_->A(o); }
    uint64_t vec(const std::string& n) const { return ar_->vecOff.at(n); }
    uint64_t woff(const std::string& n) const { return ws_->poff.at(n); }
    uint64_t wscale(const std::string& n) const { return ws_->scaleOff(n); }
    bool isQuantized(const std::string& n) const { return ws_->isQuantized(n); }
    std::string tn(const char* fmt, int i) const { return WeightsStore::tname(fmt, i); }

    // B4 fused-partition decision, shared by the FFN tail (which may emit an
    // f16 duplicate of the fp32 carrier via F_DUP16) and recordWindowAttn's
    // fused A-load. Fusing is skipped when the qkv gemm itself runs int8
    // (it consumes materialized oWIN through quant_act). D5C_FUSE_PART=0 off.
    bool qkvApart(int fam, int idx) const {
        if (parsePilot("D5C_FUSE_PART", nullptr, 1) <= 0) return false;
        int pilotQKV = parsePilot("D5C_INT8_ATTN_QKV", "D5C_INT8_ATTN", -1);
        std::string qkvN = tn(fam == 2 ? "block%d.layer2.qkv_weight"
                                       : "block%d.layer0.qkv_weight", idx);
        return !(isQuantized(qkvN) && pilotQKV >= 0 && idx <= pilotQKV);
    }

    // winblock (winblock.comp) mega-kernel eligibility: fam0 C=32/H=1 window
    // blocks (b0-4, b66-70) on the f16 path - the int8 qkv/proj pilots keep
    // their materialized oWIN path, and the kernel's gather only implements
    // the fp32-carrier variant (fam0's oG4). D5C_WINBLOCK=0 restores the
    // split path for bisecting; D5C_WINBLOCK_E4SRC=1 switches the gather
    // rounding to e4m3 (vendor-graph experiment).
    bool winBlkEligible(int fam, int idx, int C, int H) const {
        if (parsePilot("D5C_WINBLOCK", nullptr, 1) <= 0) return false;
        if (fam != 0 || C != 32 || H != 1) return false;
        int pilotQKV = parsePilot("D5C_INT8_ATTN_QKV", "D5C_INT8_ATTN", -1);
        int pilotProj = parsePilot("D5C_INT8_ATTN_PROJ", "D5C_INT8_ATTN", -1);
        if (isQuantized(tn("block%d.layer0.qkv_weight", idx))
                && pilotQKV >= 0 && idx <= pilotQKV) return false;
        if (isQuantized(tn("block%d.layer0.projection_weight", idx))
                && pilotProj >= 0 && idx <= pilotProj) return false;
        return true;
    }
};

} // namespace d5c
