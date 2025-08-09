#include <cstdio>
#include <cstdlib>
#include "chunk.h"
#include "mesh.h"

using namespace wf;

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

    Mesh m;
    extern void mesh_chunk_naive(const Chunk64&, Mesh&, float);
    mesh_chunk_naive(c, m, 0.10f);
    std::printf("Vertices: %zu, Triangles: %zu\n", m.vertices.size(), m.indices.size() / 3);
    return 0;
}

