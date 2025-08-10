#include "mesh.h"
#include "chunk.h"
#include "wf_math.h"

namespace wf {

static inline void add_quad(Mesh& m, float x, float y, float z, float w, float h,
                            Float3 u, Float3 v, Float3 n, uint16_t mat) {
    const uint32_t base = (uint32_t)m.vertices.size();
    Float3 p0 = {x, y, z};
    Float3 p1 = {x + u.x * w, y + u.y * w, z + u.z * w};
    Float3 p2 = {p1.x + v.x * h, p1.y + v.y * h, p1.z + v.z * h};
    Float3 p3 = {x + v.x * h, y + v.y * h, z + v.z * h};

    Vertex v0{p0.x, p0.y, p0.z, n.x, n.y, n.z, mat};
    Vertex v1{p1.x, p1.y, p1.z, n.x, n.y, n.z, mat};
    Vertex v2{p2.x, p2.y, p2.z, n.x, n.y, n.z, mat};
    Vertex v3{p3.x, p3.y, p3.z, n.x, n.y, n.z, mat};
    m.vertices.push_back(v0);
    m.vertices.push_back(v1);
    m.vertices.push_back(v2);
    m.vertices.push_back(v3);
    // two triangles
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 3);
}

// Naive mesher: emits a face for any solid voxel face that borders air or a different material
void mesh_chunk_naive(const Chunk64& c, Mesh& out, float voxel_size_m) {
    out.vertices.clear(); out.indices.clear();
    const int N = Chunk64::N;
    for (int z = 0; z < N; ++z) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                if (!c.is_solid(x, y, z)) continue;
                uint16_t mat = c.get_material(x, y, z);
                // neighbor helper
                auto neighbor = [&](int nx, int ny, int nz)->bool{
                    // Treat out-of-chunk as having the same material to avoid exposing chunk-edge walls
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= N || ny >= N || nz >= N) return true;
                    if (!c.is_solid(nx, ny, nz)) return false;
                    return c.get_material(nx, ny, nz) == mat;
                };
                float X = x * voxel_size_m;
                float Y = y * voxel_size_m;
                float Z = z * voxel_size_m;
                float s = voxel_size_m;
                // -X
                if (!neighbor(x - 1, y, z)) add_quad(out, X, Y, Z, s, s, {0,0,1}, {0,1,0}, {-1,0,0}, mat);
                // +X
                if (!neighbor(x + 1, y, z)) add_quad(out, X + s, Y, Z, s, s, {0,1,0}, {0,0,1}, {1,0,0}, mat);
                // -Y
                if (!neighbor(x, y - 1, z)) add_quad(out, X, Y, Z, s, s, {1,0,0}, {0,0,1}, {0,-1,0}, mat);
                // +Y
                if (!neighbor(x, y + 1, z)) add_quad(out, X, Y + s, Z, s, s, {0,0,1}, {1,0,0}, {0,1,0}, mat);
                // -Z
                if (!neighbor(x, y, z - 1)) add_quad(out, X, Y, Z, s, s, {1,0,0}, {0,1,0}, {0,0,-1}, mat);
                // +Z
                if (!neighbor(x, y, z + 1)) add_quad(out, X, Y, Z + s, s, s, {0,1,0}, {1,0,0}, {0,0,1}, mat);
            }
        }
    }
}

} // namespace wf
