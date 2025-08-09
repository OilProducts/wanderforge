#pragma once

#include <cstdint>
#include "wf_math.h"

namespace wf {

enum Material : uint16_t {
    MAT_AIR = 0,
    MAT_ROCK,
    MAT_DIRT,
    MAT_WATER,
    MAT_LAVA,
};

struct BaseSample { uint16_t material; float density; };

struct PlanetConfig {
    double radius_m = 1150.0;   // ~1.15 km (from 7.2 km circumference example)
    double voxel_size_m = 0.10; // 10 cm base voxels
    double sea_level_m = 1135.0;// slightly below radius to create water basins
    uint32_t seed = 1337u;
};

// Map a 3D direction to cube face index (0..5) – utility for future chunking
int face_from_direction(Float3 dir);

// Given a face index and local [-1,1] uv on that face, return unit direction on the sphere
Float3 direction_from_face_uv(int face, float u, float v);

// Sample the base world (procedural, read-only).
BaseSample sample_base(const PlanetConfig& cfg, Int3 voxel);

} // namespace wf

