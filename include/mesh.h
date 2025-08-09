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

// Meshing APIs
void mesh_chunk_naive(const struct Chunk64& c, Mesh& out, float voxel_size_m);
void mesh_chunk_greedy(const struct Chunk64& c, Mesh& out, float voxel_size_m);

} // namespace wf
