// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - M8bRec: the 71-block DLSSNR chain recorder (Stage 3c extraction
// from main.cpp).
//
// Owns the whole record-time world the chain lambdas used to capture: arena
// slot offsets, chain pipelines, per-dispatch BDA span audit, submit-bisect
// instrumentation and the hazZone barrier policy. References to main-owned
// state (SegSubmitter, chunks, arena segs, weight offset maps, stage table)
// are bound once at construction - the bodies run verbatim against member
// names identical to the old capture list.
//
// Instrumentation stays environment-gated exactly as before (D5_BAR_MODE,
// D5_SUBMIT_EVERY, D5_SEG_MB, D5_SPANTAG, D5_SPLIT_FROM, D5_TAIL_MONO,
// D5_CO, D5_NOHAZ); defaults are silent.
// ============================================================================
#pragma once

#include "chain_bufs.h"
#include "weights_file.h"
#include "seg_submit.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

// One arena chunk: a device buffer plus the BDA base and the logical
// [start,end) range of arena offsets it backs (moved from main.cpp).
struct Chunk { VkDeviceSize start, end; ChainBuf b; uint64_t baseA = 0; };

class M8bRec {
public:
    // ---- arena slot offsets (filled by main during arena layout) ---------
    VkDeviceSize oHeadW = 0;    // folded [32,16] f16 head weight
    VkDeviceSize oX16 = 0, oADA = 0, oG1 = 0, oG2 = 0, oG3 = 0, oG4 = 0;
    VkDeviceSize oRAW = 0, oPOOL = 0;
    VkDeviceSize oWIN = 0, oPROJW = 0;
    VkDeviceSize oQW = 0, oKW = 0, oVW = 0, oSCW = 0, oPRW = 0;
    VkDeviceSize oMGW = 0, oATW = 0, oABW = 0;
    VkDeviceSize oHG = 0, oPROJG = 0;
    VkDeviceSize oQG = 0, oKG = 0, oVG = 0, oSCG = 0, oPRG = 0;
    VkDeviceSize oMGG = 0, oATG = 0, oABG = 0, oBRG = 0;
    VkDeviceSize oB0RAW = 0, oFRS = 0;
    VkDeviceSize oSKIP0 = 0, oSKIP1 = 0, oSKIP2 = 0, oSKIP3 = 0, oSS = 0;
    VkDeviceSize oB4DS = 0, oB22DS = 0, oBRIDGE = 0;
    VkDeviceSize oB38 = 0, oB39 = 0, oB48 = 0, oB69 = 0;
    VkDeviceSize oMRG39 = 0, oMRG70 = 0, oHEAD = 0;
    VkDeviceSize oFeatV = 0, oHeadDC = 0;

    // ---- chain pipelines (filled by main via chainMakePipe) --------------
    ChainPipe pGemm, pGemm1, pCos, pCosW, pSmax, pEw, pPart, pTrans,
              pGather, pMerge, pFeatpack, pHeadpack, pPool, pUpM;

    // Borrowed references bound at construction; all outlive the recorder.
    M8bRec(SegSubmitter &sub,
           const std::vector<Chunk> &chunks,
           const std::vector<std::pair<VkDeviceSize, VkDeviceSize>> &segs,
           const Stage (&st)[7], uint32_t T6P,
           std::map<std::string, uint64_t> &poff,
           std::map<std::string, VkDeviceSize> &vecOff,
           VkEvent barEvent, VkQueryPool tsPool, PFN_vkCmdDispatch realDispatch);

    // BDA / arena helpers (also used by the frame path).
    int          chunkOf(VkDeviceSize o) const;
    uint64_t     A(VkDeviceSize o) const;
    VkBuffer     bufOf(VkDeviceSize o) const;
    VkDeviceSize localOff(VkDeviceSize o) const;

    // Record-time instrumentation + the hazard-zone separator. m8bDispatch is
    // wired in main as `#define vkCmdDispatch rec.m8bDispatch` so every
    // dispatch in the frame path is counted (it forwards to realDispatch
    // first; the bookkeeping only fires for the frame buffer).
    void spanTag(const char* tag,
                 std::initializer_list<std::pair<uint64_t, uint64_t>> ops);
    void m8bDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z);
    void bar(VkCommandBuffer cb);
    void recordChain(VkCommandBuffer &cb);

private:
    struct Cur { VkDeviceSize off; bool is16; uint32_t C; };
    struct PoolPush { uint64_t a, c; uint32_t Hin, Win, Wpad, C, n, flags; };
    struct UpMergePush {
        uint64_t a, b, c, d, e, h;
        uint32_t Wt, Ht, Wl, C, kind;
    };

    // borrowed state (identical names to the old lambda capture list)
    SegSubmitter &sub;
    const std::vector<Chunk> &chunks;
    const std::vector<std::pair<VkDeviceSize, VkDeviceSize>> &segs;   // ar.segs
    const Stage *st;
    // non-const refs: the moved code indexes with operator[] (same as the
    // original [&]-captured lambdas did)
    std::map<std::string, uint64_t> &poff;
    std::map<std::string, VkDeviceSize> &vecOff;
    VkCommandBuffer &cmd;         // == sub.cmd (sees fresh segment buffers)
    bool &hazZone;                // == sub.hazZone
    VkEvent barEvent;
    VkQueryPool tsPool;
    PFN_vkCmdDispatch realDispatch;
    const uint32_t T6P;
    const int barMode;
    const long submitEvery;
    const long splitFrom;
    const long tailMono;
    const uint64_t segBudget;
    const bool spanLog;
    const bool hazOff;
    long coA = -1, coB = -1;

    // instrumentation state
    long dispSeq = 0;
    std::vector<std::pair<uint64_t, uint64_t>> segRanges;
    uint64_t segDisp = 0;
    uint32_t tsq = 0;

    uint64_t slotRemain(uint64_t bda) const;
    uint64_t segUnion();
    void     segFlush();

    static uint32_t           gflags(uint32_t epi, bool narrow);
    static void               winGeo(const Stage &s, int oy, int ox,
                                     uint32_t &padTop, uint32_t &padLeft,
                                     uint32_t &Ww, uint32_t &wp8);
    static std::pair<int,int> originOf(int blockIndex);
    static std::string        tname(const char *fmt, int i);

    void dGemm(VkCommandBuffer &cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
               uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
               uint32_t sa, uint32_t sb, uint32_t sc,
               uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc);
    void dEw(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
             uint64_t h, uint32_t n, uint32_t kind, uint32_t ch);
    void dPart(VkCommandBuffer &cb, uint64_t a, uint64_t c2, const Stage &s,
               uint32_t C, uint32_t padTop, uint32_t padLeft, uint32_t wp8,
               bool in16);
    void dGather(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t d,
                 uint64_t c2, uint64_t h, const Stage &s, uint32_t C,
                 uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool pub,
                 bool b16);
    void dTrans(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Ww,
                uint32_t H);
    void dCosW(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint64_t sc,
               uint32_t Ww, uint32_t H, uint32_t C, uint32_t kind);
    void dCosG(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint64_t sc,
               uint32_t kind);
    void dSmaxW(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Ww,
                uint32_t H, uint64_t bias);
    void dSmaxG(VkCommandBuffer &cb, uint64_t a, uint64_t c2);
    void dMerge(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2,
                uint64_t d, uint64_t e, uint64_t h, uint32_t n, uint32_t ch,
                uint32_t kind);
    void dPool(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Hin,
               uint32_t Win, uint32_t Wpad, uint32_t Hpad, uint32_t C,
               uint32_t flags);
    void dUpMerge(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2,
                  uint64_t sin, uint64_t cos, uint64_t h, const Stage &tgt,
                  const Stage &low, uint32_t C, uint32_t kind);

    void recordWindowAttn(VkCommandBuffer &cb, int famIdx, int idx,
                          const Stage &s, int C, int H, int fam, int oy, int ox,
                          VkDeviceSize ffnOff, bool ffn16,
                          VkDeviceSize rawOff, VkDeviceSize pubOff,
                          bool publish);
    void recordWindowBlock(VkCommandBuffer &cb, int famIdx, int idx,
                           const Stage &s, int C, int H, int fam, int oy,
                           int ox, bool publish, Cur &cur,
                           VkDeviceSize rawOff, VkDeviceSize pubOff);
    void recordDs(VkCommandBuffer &cb, int famIdx, int idx, int C, int Cn,
                  VkDeviceSize rawOff, VkDeviceSize outOff,
                  const Stage &inS, const Stage &outS, bool pad8);
    void recordUp(VkCommandBuffer &cb, int famIdx, int idx, int Ci, int Co,
                  Cur &cur, VkDeviceSize skipOff, VkDeviceSize out16Off,
                  const Stage &lowS, const Stage &tgtS);
    void recordGlobal(VkCommandBuffer &cb, int famIdx, int idx, Cur &cur,
                      VkDeviceSize rawOff, VkDeviceSize pubOff);
};
