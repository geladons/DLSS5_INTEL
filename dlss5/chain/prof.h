// ============================================================================
// dlss5/chain — ChainProf: optional per-dispatch GPU profiling (M10).
// One timestamp per dispatched kernel at COMPUTE_SHADER stage; deltas between
// consecutive timestamps give per-dispatch ms in the (barrier-serialized)
// chain stream. Shape/grid metadata is recorded CPU-side in record order and
// married with the timestamps after the frame fence -> CSV for offline
// bucketing. Completely inert unless enabled (every call is a null check),
// so the production daemon path pays nothing.
// ============================================================================
#pragma once

#include "vk_util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace d5c {

class ChainProf {
public:
    void create(const VkCtx& c, const std::string& csvPath, uint32_t maxMarks = 16384);
    void destroy(VkDevice dev);

    bool enabled() const { return pool_ != VK_NULL_HANDLE; }

    // Reset the pool and write the frame-start timestamp. Call inside the
    // frame command buffer, before the first dispatch.
    void begin(VkCommandBuffer cb);
    // Boundary AFTER a dispatch. fam/note must be string literals (only the
    // pointer is stored). m/n/k = GEMM dims or 0; gx/gy/gz = dispatch grid.
    void mark(VkCommandBuffer cb, const char* fam, const char* note,
              uint32_t m, uint32_t n, uint32_t k,
              uint32_t gx, uint32_t gy, uint32_t gz);
    // After the frame fence: fetch timestamps, append CSV rows.
    bool collect(VkDevice dev, float periodNs, long frameIndex);

private:
    struct Ev {
        const char* fam;
        const char* note;
        uint32_t m, n, k, gx, gy, gz;
    };

    VkQueryPool pool_ = VK_NULL_HANDLE;
    uint32_t cap_ = 0;
    uint32_t count_ = 0;          // marks written into the current frame
    bool overflow_ = false;
    std::string path_;
    std::vector<Ev> evs_;
    std::vector<uint64_t> ts_;    // scratch for query results
};

} // namespace d5c
