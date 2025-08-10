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
// Neighbor-aware greedy mesher to correctly close seams across chunk boundaries without emitting outer walls.
// Neighbor pointers may be null; when null, boundaries toward that neighbor are treated as seamless (no faces emitted).
void mesh_chunk_greedy_neighbors(const struct Chunk64& c,
                                 const struct Chunk64* negX, const struct Chunk64* posX,
                                 const struct Chunk64* negY, const struct Chunk64* posY,
                                 const struct Chunk64* negZ, const struct Chunk64* posZ,
                                 Mesh& out, float voxel_size_m);

} // namespace wf
