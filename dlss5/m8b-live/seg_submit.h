// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - SegSubmitter: splits one frame's command stream into
// queue-ordered segments chained on a timeline semaphore.
//
// Why this exists (B50 driver bug, reproduced 2026-10-03/04): on the Arc Pro
// B50 driver 101.8805, co-submitting producer->consumer compute dispatch
// pairs in a single vkQueueSubmit can hang the GPU (igfxnd TDR ->
// VK_ERROR_DEVICE_LOST at a later submit). In-buffer ordering primitives do
// NOT help: VkBufferMemoryBarrier, VkMemoryBarrier, ALL_COMMANDS barriers,
// events and timestamps were all tried and the device still died. The only
// mechanism that enforces real execution ordering on this driver is a
// timeline-semaphore dependency between separate submissions.
//
// Usage: the recorder writes ops into `cmd`; where a producer->consumer
// boundary must be honored, it calls a separator (bar()/barrierAll() in
// main.cpp). Inside a hazard zone (hazZone) those separators call
// submitChained(), which ends the buffer, submits it waiting on the
// previous segment's timeline value, signals the next, and continues into a
// fresh buffer - GPU-side serialization with no CPU fence stall.
// ============================================================================
#pragma once
#include "vk_util.h"

class SegSubmitter {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;   // current recording buffer
    bool hazZone = false;                   // separators become chained submits
    long segsChained = 0;                   // chained submits this frame

    // dev/queue/pool/fence are borrowed (owned by the caller).
    int  init(VkDevice dev, VkQueue queue, VkCommandPool pool, VkFence fence);
    void shutdown();

    int  begin();          // vkBeginCommandBuffer on cmd
    int  allocBegin();     // allocate a fresh primary buffer into cmd + begin
    int  submitChained();  // end + timeline-chained submit + fresh buffer

    // Diagnostic segment flush (D5_NOWAIT / D5_SEM env modes).
    int  submitSeg();

    // Final frame submit on the caller's fence + full wait; frees recycled
    // segment buffers (queue is in-order, so the fence covers them all).
    double submitAndWait();
    void   freeSegs();       // same recycle free, for a caller-side fence wait

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore tlSem_ = VK_NULL_HANDLE;
    uint64_t tlVal_ = 0;
    std::vector<VkCommandBuffer> segCmds_;   // submitted segs, freed after fence
    bool noWait_ = false;                    // D5_NOWAIT diagnostic
    bool segSem_ = false;                    // D5_SEM diagnostic
};
