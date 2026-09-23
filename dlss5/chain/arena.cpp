// ============================================================================
// dlss5/chain — ChainArena implementation (see arena.h).
// ============================================================================
#include "arena.h"

#include <algorithm>

namespace d5c {

static uint32_t chainMemType(const VkCtx& c, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i)
        if ((c.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
    std::exit(1);
}

void chainAllocDev(const VkCtx& c, VkDeviceSize size, ChainBuf& b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    D5C_VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo mafi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &mafi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    D5C_VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    D5C_VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    b.size = size;
}

void chainAllocHost(const VkCtx& c, VkDeviceSize size, ChainBuf& b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    D5C_VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    D5C_VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    D5C_VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    D5C_VK_FATAL(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    b.size = size;
}

void ChainArena::layout(const Stage st[7], uint64_t T6P, uint64_t packTotalIn,
                        const WeightsStore& ws) {
    struct Ar {
        VkDeviceSize base = 0;
        VkDeviceSize off = 0;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> segs;   // (abs off, bytes) for chunking
        VkDeviceSize alloc(VkDeviceSize bytes) {
            VkDeviceSize a = (off + 255) & ~(VkDeviceSize)255;
            off = a + bytes;
            segs.push_back({base + a, bytes});
            return base + a;
        }
    } ar;
    ar.base = packTotalIn;
    vecOff.clear();
    auto needVecL = [&](const std::string& n) {
        if (!vecOff.count(n)) { VkDeviceSize o = ar.alloc(4096); vecOff[n] = o; }
        return vecOff[n];
    };
    off.oHeadW = ar.alloc(1024);   // folded [32,16] f16 head weight

    auto slot = [&](VkDeviceSize bytes) { return ar.alloc(bytes); };

    // per-scale maxima (window scales s0..s5: C = 32,32,64,128,256,512; heads = C/32)
    const uint64_t T6 = st[6].TOK;
    uint64_t maxTokC = 0, maxTok4C = 0;
    const uint32_t stageC[6] = {32, 32, 64, 128, 256, 512};
    for (int s = 0; s < 6; ++s) {
        maxTokC = std::max(maxTokC, (uint64_t)st[s].TOK * stageC[s]);
        maxTok4C = std::max(maxTok4C, (uint64_t)st[s].TOK * 4 * stageC[s]);
    }
    maxTokC = std::max(maxTokC, T6 * 1024);
    uint64_t wWin = 0, wProj = 0, wQkv = 0, wSc = 0;
    for (int s = 0; s < 6; ++s) {
        uint32_t hp8 = (st[s].H + 4 + 7) / 8, wp8 = (st[s].W + 4 + 7) / 8;
        uint64_t Ww = (uint64_t)hp8 * wp8, Hd = stageC[s] / 32;
        wWin = std::max(wWin, Ww * 64 * stageC[s]);
        wProj = std::max(wProj, Ww * 64 * 3 * stageC[s]);
        wQkv = std::max(wQkv, Ww * Hd * 64 * 32);
        wSc = std::max(wSc, Ww * Hd * 64 * 64);
    }

    off.oX16 = slot((uint64_t)st[0].TOK * 16 * 2);
    off.oADA = slot((uint64_t)st[0].TOK * 32 * 4);
    off.oG1 = slot(maxTokC * 4);
    off.oG2 = slot(maxTokC * 4);
    off.oG3 = slot(maxTok4C * 4);
    off.oG4 = slot(maxTokC * 4);
    off.oRAW = slot(maxTokC * 4);
    off.oPOOL = slot(maxTokC * 2);
    // window attention set
    off.oWIN = slot(wWin * 2);
    off.oPROJW = slot(wProj * 4);
    off.oQW = slot(wQkv * 2); off.oKW = slot(wQkv * 2); off.oVW = slot(wQkv * 2);
    off.oSCW = slot(wSc * 4); off.oPRW = slot(wSc * 2);
    off.oMGW = slot(wQkv * 4); off.oATW = slot(wWin * 2); off.oABW = slot(wWin * 4);
    // global set (L6); token count padded to T6P (align 32) for the coopmat
    // score GEMM — softmax masks cols >= T6 (nReal), padded rows carry zeros.
    off.oHG = slot((uint64_t)T6P * 4096 * 2);
    off.oPROJG = slot((uint64_t)T6P * 3072 * 4);
    off.oQG = slot((uint64_t)T6P * 1024 * 2); off.oKG = slot((uint64_t)T6P * 1024 * 2);
    off.oVG = slot((uint64_t)T6P * 1024 * 2);
    off.oSCG = slot(32ull * T6P * T6P * 4); off.oPRG = slot(32ull * T6P * T6P * 2);
    off.oMGG = slot((uint64_t)T6P * 1024 * 4); off.oATG = slot((uint64_t)T6P * 1024 * 2);
    off.oABG = slot((uint64_t)T6P * 1024 * 4); off.oBRG = slot((uint64_t)T6P * 1024 * 4);
    // boundary / skip slots
    off.oB0RAW = slot((uint64_t)st[0].TOK * 32 * 4); off.oFRS = slot((uint64_t)st[0].TOK * 32 * 2);
    off.oSKIP0 = slot((uint64_t)st[1].TOK * 32 * 2); off.oSKIP1 = slot((uint64_t)st[2].TOK * 64 * 2);
    off.oSKIP2 = slot((uint64_t)st[3].TOK * 128 * 2); off.oSKIP3 = slot((uint64_t)st[4].TOK * 256 * 2);
    off.oSS = slot((uint64_t)st[5].TOK * 512 * 2);
    off.oB4DS = slot((uint64_t)st[2].TOK * 64 * 2); off.oB22DS = slot((uint64_t)st[5].TOK * 512 * 2);
    off.oBRIDGE = slot((uint64_t)T6P * 1024 * 2);
    off.oB38 = slot((uint64_t)T6P * 1024 * 2); off.oB39 = slot((uint64_t)st[5].TOK * 512 * 2);
    off.oB48 = slot((uint64_t)st[4].TOK * 256 * 2);
    off.oB69 = slot((uint64_t)st[1].TOK * 32 * 2);
    off.oMRG39 = slot((uint64_t)st[5].TOK * 512 * 4);
    off.oMRG70 = slot((uint64_t)st[0].TOK * 32 * 4); off.oHEAD = slot((uint64_t)st[0].TOK * 16 * 4);
    // m8b bridge slots: features output (fp32 [TOK0,16]) + head row-DC scratch
    // (kept allocated: descriptor bindings 3/15 reference these ranges)
    off.oFeatV = slot((uint64_t)st[0].TOK * 16 * 4);
    off.oHeadDC = slot(1024);

    auto preVec = [&](const std::string& n) { if (ws.poff.count(n)) needVecL(n); };
    for (int i = 0; i <= 70; ++i) {
        if (i == 39) { preVec("block39.layer0.inp_upsample_sin"); continue; }
        if (i >= 31 && i <= 38) {
            preVec(WeightsStore::tname("block%d.layer1.ffn_cos_skip", i));
            preVec(WeightsStore::tname("block%d.layer4.attn_cos_skip", i));
            needVecL(WeightsStore::tname("block%d.layer2.attn_scale.q32", i));
            continue;
        }
        if ((i >= 23 && i <= 30) || (i >= 40 && i <= 47)) {
            preVec(WeightsStore::tname("block%d.layer1.ffn_cos_skip", i));
            preVec(WeightsStore::tname("block%d.layer3.attn_cos_skip", i));
        } else {
            preVec(WeightsStore::tname("block%d.layer0.ffn_cos_skip", i));
            preVec(WeightsStore::tname("block%d.layer0.attn_cos_skip", i));
        }
        if (i == 48 || i == 56 || i == 62 || i == 66)
            preVec(WeightsStore::tname("block%d.layer0.sin", i));
        if (i == 70) {
            preVec("block70.layer0.inp_merge_sin");
            preVec("block70.layer0.inp_merge_cos");
        }
    }

    scratchBytes = ar.off;
    totalBytes = packTotalIn + ar.off;
    std::printf("arena: %.2f MB scratch after weights; total %.1f MB\n",
                ar.off / 1048576.0, totalBytes / 1048576.0);

    // Chunked split at slot boundaries (the weights pack rides chunk 0).
    const VkDeviceSize CHUNK_CAP = 3500ull * 1024 * 1024;
    chunks.clear();
    {
        VkDeviceSize curStart = 0, curEnd = packTotalIn;
        for (auto& sg : ar.segs) {
            if (sg.first + sg.second - curStart > CHUNK_CAP && sg.first > curStart) {
                chunks.push_back({curStart, curEnd, {}, 0});
                curStart = sg.first;
            }
            curEnd = sg.first + sg.second;
        }
        chunks.push_back({curStart, curEnd, {}, 0});
    }
}

void ChainArena::allocDevice(const VkCtx& c) {
    for (auto& ch : chunks) chainAllocDev(c, ch.end - ch.start, ch.b);
    std::printf("[mem] arena chunked: %zu device buffers (cap %.2f GiB):", chunks.size(),
                3500ull * 1024 * 1024 / 1073741824.0);
    for (auto& ch : chunks) std::printf("  %.2f MB", (ch.end - ch.start) / 1048576.0);
    std::printf("\n");
    for (auto& ch : chunks) {
        VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, ch.b.buf};
        ch.baseA = vkGetBufferDeviceAddress(c.dev, &bdai);
    }
}

void ChainArena::freeDevice(const VkCtx& c) {
    for (auto& ch : chunks) {
        if (ch.b.buf) vkDestroyBuffer(c.dev, ch.b.buf, nullptr);
        if (ch.b.mem) vkFreeMemory(c.dev, ch.b.mem, nullptr);
        ch.b = ChainBuf{};
        ch.baseA = 0;
    }
}

void ChainArena::upload(const VkCtx& c, VkCommandPool pool, VkFence fence, const char* hostImg) const {
    for (auto& ch : chunks) {   // per-chunk staging upload (alloc cap)
        ChainBuf stg;
        chainAllocHost(c, ch.end - ch.start, stg);
        std::memcpy(stg.mapped, hostImg + ch.start, (size_t)(ch.end - ch.start));
        VkCommandBuffer up = BeginOneShot(c, pool);
        VkBufferCopy cp1{0, 0, ch.end - ch.start};
        vkCmdCopyBuffer(up, stg.buf, ch.b.buf, 1, &cp1);
        SubmitOneShot(c, pool, fence, up);
        if (stg.mapped) vkUnmapMemory(c.dev, stg.mem);
        vkDestroyBuffer(c.dev, stg.buf, nullptr);
        vkFreeMemory(c.dev, stg.mem, nullptr);
    }
}

int ChainArena::chunkOf(VkDeviceSize o) const {
    for (int i = 0; i < (int)chunks.size(); i++)
        if (o >= chunks[i].start && o < chunks[i].end) return i;
    return (int)chunks.size() - 1;
}

} // namespace d5c
