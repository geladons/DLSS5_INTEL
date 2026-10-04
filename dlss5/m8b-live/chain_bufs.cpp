// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Chain-side buffers/pipelines (see chain_bufs.h).
#include "chain_bufs.h"


// ============================================================================
uint32_t chainMemType(const VkCtx &c, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i)
        if ((c.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
    std::exit(1);
}

void chainAllocDev(const VkCtx &c, VkDeviceSize size, ChainBuf &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo mafi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &mafi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    b.size = size;
}

void chainAllocHost(const VkCtx &c, VkDeviceSize size, ChainBuf &b) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_FATAL(vkCreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chainMemType(c, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_FATAL(vkAllocateMemory(c.dev, &mai, nullptr, &b.mem));
    VK_FATAL(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    VK_FATAL(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    b.size = size;
}

void chainMakePipe(const VkCtx &c, const std::string &spv, ChainPipe &out) {
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout dsl;
    VK_FATAL(vkCreateDescriptorSetLayout(c.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_FATAL(vkCreatePipelineLayout(c.dev, &plci, nullptr, &out.layout));
    VkShaderModule mod = LoadSpv(c, spv);
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = out.layout;
    VK_FATAL(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out.pipe));
    vkDestroyShaderModule(c.dev, mod, nullptr);
    out.dsl = dsl;   // NOT destroyed: pipeline layout references it (device-lost on Arc otherwise)
}


// push blocks (scalar layout) — verbatim from m8-full-chain main.cpp
