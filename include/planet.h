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
    // Terrain elevation FBM parameters (app-load configurable)
    double terrain_amp_m = 12.0;  // peak-to-peak amplitude in meters (approx)
    float  terrain_freq = 64.0f;  // base frequency scaling for face-UV
    int    terrain_octaves = 4;   // number of FBM octaves
    float  terrain_lacunarity = 2.0f; // octave frequency multiplier
    float  terrain_gain = 0.5f;        // amplitude falloff per octave
};

// Map a 3D direction to cube face index (0..5) – utility for future chunking
int face_from_direction(Float3 dir);

// Given a face index and local [-1,1] uv on that face, return unit direction on the sphere
Float3 direction_from_face_uv(int face, float u, float v);

// Inverse mapping: given a direction, return face and face-UV in [-1,1].
// Returns true on success (non-zero forward component).
bool face_uv_from_direction(Float3 dir, int& face, float& u, float& v);

// Spherical helpers (latitude φ in radians [-pi/2, pi/2], longitude λ in radians [-pi, pi])
Float3 direction_from_lat_lon(double lat_rad, double lon_rad);

// Basis vectors for a cube face: right, up, forward (unit length)
void face_basis(int face, Float3& right, Float3& up, Float3& forward);

// World <-> Voxel conversions
Int3 voxel_from_lat_lon_h(const PlanetConfig& cfg, double lat_rad, double lon_rad, double height_m);
void lat_lon_h_from_voxel(const PlanetConfig& cfg, Int3 voxel, double& lat_rad, double& lon_rad, double& height_m);

// Face-local chunk grid mapping for early streaming prototypes
struct FaceChunkKey { int face; std::int64_t i; std::int64_t j; std::int64_t k; };
FaceChunkKey face_chunk_from_voxel(const PlanetConfig& cfg, Int3 voxel, int chunk_vox = 64);

// Sample the base world (procedural, read-only).
BaseSample sample_base(const PlanetConfig& cfg, Int3 voxel);

// Deterministic terrain height at a direction on the sphere, in meters above cfg.radius_m.
// Mirrors the elevation logic used in sample_base (no caves/water/biomes); used for ground following.
double terrain_height_m(const PlanetConfig& cfg, Float3 direction);

} // namespace wf
