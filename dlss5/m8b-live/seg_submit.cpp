// SPDX-License-Identifier: Apache-2.0
// SegSubmitter - see seg_submit.h for the driver-hazard rationale.
#include "seg_submit.h"

#include <chrono>

using clk = std::chrono::steady_clock;

int SegSubmitter::init(VkDevice dev, VkQueue queue, VkCommandPool pool, VkFence fence) {
    dev_ = dev; queue_ = queue; pool_ = pool; fence_ = fence;
    noWait_ = getenv("D5_NOWAIT") != nullptr;
    segSem_ = getenv("D5_SEM") != nullptr;
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    VK_CHECK(vkCreateSemaphore(dev_, &sci, nullptr, &tlSem_));
    return 0;
}

void SegSubmitter::shutdown() {
    if (tlSem_) vkDestroySemaphore(dev_, tlSem_, nullptr);
    tlSem_ = VK_NULL_HANDLE;
}

int SegSubmitter::begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return 0;
}

int SegSubmitter::allocBegin() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &cmd));
    return begin();
}

int SegSubmitter::submitChained() {
    VK_FATAL(vkEndCommandBuffer(cmd));
    ++segsChained;
    VkPipelineStageFlags stAll = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    uint64_t waitV = tlVal_, sigV = tlVal_ + 1;
    tssi.waitSemaphoreValueCount = 1;
    tssi.pWaitSemaphoreValues = &waitV;
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues = &sigV;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tssi};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.waitSemaphoreCount = tlVal_ ? 1 : 0;
    si.pWaitSemaphores = &tlSem_;
    si.pWaitDstStageMask = &stAll;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &tlSem_;
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
    ++tlVal_;
    segCmds_.push_back(cmd);
    return allocBegin();
}

int SegSubmitter::submitSeg() {
    if (segSem_) return submitChained();
    VK_FATAL(vkEndCommandBuffer(cmd));
    if (noWait_) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
        segCmds_.push_back(cmd);
        return allocBegin();
    }
    submitAndWait();
    VK_FATAL(vkResetCommandBuffer(cmd, 0));
    return begin();
}

double SegSubmitter::submitAndWait() {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    auto t0 = clk::now();
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, fence_));
    VK_CHECK(vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX));
    vkResetFences(dev_, 1, &fence_);
    freeSegs();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

void SegSubmitter::freeSegs() {
    if (segCmds_.empty()) return;
    vkFreeCommandBuffers(dev_, pool_, (uint32_t)segCmds_.size(), segCmds_.data());
    segCmds_.clear();
}
