// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - M8bRec implementation (Stage 3c extraction from main.cpp).
// Method bodies are the verbatim chain-recorder lambdas; captured state is
// now the member set declared in m8b_rec.h (same names on purpose).
// ============================================================================
#include "m8b_rec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// Dispatch instrumentation: all vkCmdDispatch text in this TU routes through
// M8bRec::m8bDispatch (same scheme main.cpp used on this block).
#define vkCmdDispatch m8bDispatch

M8bRec::M8bRec(SegSubmitter &sub,
               const std::vector<Chunk> &chunks,
               const std::vector<std::pair<VkDeviceSize, VkDeviceSize>> &segs,
               const Stage (&st)[7], uint32_t T6P,
               std::map<std::string, uint64_t> &poff,
               std::map<std::string, VkDeviceSize> &vecOff,
               VkEvent barEvent, VkQueryPool tsPool, PFN_vkCmdDispatch realDispatch)
    : sub(sub), chunks(chunks), segs(segs), st(st), poff(poff), vecOff(vecOff),
      cmd(sub.cmd), hazZone(sub.hazZone), barEvent(barEvent), tsPool(tsPool),
      realDispatch(realDispatch), T6P(T6P),
      barMode(getenv("D5_BAR_MODE") ? atoi(getenv("D5_BAR_MODE")) : 0),
      submitEvery(getenv("D5_SUBMIT_EVERY") ? atol(getenv("D5_SUBMIT_EVERY")) : 0),
      splitFrom(getenv("D5_SPLIT_FROM") ? atol(getenv("D5_SPLIT_FROM")) : -1),
      tailMono(getenv("D5_TAIL_MONO") ? atol(getenv("D5_TAIL_MONO")) : -1),
      segBudget((getenv("D5_SEG_MB") ? (uint64_t)atol(getenv("D5_SEG_MB")) : 0) * 1048576ull),
      spanLog(getenv("D5_SPANTAG") != nullptr),
      hazOff(getenv("D5_NOHAZ") != nullptr) {
    if (getenv("D5_CO")) sscanf(getenv("D5_CO"), "%ld:%ld", &coA, &coB);
}

// BDA addressing resolves a logical arena offset via chunkOf/localOff.
int M8bRec::chunkOf(VkDeviceSize o) const {
    for (int i = 0; i < (int)chunks.size(); i++)
        if (o >= chunks[i].start && o < chunks[i].end) return i;
    return (int)chunks.size() - 1;
}
uint64_t M8bRec::A(VkDeviceSize o) const {
    int i = chunkOf(o);
    return chunks[i].baseA + (uint64_t)(o - chunks[i].start);
}
VkBuffer M8bRec::bufOf(VkDeviceSize o) const { return chunks[chunkOf(o)].b.buf; }
VkDeviceSize M8bRec::localOff(VkDeviceSize o) const { return o - chunks[chunkOf(o)].start; }

std::string M8bRec::tname(const char *fmt, int i) {
    char b[160]; std::snprintf(b, sizeof b, fmt, i); return b;
}

    // D5_BAR_MODE diagnostic (B50 device-lost bisect, 2026-10-03):
    //   0 = per-chunk VkBufferMemoryBarrier, size=VK_WHOLE_SIZE (original)
    //   1 = single global VkMemoryBarrier (same ordering, coarser scope)
    //   2 = no barrier at all (ordering broken; diagnosis only)
void M8bRec::bar(VkCommandBuffer cb) {
        if (hazZone && !hazOff) { sub.submitChained(); return; }
        if (barMode == 2) return;
        if (barMode == 5) {
            // Interpose a different command class between dispatches: a
            // timestamp write forces the front-end to re-sync launch state
            // (probe for a launch-phase hazard between adjacent dispatches).
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                tsPool, (tsq = (tsq + 1) % 6));
            VkBufferMemoryBarrier bs[8];
            uint32_t n = 0;
            for (auto &ch : chunks) {
                if (n >= 8) break;
                bs[n] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                         VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                         ch.b.buf, 0, VK_WHOLE_SIZE};
                n++;
            }
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, n, bs, 0, nullptr);
            return;
        }
        if (barMode == 4) {
            vkCmdSetEvent(cb, barEvent, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vkCmdWaitEvents(cb, 1, &barEvent, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, nullptr,
                            0, nullptr, 0, nullptr);
            return;
        }
        if (barMode == 1 || barMode == 3) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            const VkPipelineStageFlags st =
                barMode == 3 ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                             : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            vkCmdPipelineBarrier(cb, st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
            return;
        }
        // M9b: full compute barrier across every arena chunk.
        VkBufferMemoryBarrier bs[8];
        uint32_t n = 0;
        for (auto &ch : chunks) {
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
    // ---- TDR probe: per-dispatch BDA span audit + optional submit bisect.
    // The B50 driver device-losts on a single dispatch whose accessed BDA
    // byte-span exceeds ~2 GB (cross-chunk operand pairs do this silently
    // when the arena splits). spanTag is called at record time with each
    // dispatch's (ptr, accessedBytes) operand list and reports violations
    // decoded back to arena chunks. D5_SUBMIT_EVERY=N additionally ends/
    // submits/waits the frame cmd buffer every N dispatches so a hang or
    // device-lost localises to the guilty segment even if the cause is not
    // a span (e.g. a watchdog timeout on a single kernel).
    // Reverse map BDA -> remaining bytes in the containing arena slot (a
    // kernel can never legally touch past its slot end, so this clamps the
    // caller's extent estimates). Weights pack region (before ar.base) has
    // no seg entry -> falls back to the chunk end, an intentional
    // over-estimate so cross-chunk weight reads still flag.
uint64_t M8bRec::slotRemain(uint64_t bda) const {
        for (auto &ch : chunks) {
            if (bda < ch.baseA || bda >= ch.baseA + (ch.end - ch.start)) continue;
            VkDeviceSize o = ch.start + (VkDeviceSize)(bda - ch.baseA);
            for (auto &sg : segs)
                if (o >= sg.first && o < sg.first + sg.second)
                    return (uint64_t)(sg.first + sg.second - o);
            return (uint64_t)(ch.end - o);
        }
        return 0;
}
    // D5_SEG_MB: split the frame command buffer whenever the segment's
    // merged BDA footprint (union of operand ranges, slot-clamped) would
    // exceed N MB. Diagnosis + mitigation for submissions whose aggregate
    // referenced address space trips the driver.
uint64_t M8bRec::segUnion() {
        if (segRanges.empty()) return 0;
        auto v = segRanges;
        std::sort(v.begin(), v.end());
        uint64_t tot = 0, lo = v[0].first, hi = v[0].second;
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i].first <= hi) hi = std::max(hi, v[i].second);
            else { tot += hi - lo; lo = v[i].first; hi = v[i].second; }
        }
        return tot + hi - lo;
}
void M8bRec::segFlush() {
        if (!segDisp) return;
        std::printf("[segf] submit: %llu disp, union %.2f GB\n",
                    (unsigned long long)segDisp, segUnion() / 1073741824.0);
        std::fflush(stdout);
        sub.submitSeg();
        segRanges.clear();
        segDisp = 0;
}
void M8bRec::spanTag(const char* tag,
                       std::initializer_list<std::pair<uint64_t, uint64_t>> ops) {
        uint64_t lo = ~0ull, hi = 0;
        for (auto &e : ops) {
            if (!e.first) continue;
            uint64_t ext = std::min(e.second, slotRemain(e.first));
            lo = std::min(lo, e.first);
            hi = std::max(hi, e.first + ext);
            segRanges.push_back({e.first, e.first + ext});
        }
        ++segDisp;
        if (segBudget && segUnion() > segBudget) segFlush();
        if (!spanLog) return;
        if (lo == ~0ull || hi - lo <= 1500ull * 1024 * 1024) return;
        auto dec = [&](uint64_t a) {
            for (int i = 0; i < (int)chunks.size(); ++i) {
                uint64_t sz = chunks[i].end - chunks[i].start;
                if (a >= chunks[i].baseA && a < chunks[i].baseA + sz)
                    std::printf("c%d+0x%llx", i, (unsigned long long)(a - chunks[i].baseA));
            }
        };
        std::printf("[tdr-span] %s (disp ~%ld) span %.2f GB | lo=0x%llx(", tag, dispSeq + 1,
                    (hi - lo) / 1073741824.0, (unsigned long long)lo);
        dec(lo);
        std::printf(") hi=0x%llx(", (unsigned long long)hi);
        dec(hi);
        std::printf(")\n");
        std::fflush(stdout);
}
void M8bRec::m8bDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
        realDispatch(cb, x, y, z);
        if (cb != cmd) return;
        ++dispSeq;
        bool hit;
        if (tailMono >= 0) {
            // Singles (waited) for the first tailMono dispatches, then the
            // remainder rides in one monolithic segment - probes whether all
            // hazard edges live in the early (large-grid) zone.
            hit = (dispSeq < tailMono);
        } else if (coA >= 0) {
            // Co-submission window probe: every dispatch submits alone EXCEPT
            // [coA..coB), which rides in one submission. If that window holds
            // a hang-inducing cluster, the device dies at its flush.
            hit = !(dispSeq >= coA && dispSeq < coB);
        } else {
            // Split policy: an exact boundary at splitFrom (isolates the
            // prefix), then every submitEvery dispatches from there.
            hit = (dispSeq == splitFrom) ||
                  (submitEvery > 0 && dispSeq >= (splitFrom > 0 ? splitFrom : 1) &&
                   (dispSeq - (splitFrom > 0 ? splitFrom : 0)) % submitEvery == 0);
        }
        if (!hit) return;
        if (submitEvery > 0 || splitFrom > 0 || coA >= 0 || tailMono >= 0) {
            std::printf("[seg] submit through disp #%ld (union %.2f GB)\n",
                        dispSeq, segUnion() / 1073741824.0);
            std::fflush(stdout);
            segRanges.clear();
            segDisp = 0;
            sub.submitSeg();
        }
}
uint32_t M8bRec::gflags(uint32_t epi, bool narrow) { return (epi << 8) | (narrow ? F_NARROW : 0u); };
void M8bRec::dGemm(VkCommandBuffer &cb, int fam, uint64_t a, uint64_t b, uint64_t c2,
                     uint32_t m, uint32_t n, uint32_t k, uint32_t batch,
                     uint32_t sa, uint32_t sb, uint32_t sc,
                     uint32_t flags, uint32_t lda, uint32_t ldb, uint32_t ldc) {
        (void)fam;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGemm.pipe);
        GemmPush p{a, b, c2, m, n, k, batch, sa, sb, sc, flags, lda, ldb, ldc};
        vkCmdPushConstants(cb, pGemm.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("gemm", {{a, (uint64_t)batch * std::max<uint64_t>(sa, (uint64_t)m * lda) * 4},
                         {b, (uint64_t)batch * std::max<uint64_t>(sb, (uint64_t)k * ldb) * 4},
                         {c2, (uint64_t)batch * std::max<uint64_t>(sc, (uint64_t)m * ldc) * 4}});
        vkCmdDispatch(cb, n / 32, m / 16, batch);
}
void M8bRec::dEw(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                   uint64_t h, uint32_t n, uint32_t kind, uint32_t ch) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pEw.pipe);
        EWPush p{a, b, c2, d, h, n, kind, ch};
        vkCmdPushConstants(cb, pEw.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("ew", {{a, (uint64_t)n * 4}, {b, (uint64_t)n * 4}, {c2, (uint64_t)n * 4},
                       {d, (uint64_t)n * 4}, {h, (uint64_t)n * 4}});
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
}
void M8bRec::dPart(VkCommandBuffer &cb, uint64_t a, uint64_t c2, const Stage &s, uint32_t C,
                     uint32_t padTop, uint32_t padLeft, uint32_t wp8, bool in16) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPart.pipe);
        uint32_t hp8 = (s.H + padTop + 7) / 8;
        PartPush p{a, c2, s.H, s.W, C, padTop, padLeft, wp8, in16 ? 1u : 0u};
        vkCmdPushConstants(cb, pPart.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("part", {{a, (uint64_t)s.TOK * C * 4},
                         {c2, (uint64_t)hp8 * wp8 * 64 * C * 4}});
        vkCmdDispatch(cb, hp8 * wp8 * 64, 1, 1);
}
void M8bRec::dGather(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t d, uint64_t c2,
                       uint64_t h, const Stage &s, uint32_t C, uint32_t padTop, uint32_t padLeft,
                       uint32_t wp8, bool pub, bool b16) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pGather.pipe);
        GatherPush p{a, b, d, c2, h, s.H, s.W, C, padTop, padLeft, wp8,
                     (pub ? 1u : 0u) | (b16 ? 2u : 0u)};
        vkCmdPushConstants(cb, pGather.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("gather", {{a, (uint64_t)s.TOK * C * 8}, {b, (uint64_t)s.TOK * C * 8},
                           {d, (uint64_t)s.TOK * C * 8}, {c2, (uint64_t)s.TOK * C * 8},
                           {h, (uint64_t)s.TOK * C * 8}});
        vkCmdDispatch(cb, s.TOK, 1, 1);
}
void M8bRec::dTrans(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pTrans.pipe);
        TransWePush p{a, c2, Ww, H};
        vkCmdPushConstants(cb, pTrans.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("trans", {{a, (uint64_t)Ww * 64 * H * 512}, {c2, (uint64_t)Ww * 64 * H * 512}});
        vkCmdDispatch(cb, Ww * 64, 1, 1);
}
void M8bRec::dCosW(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint64_t sc,
                     uint32_t Ww, uint32_t H, uint32_t C, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCosW.pipe);
        CosWinPush p{a, c2, sc, Ww * 64 * H, kind, C, H};
        vkCmdPushConstants(cb, pCosW.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("cosw", {{a, (uint64_t)Ww * 64 * H * C * 4},
                         {c2, (uint64_t)Ww * 64 * H * C * 4}, {sc, 8192}});
        vkCmdDispatch(cb, (Ww * 64 * H + 31) / 32, 1, 1);
}
void M8bRec::dCosG(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint64_t sc, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pCos.pipe);
        CosPush p{a, c2, sc, T6P * 32, kind};
        vkCmdPushConstants(cb, pCos.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("cosg", {{a, (uint64_t)T6P * 32 * 1024 * 4},
                         {c2, (uint64_t)T6P * 32 * 1024 * 4}, {sc, 8192}});
        vkCmdDispatch(cb, (T6P * 32 + 31) / 32, 1, 1);
}
void M8bRec::dSmaxW(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Ww, uint32_t H,
                      uint64_t bias) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c2, Ww * H * 64, 64, 0.0f, bias, H, 0};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("smaxw", {{a, (uint64_t)Ww * H * 64 * 64 * 4},
                          {c2, (uint64_t)Ww * H * 64 * 64 * 4},
                          {bias, (uint64_t)Ww * H * 64 * 4}});
        vkCmdDispatch(cb, (Ww * H * 64 + 31) / 32, 1, 1);
}
void M8bRec::dSmaxG(VkCommandBuffer &cb, uint64_t a, uint64_t c2) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pSmax.pipe);
        SMaxPush p{a, c2, 32 * T6P, T6P, 3.0f, 0, 0, st[6].TOK};
        vkCmdPushConstants(cb, pSmax.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("smaxg", {{a, 32ull * T6P * T6P * 4}, {c2, 32ull * T6P * T6P * 4}});
        vkCmdDispatch(cb, 32 * T6P, 1, 1);
}
void M8bRec::dMerge(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t d,
                      uint64_t e, uint64_t h, uint32_t n, uint32_t ch, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pMerge.pipe);
        MergePush p{a, b, c2, d, e, h, n, ch, kind};
        vkCmdPushConstants(cb, pMerge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("merge", {{a, (uint64_t)n * 4}, {b, (uint64_t)n * 4}, {c2, (uint64_t)n * 4},
                          {d, (uint64_t)n * 4}, {e, (uint64_t)n * 4}, {h, (uint64_t)n * 4}});
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
}
void M8bRec::dPool(VkCommandBuffer &cb, uint64_t a, uint64_t c2, uint32_t Hin, uint32_t Win,
                     uint32_t Wpad, uint32_t Hpad, uint32_t C, uint32_t flags) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pPool.pipe);
        uint32_t n = (Hpad / 2) * (Wpad / 2);
        PoolPush p{a, c2, Hin, Win, Wpad, C, n, flags};
        vkCmdPushConstants(cb, pPool.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("pool", {{a, (uint64_t)Hin * Win * C * 4}, {c2, (uint64_t)Hpad * Wpad * C * 4}});
        vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
}
void M8bRec::dUpMerge(VkCommandBuffer &cb, uint64_t a, uint64_t b, uint64_t c2, uint64_t sin,
                        uint64_t cos, uint64_t h, const Stage &tgt, const Stage &low,
                        uint32_t C, uint32_t kind) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pUpM.pipe);
        UpMergePush p{a, b, c2, sin, cos, h, tgt.W, tgt.H, low.W, C, kind};
        vkCmdPushConstants(cb, pUpM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        spanTag("upmerge", {{a, (uint64_t)tgt.TOK * C * 4}, {b, (uint64_t)tgt.TOK * C * 4},
                            {c2, (uint64_t)tgt.TOK * C * 4}, {sin, 8192}, {cos, 8192},
                            {h, (uint64_t)tgt.TOK * C * 4}});
        vkCmdDispatch(cb, (tgt.TOK + 255) / 256, 1, 1);
}

void M8bRec::winGeo(const Stage &s, int oy, int ox,
                     uint32_t &padTop, uint32_t &padLeft, uint32_t &Ww, uint32_t &wp8) {
        padTop = (uint32_t)(-oy);
        padLeft = (uint32_t)(-ox);
        uint32_t hp8 = (s.H + padTop + 7) / 8;
        wp8 = (s.W + padLeft + 7) / 8;
        Ww = hp8 * wp8;
}


    // ---- window attention shared tail (per-stage geometry) -----------------
