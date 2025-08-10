#include "chunk_renderer.h"
#include "vk_utils.h"
#include "mesh.h"

#include <string>
#include <iostream>
#include <algorithm>

namespace wf {

void ChunkRenderer::init(VkPhysicalDevice phys, VkDevice device, VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir) {
    phys_ = phys; device_ = device; render_pass_ = renderPass; extent_ = extent; shader_dir_ = shaderDir ? std::string(shaderDir) : std::string();

    // Load shaders
    std::string vsPath = shader_dir_ + "/chunk.vert.spv";
    std::string fsPath = shader_dir_ + "/chunk.frag.spv";
    std::vector<std::string> vsFallbacks = {
        std::string("build/shaders/") + "chunk.vert.spv",
        std::string("shaders/") + "chunk.vert.spv",
        std::string("shaders_build/") + "chunk.vert.spv"
    };
    std::vector<std::string> fsFallbacks = {
        std::string("build/shaders/") + "chunk.frag.spv",
        std::string("shaders/") + "chunk.frag.spv",
        std::string("shaders_build/") + "chunk.frag.spv"
    };
    VkShaderModule vs = wf::vk::load_shader_module(device_, vsPath, vsFallbacks);
    VkShaderModule fs = wf::vk::load_shader_module(device_, fsPath, fsFallbacks);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        std::cout << "[info] Chunk shaders not found. Chunk rendering disabled." << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind{}; bind.binding = 0; bind.stride = sizeof(Vertex); bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R16_UINT;          attrs[2].offset = 24;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind; vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{}; vp.x = 0; vp.y = 0; vp.width = (float)extent_.width; vp.height = (float)extent_.height; vp.minDepth = 0; vp.maxDepth = 1;
    VkRect2D sc{}; sc.offset = {0,0}; sc.extent = extent_;
    VkPipelineViewportStateCreateInfo vpstate{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vpstate.viewportCount = 1; vpstate.pViewports = &vp; vpstate.scissorCount = 1; vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = sizeof(float) * 16;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return;
    }

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpstate;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.layout = layout_;
    gpi.renderPass = render_pass_;
    gpi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_) != VK_SUCCESS) {
        std::cerr << "Failed to create chunk graphics pipeline.\n";
        vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

void ChunkRenderer::recreate(VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir) {
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    init(phys_, device_, renderPass, extent, shaderDir ? shaderDir : shader_dir_.c_str());
}

void ChunkRenderer::cleanup(VkDevice device) {
    if (pipeline_) { vkDestroyPipeline(device, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_) { vkDestroyPipelineLayout(device, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (vtx_pool_) { vkDestroyBuffer(device, vtx_pool_, nullptr); vtx_pool_ = VK_NULL_HANDLE; }
    if (vtx_mem_) { vkFreeMemory(device, vtx_mem_, nullptr); vtx_mem_ = VK_NULL_HANDLE; vtx_capacity_ = 0; vtx_used_ = 0; }
    if (idx_pool_) { vkDestroyBuffer(device, idx_pool_, nullptr); idx_pool_ = VK_NULL_HANDLE; }
    if (idx_mem_) { vkFreeMemory(device, idx_mem_, nullptr); idx_mem_ = VK_NULL_HANDLE; idx_capacity_ = 0; idx_used_ = 0; }
    if (indirect_buf_) { vkDestroyBuffer(device, indirect_buf_, nullptr); indirect_buf_ = VK_NULL_HANDLE; }
    if (indirect_mem_) { vkFreeMemory(device, indirect_mem_, nullptr); indirect_mem_ = VK_NULL_HANDLE; indirect_capacity_cmds_ = 0; }
}

void ChunkRenderer::record(VkCommandBuffer cmd, const float mvp[16], const std::vector<ChunkDrawItem>& items) {
    if (!pipeline_ || items.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16, mvp);
    // If items reference the shared pools (no per-chunk buffers), build and issue indirect multi-draw
    bool pooled = true;
    for (const auto& it : items) {
        if (it.vbuf != VK_NULL_HANDLE || it.ibuf != VK_NULL_HANDLE) { pooled = false; break; }
    }
    if (pooled && vtx_pool_ && idx_pool_) {
        ensure_indirect_capacity(items.size());
        // Build commands
        using Cmd = VkDrawIndexedIndirectCommand;
        std::vector<Cmd> cmds; cmds.reserve(items.size());
        for (const auto& it : items) {
            Cmd c{ it.index_count, 1u, it.first_index, it.base_vertex, 0u };
            cmds.push_back(c);
        }
        wf::vk::upload_host_visible(device_, indirect_mem_, sizeof(Cmd) * cmds.size(), cmds.data(), 0);
        if (log_ && (++log_frame_cnt_ % log_every_n_ == 0)) {
            std::cout << "[pool] record: draws=" << cmds.size() << " vtx_used=" << (unsigned long long)vtx_used_
                      << "/" << (unsigned long long)vtx_capacity_ << " idx_used=" << (unsigned long long)idx_used_
                      << "/" << (unsigned long long)idx_capacity_ << "\n";
        }
        VkDeviceSize offs = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vtx_pool_, &offs);
        vkCmdBindIndexBuffer(cmd, idx_pool_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, indirect_buf_, 0, (uint32_t)cmds.size(), sizeof(Cmd));
    } else {
        // Fallback to direct per-chunk draws
        for (const auto& it : items) {
            VkDeviceSize offs = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &it.vbuf, &offs);
            vkCmdBindIndexBuffer(cmd, it.ibuf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, it.index_count, 1, 0, 0, 0);
        }
    }
}

bool ChunkRenderer::ensure_pool_capacity(VkDeviceSize add_vtx_bytes, VkDeviceSize add_idx_bytes) {
    // Create on first use only; afterwards, reuse via free-list without growth
    if (vtx_capacity_ == 0) {
        VkDeviceSize base_cap = (vtx_initial_cap_ > 0) ? vtx_initial_cap_ : (VkDeviceSize)(64 * 1024 * 1024);
        VkDeviceSize new_cap = std::max<VkDeviceSize>(add_vtx_bytes, base_cap);
        wf::vk::create_buffer(phys_, device_, new_cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              vtx_pool_, vtx_mem_);
        vtx_capacity_ = new_cap; vtx_used_ = 0; vtx_tail_ = 0; vtx_free_.clear();
    }
    if (idx_capacity_ == 0) {
        VkDeviceSize base_cap = (idx_initial_cap_ > 0) ? idx_initial_cap_ : (VkDeviceSize)(64 * 1024 * 1024);
        VkDeviceSize new_cap = std::max<VkDeviceSize>(add_idx_bytes, base_cap);
        wf::vk::create_buffer(phys_, device_, new_cap, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              idx_pool_, idx_mem_);
        idx_capacity_ = new_cap; idx_used_ = 0; idx_tail_ = 0; idx_free_.clear();
    }
    return true;
}

void ChunkRenderer::ensure_indirect_capacity(size_t drawCount) {
    if (indirect_capacity_cmds_ >= drawCount) return;
    size_t new_cap = std::max<size_t>(drawCount, indirect_capacity_cmds_ ? indirect_capacity_cmds_ * 2 : 1024);
    VkDeviceSize bytes = (VkDeviceSize)(new_cap * sizeof(uint32_t) * 5); // conservative
    VkBuffer nb = VK_NULL_HANDLE; VkDeviceMemory nm = VK_NULL_HANDLE;
    wf::vk::create_buffer(phys_, device_, bytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          nb, nm);
    if (indirect_mem_) { vkDestroyBuffer(device_, indirect_buf_, nullptr); vkFreeMemory(device_, indirect_mem_, nullptr); }
    indirect_buf_ = nb; indirect_mem_ = nm; indirect_capacity_cmds_ = new_cap;
}

bool ChunkRenderer::upload_mesh(const struct Vertex* vertices, size_t vcount,
                                const uint32_t* indices, size_t icount,
                                uint32_t& out_first_index,
                                int32_t& out_base_vertex) {
    VkDeviceSize vbytes = (VkDeviceSize)(vcount * sizeof(Vertex));
    VkDeviceSize ibytes = (VkDeviceSize)(icount * sizeof(uint32_t));
    if (!ensure_pool_capacity(vbytes, ibytes)) {
        out_base_vertex = 0; out_first_index = 0; return false;
    }
    VkDeviceSize voff = 0, ioff = 0;
    if (!alloc_from_pool(vbytes, sizeof(Vertex), true, voff)) return false;
    if (!alloc_from_pool(ibytes, sizeof(uint32_t), false, ioff)) { free_to_pool(voff, vbytes, true); return false; }
    out_base_vertex = (int32_t)(voff / sizeof(Vertex));
    out_first_index = (uint32_t)(ioff / sizeof(uint32_t));
    // Upload vertices
    wf::vk::upload_host_visible(device_, vtx_mem_, vbytes, vertices, voff);
    vtx_used_ = std::max(vtx_used_, voff + vbytes);
    // Upload indices
    wf::vk::upload_host_visible(device_, idx_mem_, ibytes, indices, ioff);
    idx_used_ = std::max(idx_used_, ioff + ibytes);
    if (log_) {
        std::cout << "[pool] upload: vtx off=" << (unsigned long long)voff << " bytes=" << (unsigned long long)vbytes
                  << " idx off=" << (unsigned long long)ioff << " bytes=" << (unsigned long long)ibytes << "\n";
    }
    return true;
}

bool ChunkRenderer::alloc_from_pool(VkDeviceSize bytes, VkDeviceSize alignment, bool isVertex, VkDeviceSize& out_offset) {
    auto& freev = isVertex ? vtx_free_ : idx_free_;
    VkDeviceSize& tail = isVertex ? vtx_tail_ : idx_tail_;
    VkDeviceSize capacity = isVertex ? vtx_capacity_ : idx_capacity_;
    // First-fit with alignment inside blocks
    for (size_t i = 0; i < freev.size(); ++i) {
        VkDeviceSize a = align_up(freev[i].off, alignment);
        VkDeviceSize end = freev[i].off + freev[i].size;
        if (a + bytes <= end) {
            // Allocate [a, a+bytes)
            std::vector<FreeBlock> newBlocks;
            if (a > freev[i].off) newBlocks.push_back(FreeBlock{ freev[i].off, a - freev[i].off });
            VkDeviceSize tailSize = end - (a + bytes);
            if (tailSize > 0) newBlocks.push_back(FreeBlock{ a + bytes, tailSize });
            freev.erase(freev.begin() + i);
            freev.insert(freev.begin() + i, newBlocks.begin(), newBlocks.end());
            out_offset = a;
            return true;
        }
    }
    // Allocate at tail with alignment
    VkDeviceSize a = align_up(tail, alignment);
    if (a + bytes <= capacity) {
        out_offset = a;
        tail = a + bytes;
        return true;
    }
    return false;
}

void ChunkRenderer::free_to_pool(VkDeviceSize offset, VkDeviceSize bytes, bool isVertex) {
    auto& freev = isVertex ? vtx_free_ : idx_free_;
    FreeBlock nb{ offset, bytes };
    auto it = std::lower_bound(freev.begin(), freev.end(), nb, [](const FreeBlock& a, const FreeBlock& b){ return a.off < b.off; });
    freev.insert(it, nb);
    // Coalesce
    std::vector<FreeBlock> merged;
    for (const auto& b : freev) {
        if (!merged.empty()) {
            auto& last = merged.back();
            if (last.off + last.size >= b.off) {
                VkDeviceSize newEnd = std::max(last.off + last.size, b.off + b.size);
                last.size = newEnd - last.off;
                continue;
            }
        }
        merged.push_back(b);
    }
    freev.swap(merged);
    if (log_) {
        std::cout << "[pool] free: " << (isVertex?"vtx":"idx") << " off=" << (unsigned long long)offset
                  << " size=" << (unsigned long long)bytes << " blocks=" << freev.size() << "\n";
    }
}

void ChunkRenderer::free_mesh(uint32_t first_index, uint32_t index_count,
                              int32_t base_vertex, uint32_t vertex_count) {
    if (index_count > 0) {
        VkDeviceSize off = (VkDeviceSize)first_index * sizeof(uint32_t);
        VkDeviceSize sz = (VkDeviceSize)index_count * sizeof(uint32_t);
        free_to_pool(off, sz, false);
    }
    if (vertex_count > 0) {
        VkDeviceSize off = (VkDeviceSize)base_vertex * sizeof(Vertex);
        VkDeviceSize sz = (VkDeviceSize)vertex_count * sizeof(Vertex);
        free_to_pool(off, sz, true);
    }
}
// Free-list removed: pooled memory is append-only without reuse

} // namespace wf
