#include <cstdio>
#include <cstdlib>
#include <string>
#include "region_io.h"
#include "mesh.h"

using namespace wf;

int main(int argc, char** argv) {
    int face = 0; long long i = 0, j = 0, k = 0;
    std::string root = "regions";
    if (argc > 1) face = std::atoi(argv[1]);
    if (argc > 2) i = std::atoll(argv[2]);
    if (argc > 3) j = std::atoll(argv[3]);
    if (argc > 4) k = std::atoll(argv[4]);
    if (argc > 5) root = argv[5];

    FaceChunkKey key{face, i, j, k};

    // Build a simple chunk
    Chunk64 c{};
    for (int z = 0; z < Chunk64::N; ++z) {
        for (int y = 0; y < Chunk64::N; ++y) {
            for (int x = 0; x < Chunk64::N; ++x) {
                uint16_t mat = (y < 32) ? MAT_ROCK : MAT_AIR;
                if ((x-20)*(x-20) + (z-20)*(z-20) < 16 && y < 40) mat = MAT_DIRT; // little column
                c.set_voxel(x, y, z, mat);
            }
        }
    }

    // Save
    if (!RegionIO::save_chunk(key, c, 32, root)) {
        std::fprintf(stderr, "Save failed\n");
        return 1;
    }

    // Load back
    Chunk64 d{};
    if (!RegionIO::load_chunk(key, d, 32, root)) {
        std::fprintf(stderr, "Load failed\n");
        return 1;
    }

    // Quick sanity: compare a few voxels
    size_t mism = 0;
    for (int z = 0; z < Chunk64::N; ++z) {
        for (int y = 0; y < Chunk64::N; ++y) {
            for (int x = 0; x < Chunk64::N; ++x) {
                if (c.get_material(x,y,z) != d.get_material(x,y,z)) { ++mism; }
            }
        }
    }

    Mesh m; mesh_chunk_greedy(d, m, 0.10f);
    std::printf("Round-trip mismatches: %zu, Mesh tris: %zu, verts: %zu\n", mism, m.indices.size()/3, m.vertices.size());
    return 0;
}

