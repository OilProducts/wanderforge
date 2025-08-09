#pragma once

#include <cstdint>
#include <vector>

namespace wf {

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    uint16_t mat;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

} // namespace wf

