// ============================================================================
// dlss5/chain — ChainProf implementation (see prof.h).
// ============================================================================
#include "prof.h"

#include <algorithm>
#include <cstdio>

namespace d5c {

void ChainProf::create(const VkCtx& c, const std::string& csvPath, uint32_t maxMarks) {
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = maxMarks;
    if (vkCreateQueryPool(c.dev, &qpci, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return;
    }
    cap_ = maxMarks;
    path_ = csvPath;
    evs_.reserve(4096);
    ts_.resize(maxMarks);
    overflow_ = false;
    // header once, at creation
    FILE* f = std::fopen(path_.c_str(), "w");
    if (f) {
        std::fputs("frame,idx,fam,note,m,n,k,gx,gy,gz,bytes,busy_us,idle_us,total_us\n", f);
        std::fclose(f);
    }
}

void ChainProf::destroy(VkDevice dev) {
    if (pool_) vkDestroyQueryPool(dev, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

void ChainProf::begin(VkCommandBuffer cb) {
    count_ = 0;
    evs_.clear();
    overflow_ = false;
    vkCmdResetQueryPool(cb, pool_, 0, cap_);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool_, 0);
    count_ = 1;   // slot 0 = frame start
}

void ChainProf::markBegin(VkCommandBuffer cb) {
    if (count_ >= cap_) {
        overflow_ = true;
        return;
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool_, count_++);
}

void ChainProf::markEnd(VkCommandBuffer cb, const char* fam, const char* note,
                        uint32_t m, uint32_t n, uint32_t k,
                        uint32_t gx, uint32_t gy, uint32_t gz,
                        uint64_t bytes) {
    if (count_ >= cap_) {
        overflow_ = true;
        return;
    }
    evs_.push_back({fam, note, m, n, k, gx, gy, gz, bytes});
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool_, count_++);
}

void ChainProf::mark(VkCommandBuffer cb, const char* fam, const char* note,
                     uint32_t m, uint32_t n, uint32_t k,
                     uint32_t gx, uint32_t gy, uint32_t gz,
                     uint64_t bytes) {
    markBegin(cb);
    markEnd(cb, fam, note, m, n, k, gx, gy, gz, bytes);
}

bool ChainProf::collect(VkDevice dev, float periodNs, long frameIndex) {
    if (!enabled() || evs_.empty() || count_ < 2) return false;
    VkResult qr = vkGetQueryPoolResults(dev, pool_, 0, count_,
                                        (size_t)count_ * sizeof(uint64_t), ts_.data(),
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (qr != VK_SUCCESS) {
        std::fprintf(stderr, "[prof] query results not ready (qr=%d), frame %ld skipped\n",
                     (int)qr, frameIndex);
        return false;
    }
    FILE* f = std::fopen(path_.c_str(), "a");
    if (!f) return false;

    uint64_t ts_frame_start = ts_[0];
    size_t numEvs = std::min(evs_.size(), (size_t)((count_ - 1) / 2));
    for (size_t i = 0; i < numEvs; ++i) {
        const Ev& e = evs_[i];
        uint64_t ts_begin = ts_[1 + 2 * i];
        uint64_t ts_end = ts_[2 + 2 * i];
        uint64_t ts_prev_end = (i == 0) ? ts_frame_start : ts_[2 * i];

        double busy_us = (ts_end >= ts_begin)
            ? (double)(ts_end - ts_begin) * (double)periodNs / 1e3
            : 0.0;
        double idle_us = (ts_begin >= ts_prev_end)
            ? (double)(ts_begin - ts_prev_end) * (double)periodNs / 1e3
            : 0.0;
        double total_us = busy_us + idle_us;

        std::fprintf(f, "%ld,%zu,%s,%s,%u,%u,%u,%u,%u,%u,%llu,%.2f,%.2f,%.2f\n",
                     frameIndex, i, e.fam, e.note, e.m, e.n, e.k,
                     e.gx, e.gy, e.gz, (unsigned long long)e.bytes,
                     busy_us, idle_us, total_us);
    }
    std::fclose(f);
    if (overflow_)
        std::fprintf(stderr, "[prof] WARNING: mark overflow, some dispatches not timed\n");
    return true;
}

} // namespace d5c
