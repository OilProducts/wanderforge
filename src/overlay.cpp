#include "overlay.h"
#include "vk_utils.h"

#include <cstring>
#include <algorithm>
#include <vector>
#include <array>
#include <string>
#include <cstdio>
#include <cmath>
#include <iostream>

namespace wf {

// 6x8 bitmap font (ASCII 32..127). Each row uses 6 low bits, bit0 is leftmost pixel.
static const uint8_t WF_FONT6x8[96][8] = {
    /* 32 ' ' */ {0,0,0,0,0,0,0,0},
    /* 33 '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    /* 34 '"'*/ {0x0a,0x0a,0x0a,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x0a,0x1f,0x0a,0x0a,0x1f,0x0a,0x00,0x00},
    /* 36 '$' */ {0x04,0x1e,0x05,0x0e,0x14,0x0f,0x04,0x00},
    /* 37 '%' */ {0x03,0x13,0x08,0x04,0x02,0x19,0x18,0x00},
    /* 38 '&' */ {0x06,0x09,0x05,0x02,0x15,0x09,0x16,0x00},
    /* 39 '\''*/ {0x06,0x02,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08,0x00},
    /* 41 ')' */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02,0x00},
    /* 42 '*' */ {0x04,0x15,0x0e,0x04,0x0e,0x15,0x04,0x00},
    /* 43 '+' */ {0x00,0x04,0x04,0x1f,0x04,0x04,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x06,0x02,0x04,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x06,0x06,0x00},
    /* 47 '/' */ {0x10,0x10,0x08,0x04,0x02,0x01,0x01,0x00},
    /* 48 '0' */ {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e,0x00},
    /* 49 '1' */ {0x04,0x06,0x04,0x04,0x04,0x04,0x1f,0x00},
    /* 50 '2' */ {0x0e,0x11,0x10,0x0c,0x02,0x01,0x1f,0x00},
    /* 51 '3' */ {0x1f,0x10,0x0c,0x10,0x10,0x11,0x0e,0x00},
    /* 52 '4' */ {0x08,0x0c,0x0a,0x09,0x1f,0x08,0x08,0x00},
    /* 53 '5' */ {0x1f,0x01,0x0f,0x10,0x10,0x11,0x0e,0x00},
    /* 54 '6' */ {0x0c,0x02,0x01,0x0f,0x11,0x11,0x0e,0x00},
    /* 55 '7' */ {0x1f,0x10,0x08,0x04,0x02,0x02,0x02,0x00},
    /* 56 '8' */ {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e,0x00},
    /* 57 '9' */ {0x0e,0x11,0x11,0x1e,0x10,0x08,0x06,0x00},
    /* 58 ':' */ {0x00,0x06,0x06,0x00,0x06,0x06,0x00,0x00},
    /* 59 ';' */ {0x00,0x06,0x06,0x00,0x06,0x02,0x04,0x00},
    /* 60 '<' */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08,0x00},
    /* 61 '=' */ {0x00,0x00,0x1f,0x00,0x1f,0x00,0x00,0x00},
    /* 62 '>' */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x00},
    /* 63 '?' */ {0x0e,0x11,0x10,0x08,0x04,0x00,0x04,0x00},
    /* 64 '@' */ {0x0e,0x11,0x1d,0x15,0x1d,0x01,0x0e,0x00},
    /* 65 'A' */ {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11,0x00},
    /* 66 'B' */ {0x0f,0x11,0x11,0x0f,0x11,0x11,0x0f,0x00},
    /* 67 'C' */ {0x0e,0x11,0x01,0x01,0x01,0x11,0x0e,0x00},
    /* 68 'D' */ {0x0f,0x11,0x11,0x11,0x11,0x11,0x0f,0x00},
    /* 69 'E' */ {0x1f,0x01,0x01,0x0f,0x01,0x01,0x1f,0x00},
    /* 70 'F' */ {0x1f,0x01,0x01,0x0f,0x01,0x01,0x01,0x00},
    /* 71 'G' */ {0x0e,0x11,0x01,0x1d,0x11,0x11,0x1e,0x00},
    /* 72 'H' */ {0x11,0x11,0x11,0x1f,0x11,0x11,0x11,0x00},
    /* 73 'I' */ {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e,0x00},
    /* 74 'J' */ {0x1c,0x08,0x08,0x08,0x08,0x09,0x06,0x00},
    /* 75 'K' */ {0x11,0x09,0x05,0x03,0x05,0x09,0x11,0x00},
    /* 76 'L' */ {0x01,0x01,0x01,0x01,0x01,0x01,0x1f,0x00},
    /* 77 'M' */ {0x11,0x1b,0x15,0x11,0x11,0x11,0x11,0x00},
    /* 78 'N' */ {0x11,0x11,0x13,0x15,0x19,0x11,0x11,0x00},
    /* 79 'O' */ {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e,0x00},
    /* 80 'P' */ {0x0f,0x11,0x11,0x0f,0x01,0x01,0x01,0x00},
    /* 81 'Q' */ {0x0e,0x11,0x11,0x11,0x15,0x09,0x16,0x00},
    /* 82 'R' */ {0x0f,0x11,0x11,0x0f,0x05,0x09,0x11,0x00},
    /* 83 'S' */ {0x1e,0x01,0x01,0x0e,0x10,0x10,0x0f,0x00},
    /* 84 'T' */ {0x1f,0x04,0x04,0x04,0x04,0x04,0x04,0x00},
    /* 85 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0e,0x00},
    /* 86 'V' */ {0x11,0x11,0x11,0x11,0x11,0x0a,0x04,0x00},
    /* 87 'W' */ {0x11,0x11,0x11,0x11,0x15,0x1b,0x11,0x00},
    /* 88 'X' */ {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11,0x00},
    /* 89 'Y' */ {0x11,0x11,0x0a,0x04,0x04,0x04,0x04,0x00},
    /* 90 'Z' */ {0x1f,0x10,0x08,0x04,0x02,0x01,0x1f,0x00},
    /* 91 '[' */ {0x0e,0x02,0x02,0x02,0x02,0x02,0x0e,0x00},
    /* 92 '\\'*/ {0x01,0x01,0x02,0x04,0x08,0x10,0x10,0x00},
    /* 93 ']' */ {0x0e,0x08,0x08,0x08,0x08,0x08,0x0e,0x00},
    /* 94 '^' */ {0x04,0x0a,0x11,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0x00},
    /* 96 '`' */ {0x06,0x04,0x08,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x0e,0x10,0x1e,0x11,0x1e,0x00},
    /* 98 'b' */ {0x01,0x01,0x0f,0x11,0x11,0x11,0x0f,0x00},
    /* 99 'c' */ {0x00,0x00,0x0e,0x01,0x01,0x01,0x0e,0x00},
    /*100 'd' */ {0x10,0x10,0x1e,0x11,0x11,0x11,0x1e,0x00},
    /*101 'e' */ {0x00,0x00,0x0e,0x11,0x1f,0x01,0x0e,0x00},
    /*102 'f' */ {0x0c,0x02,0x0f,0x02,0x02,0x02,0x02,0x00},
    /*103 'g' */ {0x00,0x00,0x1e,0x11,0x11,0x1e,0x10,0x0e},
    /*104 'h' */ {0x01,0x01,0x0f,0x11,0x11,0x11,0x11,0x00},
    /*105 'i' */ {0x00,0x04,0x00,0x06,0x04,0x04,0x0e,0x00},
    /*106 'j' */ {0x00,0x08,0x00,0x0c,0x08,0x08,0x06,0x00},
    /*107 'k' */ {0x01,0x09,0x05,0x03,0x05,0x09,0x11,0x00},
    /*108 'l' */ {0x06,0x04,0x04,0x04,0x04,0x04,0x0e,0x00},
    /*109 'm' */ {0x00,0x00,0x1b,0x15,0x15,0x11,0x11,0x00},
    /*110 'n' */ {0x00,0x00,0x0f,0x11,0x11,0x11,0x11,0x00},
    /*111 'o' */ {0x00,0x00,0x0e,0x11,0x11,0x11,0x0e,0x00},
    /*112 'p' */ {0x00,0x00,0x0f,0x11,0x11,0x0f,0x01,0x01},
    /*113 'q' */ {0x00,0x00,0x1e,0x11,0x11,0x1e,0x10,0x10},
    /*114 'r' */ {0x00,0x00,0x0d,0x13,0x01,0x01,0x01,0x00},
    /*115 's' */ {0x00,0x00,0x1e,0x01,0x0e,0x10,0x0f,0x00},
    /*116 't' */ {0x02,0x02,0x0f,0x02,0x02,0x02,0x0c,0x00},
    /*117 'u' */ {0x00,0x00,0x11,0x11,0x11,0x11,0x1e,0x00},
    /*118 'v' */ {0x00,0x00,0x11,0x11,0x11,0x0a,0x04,0x00},
    /*119 'w' */ {0x00,0x00,0x11,0x11,0x15,0x1b,0x11,0x00},
    /*120 'x' */ {0x00,0x00,0x11,0x0a,0x04,0x0a,0x11,0x00},
    /*121 'y' */ {0x00,0x00,0x11,0x11,0x1e,0x10,0x0e,0x00},
    /*122 'z' */ {0x00,0x00,0x1f,0x08,0x04,0x02,0x1f,0x00},
    /*123 '{' */ {0x0c,0x04,0x04,0x03,0x04,0x04,0x0c,0x00},
    /*124 '|' */ {0x04,0x04,0x04,0x00,0x04,0x04,0x04,0x00},
    /*125 '}' */ {0x03,0x04,0x04,0x18,0x04,0x04,0x03,0x00},
    /*126 '~' */ {0x08,0x15,0x02,0x00,0x00,0x00,0x00,0x00},
    /*127     */ {0,0,0,0,0,0,0,0}
};

void OverlayRenderer::init(VkPhysicalDevice phys, VkDevice dev, VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir) {
    phys_ = phys; device_ = dev; extent_ = extent;
    // Load shaders
    auto read_all = [](const std::string& p, std::vector<char>& out)->bool{ FILE* f = fopen(p.c_str(), "rb"); if(!f) return false; fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET); out.resize(len); size_t rd=fread(out.data(),1,out.size(),f); fclose(f); return rd==out.size(); };
    auto load_shader = [&](const std::string& path)->VkShaderModule{
        std::vector<char> buf; if (!read_all(path, buf)) return VK_NULL_HANDLE; VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = buf.size(); ci.pCode = reinterpret_cast<const uint32_t*>(buf.data()); VkShaderModule m{}; if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE; return m; };
    std::string vsPath = std::string(shaderDir) + "/overlay.vert.spv";
    std::string fsPath = std::string(shaderDir) + "/overlay.frag.spv";
    VkShaderModule vs = load_shader(vsPath); VkShaderModule fs = load_shader(fsPath);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        std::cout << "[info] Overlay shaders not found. HUD disabled." << std::endl;
        return;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind{}; bind.binding = 0; bind.stride = sizeof(float) * 6; bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = sizeof(float) * 2;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind; vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{}; vp.x = 0; vp.y = (float)extent.height; vp.width = (float)extent.width; vp.height = -(float)extent.height; vp.minDepth = 0; vp.maxDepth = 1;
    VkRect2D sc{}; sc.offset = {0,0}; sc.extent = extent;
    VkPipelineViewportStateCreateInfo vpstate{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vpstate.viewportCount = 1; vpstate.pViewports = &vp; vpstate.scissorCount = 1; vpstate.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_TRUE; cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD; cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr); vkDestroyShaderModule(device_, fs, nullptr); return;
    }
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = 2; gpi.pStages = stages; gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vpstate; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.layout = pipeline_layout_; gpi.renderPass = renderPass; gpi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_) != VK_SUCCESS) {
        std::cerr << "Failed to create overlay graphics pipeline.\n";
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE;
    } else {
        std::cout << "[info] HUD overlay enabled (overlay pipeline created)." << std::endl;
    }
    vkDestroyShaderModule(device_, vs, nullptr); vkDestroyShaderModule(device_, fs, nullptr);
}

void OverlayRenderer::recreate_swapchain(VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir) {
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    extent_ = extent;
    init(phys_, device_, renderPass, extent, shaderDir);
}

void OverlayRenderer::cleanup(VkDevice dev) {
    if (pipeline_) { vkDestroyPipeline(dev, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    for (size_t i=0;i<kFrames;++i) {
        if (vbuf_[i]) vkDestroyBuffer(dev, vbuf_[i], nullptr);
        if (vmem_[i]) vkFreeMemory(dev, vmem_[i], nullptr);
        vbuf_[i]=VK_NULL_HANDLE; vmem_[i]=VK_NULL_HANDLE; capacity_bytes_[i]=0; vertex_count_[i]=0;
    }
}

uint32_t OverlayRenderer::find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags properties) {
    return wf::vk::find_memory_type(phys_, typeBits, properties);
}

void OverlayRenderer::create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, const void* data) {
    wf::vk::create_buffer(phys_, device_, size, usage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buf, mem);
    if (data) wf::vk::upload_host_visible(device_, mem, size, data, 0);
}

void OverlayRenderer::build_text(size_t frameSlot, const char* text, int W, int H) {
    struct OV { float x, y, r, g, b, a; };
    std::vector<OV> verts;
    const int ch_w = 6, ch_h = 8; const float scale = 2.0f;
    const float x0 = 6.0f, y0 = 6.0f, xr = 6.0f; // margins in pixels
    auto to_ndc = [&](float px, float py){ float xn = (px / (float)W) * 2.0f - 1.0f; float yn = (py / (float)H) * 2.0f - 1.0f; return std::array<float,2>{{xn, yn}}; };
    auto quad = [&](float x, float y, float w, float h, float r, float g, float b, float a){ auto p0 = to_ndc(x, y); auto p1 = to_ndc(x + w, y); auto p2 = to_ndc(x + w, y + h); auto p3 = to_ndc(x, y + h); verts.push_back(OV{p0[0], p0[1], r,g,b,a}); verts.push_back(OV{p1[0], p1[1], r,g,b,a}); verts.push_back(OV{p2[0], p2[1], r,g,b,a}); verts.push_back(OV{p0[0], p0[1], r,g,b,a}); verts.push_back(OV{p2[0], p2[1], r,g,b,a}); verts.push_back(OV{p3[0], p3[1], r,g,b,a}); };

    // Split incoming text on newlines and render each line separately with optional ellipsis per line
    std::vector<std::string> lines;
    {
        const char* p = text;
        const int cap = 512; // safety cap per line build
        std::string cur;
        cur.reserve(128);
        int total = 0;
        while (*p && total < cap) {
            if (*p == '\n') { lines.push_back(cur); cur.clear(); ++p; continue; }
            cur.push_back(*p++);
            ++total;
        }
        lines.push_back(cur);
    }

    int char_px = (int)std::ceil(ch_w * scale);
    int max_fit = 0;
    if (W > (x0 + xr) && char_px > 0) {
        max_fit = (int)std::floor((W - x0 - xr) / (float)char_px);
        if (max_fit < 0) max_fit = 0;
    }
    const float line_height = ch_h * scale + 4.0f; // small spacing between lines
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& src = lines[li];
        std::string line;
        if ((int)src.size() <= max_fit) {
            line = src;
        } else {
            int keep = std::max(0, max_fit - 3);
            if (keep > 0) { line.assign(src.data(), keep); line += "..."; }
            else { line.clear(); }
        }
        for (int ci = 0; ci < (int)line.size(); ++ci) {
            unsigned char ch = (unsigned char)line[ci];
            if (ch < 32 || ch > 127) ch = 32;
            const uint8_t* rows = WF_FONT6x8[ch - 32];
            for (int ry = 0; ry < ch_h; ++ry) {
                uint8_t bits = rows[ry];
                for (int rx = 0; rx < ch_w; ++rx) if (bits & (1u << rx)) {
                    float px = x0 + (ci * ch_w + rx) * scale;
                    float py = y0 + (float)li * line_height + ry * scale;
                    quad(px, py, scale, scale, 1,1,1,1);
                }
            }
        }
    }
    VkDeviceSize bytes = (VkDeviceSize)(verts.size() * sizeof(OV));
    if (bytes == 0) { vertex_count_[frameSlot] = 0; return; }
    if (capacity_bytes_[frameSlot] < bytes) {
        if (vbuf_[frameSlot]) { vkDestroyBuffer(device_, vbuf_[frameSlot], nullptr); vbuf_[frameSlot] = VK_NULL_HANDLE; }
        if (vmem_[frameSlot]) { vkFreeMemory(device_, vmem_[frameSlot], nullptr); vmem_[frameSlot] = VK_NULL_HANDLE; }
        capacity_bytes_[frameSlot] = std::max<VkDeviceSize>(bytes, 64 * 1024);
        create_host_buffer(capacity_bytes_[frameSlot], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbuf_[frameSlot], vmem_[frameSlot], nullptr);
    }
    void* p = nullptr; vkMapMemory(device_, vmem_[frameSlot], 0, capacity_bytes_[frameSlot], 0, &p); std::memcpy(p, verts.data(), (size_t)bytes); vkUnmapMemory(device_, vmem_[frameSlot]);
    vertex_count_[frameSlot] = (uint32_t)(bytes / (sizeof(float) * 6));
}

void OverlayRenderer::record_draw(VkCommandBuffer cmd, size_t frameSlot) {
    if (!pipeline_ || vertex_count_[frameSlot] == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkDeviceSize offs = 0; VkBuffer vb = vbuf_[frameSlot];
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offs);
    vkCmdDraw(cmd, vertex_count_[frameSlot], 1, 0, 0);
}

} // namespace wf
