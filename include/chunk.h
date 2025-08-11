#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include "bitarray.h"
#include "planet.h"

namespace wf {

struct Chunk64 {
    static constexpr int N = 64;
    static constexpr int N3 = N * N * N;

    std::vector<uint16_t> palette;                            // palette[index] -> material id
    std::unordered_map<uint16_t, uint16_t> palette_lut;       // material id -> palette index
    BitArray indices;                                         // paletted indices (start 8bpp)
    std::array<uint64_t, (N3 + 63) / 64> occ{};               // occupancy bitset (non-air)

    bool dirty_mesh = true;

    Chunk64() : indices(N3, 8) {}

    static inline uint32_t lindex(int x, int y, int z) {
        return uint32_t((z * N + y) * N + x);
    }

    uint16_t ensure_palette(uint16_t mat) {
        auto it = palette_lut.find(mat);
        if (it != palette_lut.end()) return it->second;
        uint16_t id = (uint16_t)palette.size();
        palette.push_back(mat);
        palette_lut.emplace(mat, id);
        return id;
    }

    void set_voxel(int x, int y, int z, uint16_t mat) {
        const uint32_t i = lindex(x, y, z);
        const uint32_t pi = ensure_palette(mat);
        indices.set(i, pi);
        const uint32_t w = i >> 6; const uint64_t bit = 1ull << (i & 63);
        if (mat != MAT_AIR) occ[w] |= bit; else occ[w] &= ~bit;
        dirty_mesh = true;
    }

    uint16_t get_material(int x, int y, int z) const {
        const uint32_t i = lindex(x, y, z);
        const uint32_t pi = indices.get(i);
        return palette.empty() ? MAT_AIR : palette[std::min<uint32_t>(pi, (uint32_t)palette.size() - 1)];
    }

    bool is_solid(int x, int y, int z) const {
        const uint32_t i = lindex(x, y, z);
        const uint32_t w = i >> 6; const uint64_t bit = 1ull << (i & 63);
        return (occ[w] & bit) != 0ull;
    }

    bool is_all_air() const {
        const size_t words = occ.size();
        if (words == 0) return true;
        // All words must be zero (ignoring padded bits in the last word)
        for (size_t i = 0; i + 1 < words; ++i) if (occ[i] != 0ull) return false;
        // Last word: mask valid bits only
        const uint32_t valid = (uint32_t)(N3 & 63);
        uint64_t mask = (valid == 0) ? ~0ull : ((1ull << valid) - 1ull);
        return (occ[words - 1] & mask) == 0ull;
    }

    bool is_all_solid() const {
        const size_t words = occ.size();
        if (words == 0) return false;
        for (size_t i = 0; i + 1 < words; ++i) if (occ[i] != ~0ull) return false;
        const uint32_t valid = (uint32_t)(N3 & 63);
        uint64_t mask = (valid == 0) ? ~0ull : ((1ull << valid) - 1ull);
        return (occ[words - 1] & mask) == mask;
    }
};

} // namespace wf
