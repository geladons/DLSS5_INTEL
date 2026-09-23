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

namespace d5c {

// push blocks (scalar layout) — verbatim from m8-full-chain main.cpp
#pragma pack(push, 1)
struct GemmPush {
    uint64_t a, b, c;
    uint32_t m, n, k, batch;
    uint32_t sa, sb, sc;
    uint32_t flags;
    uint32_t lda, ldb, ldc;
};

struct CosPush { uint64_t a, c, d; uint32_t m, flags; };

struct CosWinPush { uint64_t a, c, d; uint32_t m, flags, C, hcount; };

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
#pragma pack(pop)

enum { EPI_NONE = 0, EPI_E4M3 = 1, EPI_GATE = 2, EPI_GATE_E4M3 = 3, EPI_HALF = 4 };
enum { F_NARROW = 0x1000, F_TRANSPOSE = 1 };

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

    // Records the full 71-block chain (~1400 dispatches) into cb.
    // Input: oX16 (f16 features via featpack); output: oHEAD (fp32 [TOK0,16]).
    void recordChain(VkCommandBuffer cb);

    // front-end bridge: featpack (features fp32 -> chain input f16 at oX16)
    const ChainPipe& featpackPipe() const { return pFeatpack; }

private:
    void bar(VkCommandBuffer cb);
    static uint32_t gflags(uint32_t epi, bool narrow) { return (epi << 8) | (narrow ? F_NARROW : 0u); }

    // dispatchers (one per chain kernel)
    void dGemm(VkCommandBuffer cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
               uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
               uint32_t sa, uint32_t sb, uint32_t sc,
               uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc);
    void dEw(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
             uint64_t h, uint32_t n, uint32_t kind, uint32_t ch);
    void dPart(VkCommandBuffer cb, uint64_t a, uint64_t c2, const Stage& s, uint32_t C,
               uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16);
    void dGather(VkCommandBuffer cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c2,
                 uint64_t h, const Stage& s, uint32_t C, uint32_t padTop, uint32_t padLeft,
                 uint32_t wp8, bool pub, bool b16);
    void dTrans(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H);
    void dCosW(VkCommandBuffer cb, uint64_t a, uint64_t c2, uint64_t sc,
               uint32_t Ww, uint32_t H, uint32_t C, uint32_t kind);
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

    // block recorders
    struct Cur { VkDeviceSize off; bool is16; uint32_t C; };
    void recordWindowAttn(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                          int C, int H, int fam, int oy, int ox,
                          VkDeviceSize ffnOff, bool ffn16,
                          VkDeviceSize rawOff, VkDeviceSize pubOff, bool publish);
    void recordWindowBlock(VkCommandBuffer cb, int famIdx, int idx, const Stage& s,
                           int C, int H, int fam, int oy, int ox, bool publish, Cur& cur,
                           VkDeviceSize rawOff, VkDeviceSize pubOff);
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
    Stage st_[7]{};
    uint64_t T6_ = 0;
    uint32_t T6P_ = 0;

    ChainPipe pGemm, pGemm1, pCos, pCosW, pSmax, pEw, pPart, pTrans, pGather,
              pMerge, pFeatpack, pHeadpack, pPool, pUpM;

    uint64_t A(VkDeviceSize o) const { return ar_->A(o); }
    uint64_t vec(const std::string& n) const { return ar_->vecOff.at(n); }
    uint64_t woff(const std::string& n) const { return ws_->poff.at(n); }
    std::string tn(const char* fmt, int i) const { return WeightsStore::tname(fmt, i); }
};

} // namespace d5c
