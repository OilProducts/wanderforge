#include <cstdio>
#include <cstdlib>
#include "chunk.h"
#include "mesh.h"

using namespace wf;

// Forward declare mesher in namespace
namespace wf { void mesh_chunk_naive(const Chunk64&, Mesh&, float); }

int main() {
    Chunk64 c;
    // Simple test: fill a flat ground of rock at y<32, air above; add a small dirt pillar
    for (int z = 0; z < Chunk64::N; ++z) {
        for (int y = 0; y < Chunk64::N; ++y) {
            for (int x = 0; x < Chunk64::N; ++x) {
                uint16_t mat = (y < 32) ? MAT_ROCK : MAT_AIR;
                if (x == 20 && z == 20 && y < 40) mat = MAT_DIRT;
                c.set_voxel(x, y, z, mat);
            }
        }
    }

    Mesh m1, m2;
    wf::mesh_chunk_naive(c, m1, 0.10f);
    wf::mesh_chunk_greedy(c, m2, 0.10f);
    std::printf("Naive  -> Vertices: %zu, Tris: %zu\n", m1.vertices.size(), m1.indices.size() / 3);
    std::printf("Greedy -> Vertices: %zu, Tris: %zu\n", m2.vertices.size(), m2.indices.size() / 3);
    return 0;
}
