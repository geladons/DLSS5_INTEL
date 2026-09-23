// ============================================================================
// dlss5/chain — ChainProf implementation (see prof.h).
// ============================================================================
#include "prof.h"

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
    evs_.reserve(2048);
    ts_.resize(maxMarks);
    // header once, at creation
    FILE* f = std::fopen(path_.c_str(), "w");
    if (f) {
        std::fputs("frame,fam,note,m,n,k,gx,gy,gz,us\n", f);
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
    vkCmdResetQueryPool(cb, pool_, 0, cap_);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool_, 0);
    count_ = 1;   // slot 0 = frame start
}

void ChainProf::mark(VkCommandBuffer cb, const char* fam, const char* note,
                     uint32_t m, uint32_t n, uint32_t k,
                     uint32_t gx, uint32_t gy, uint32_t gz) {
    if (count_ >= cap_) {
        overflow_ = true;
        return;
    }
    evs_.push_back({fam, note, m, n, k, gx, gy, gz});
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool_, count_++);
}

bool ChainProf::collect(VkDevice dev, float periodNs, long frameIndex) {
    if (!enabled() || count_ < 2) return false;
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
    // evs_[i] sits between timestamps i and i+1 -> its us = delta(i, i+1)
    for (size_t i = 0; i < evs_.size(); ++i) {
        const Ev& e = evs_[i];
        double us = (double)(ts_[i + 1] - ts_[i]) * (double)periodNs / 1e3;
        std::fprintf(f, "%ld,%s,%s,%u,%u,%u,%u,%u,%u,%.2f\n",
                     frameIndex, e.fam, e.note, e.m, e.n, e.k,
                     e.gx, e.gy, e.gz, us);
    }
    std::fclose(f);
    if (overflow_)
        std::fprintf(stderr, "[prof] WARNING: mark overflow, some dispatches not timed\n");
    return true;
}

} // namespace d5c
