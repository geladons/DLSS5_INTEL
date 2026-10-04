// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - ChainEngine frame-1 debug readback + dumps (see engine.h).
// ============================================================================
#include "engine.h"
#include "engine_dbg.h"

namespace d5c {

void ChainEngine::recordDebugCopies() {
    const ArenaOffsets& O = arena_.off;
    DbgLayout dl = dbgLayout(st_, T6P_);
    // M9b: sources live in different arena chunks -> one copy per entry
    // (every slot sits whole inside its chunk by construction).
    struct DbgCp { VkDeviceSize srcOff, dstOff, size; };
    DbgCp dcps[15] = {
        {O.oFeatV, dl.offFeat, (uint64_t)st_[0].TOK * 16 * 4},
        {O.oHEAD, dl.offHead, (uint64_t)st_[0].TOK * 4 * 4},
        {O.oX16, dl.offX16, (uint64_t)st_[0].TOK * 16 * 2},
        {O.oADA, dl.offADA, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oMRG70, dl.offMRG, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oG2, dl.offG2, (uint64_t)st_[0].TOK * 32 * 4},
        {O.oHeadW, dl.offHW, 1024},
        {O.oB4DS, dl.offB4, (uint64_t)st_[2].TOK * 64 * 2},
        {O.oB22DS, dl.offB22, (uint64_t)st_[5].TOK * 512 * 2},
        {O.oB38, dl.offB38, (uint64_t)T6P_ * 1024 * 2},
        {O.oB48, dl.offB48, (uint64_t)st_[4].TOK * 256 * 2},
        {O.oB69, dl.offB69, (uint64_t)st_[1].TOK * 32 * 2},
        {arena_.vecOff["block70.layer0.inp_merge_sin"], dl.offSin, 32 * 4},
        {arena_.vecOff["block70.layer0.inp_merge_cos"], dl.offCos, 32 * 4},
        {O.oFRS, dl.offFRS, (uint64_t)st_[0].TOK * 32 * 2},
    };
    for (auto& e : dcps) {
        VkBufferCopy c1{arena_.localOff(e.srcOff), e.dstOff, e.size};
        vkCmdCopyBuffer(cmd_, arena_.bufOf(e.srcOff), bufDbg_, 1, &c1);
    }
    VkMemoryBarrier db{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; db.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &db, 0, nullptr, 0, nullptr);
}

void ChainEngine::writeDebugDumps() {
    DbgLayout dl = dbgLayout(st_, T6P_);
    void* pd = nullptr;
    D5C_VK_FATAL(vkMapMemory(vkc_.vk.dev, memDbg_, 0, VK_WHOLE_SIZE, 0, &pd));
    const char* db = (const char*)pd;
    auto wr = [&](const char* name, const void* p, uint64_t bytes) {
        std::string path = cfg_.dumpDir + "\\" + name;
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(p, 1, (size_t)bytes, f); fclose(f); }
    };
    const uint64_t T0 = st_[0].TOK;
    wr("live_featV.bin", db + dl.offFeat, (uint64_t)T0 * 16 * 4);
    {   // oHEAD is stored compact [TOK,4] fp32 on device; expand the dump
        // back to the legacy [TOK,16] layout (ch 4..15 zero) so every
        // existing reader (m11d --selftest, _validate.py, tools/*) keeps
        // the same file format and stride.
        std::vector<float> head16((size_t)T0 * 16, 0.0f);
        const float* h4 = (const float*)(db + dl.offHead);
        for (uint64_t t = 0; t < T0; ++t)
            for (uint32_t c = 0; c < 4; ++c)
                head16[t * 16 + c] = h4[t * 4 + c];
        wr("live_head.bin", head16.data(), (uint64_t)T0 * 16 * 4);
    }
    wr("live_x16.bin", db + dl.offX16, (uint64_t)T0 * 16 * 2);
    wr("live_ada.bin", db + dl.offADA, (uint64_t)T0 * 32 * 4);
    wr("live_mrg70.bin", db + dl.offMRG, (uint64_t)T0 * 32 * 4);
    wr("live_g2.bin", db + dl.offG2, (uint64_t)T0 * 32 * 4);
    wr("live_b4ds.f16", db + dl.offB4, (uint64_t)st_[2].TOK * 64 * 2);
    wr("live_b22ds.f16", db + dl.offB22, (uint64_t)st_[5].TOK * 512 * 2);
    wr("live_b38.f16", db + dl.offB38, (uint64_t)T6P_ * 1024 * 2);
    wr("live_b48.f16", db + dl.offB48, (uint64_t)st_[4].TOK * 256 * 2);
    wr("live_b69.f16", db + dl.offB69, (uint64_t)st_[1].TOK * 32 * 2);
    wr("live_frs.f16", db + dl.offFRS, (uint64_t)T0 * 32 * 2);
    vkUnmapMemory(vkc_.vk.dev, memDbg_);
    std::printf("[dbg] frame-1 chain-path dumps written to %s\\live_*\n", cfg_.dumpDir.c_str());
}
} // namespace d5c
