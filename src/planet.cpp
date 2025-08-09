#include "planet.h"
#include "wf_noise.h"

#include <algorithm>

namespace wf {

// Cube face axes (right, up, forward) for faces +X, -X, +Y, -Y, +Z, -Z
static const Float3 FACE_RIGHT[6] = {
    {0,0,-1}, {0,0,1},  {1,0,0}, {-1,0,0}, {1,0,0}, {-1,0,0}
};
static const Float3 FACE_UP[6] = {
    {0,1,0},  {0,1,0},  {0,0,1}, {0,0,-1}, {0,1,0}, {0,1,0}
};
static const Float3 FACE_FORWARD[6] = {
    {1,0,0},  {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
};

int face_from_direction(Float3 d) {
    Float3 a = { std::fabs(d.x), std::fabs(d.y), std::fabs(d.z) };
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0 ? 0 : 1;   // ±X
    if (a.y >= a.x && a.y >= a.z) return d.y >= 0 ? 2 : 3;   // ±Y
    return d.z >= 0 ? 4 : 5;                                 // ±Z
}

Float3 direction_from_face_uv(int face, float u, float v) {
    // Map face-local square [-1,1]^2 to cube then project to sphere
    Float3 right = FACE_RIGHT[face];
    Float3 up    = FACE_UP[face];
    Float3 fwd   = FACE_FORWARD[face];
    Float3 p = { fwd.x + right.x*u + up.x*v,
                 fwd.y + right.y*u + up.y*v,
                 fwd.z + right.z*u + up.z*v };
    return normalize(p);
}

BaseSample sample_base(const PlanetConfig& cfg, Int3 voxel) {
    // Convert voxel (integer grid at 10cm) to meters and spherical radius
    Float3 pos_m = to_float3(voxel, float(cfg.voxel_size_m));
    float r = length(pos_m);

    // Elevation via FBM on the angular direction; cave noise via 3D fbm
    Float3 dir = (r > 0) ? pos_m / r : Float3{0,1,0};
    // Scale angular coords to a pseudo 3D position by mapping direction to cube face uv
    int face = face_from_direction(dir);
    // Project dir back to face uv approximately using dot products
    Float3 right = FACE_RIGHT[face], up = FACE_UP[face], fwd = FACE_FORWARD[face];
    float u = dot(dir, right);
    float v = dot(dir, up);

    // Terrain FBM parameters
    float elev = fbm({u*128.0f, v*128.0f, 0.0f}, 5, 2.0f, 0.5f, cfg.seed); // [-1,1]
    elev = (elev + 1.0f) * 0.5f; // [0,1]
    double height_m = 40.0 * elev; // up to ~40 m hills

    // Base radial surface at radius + height
    double surface_r = cfg.radius_m + height_m;

    // Cave noise (3D)
    float cave = fbm({pos_m.x*0.05f, pos_m.y*0.05f, pos_m.z*0.05f}, 4, 2.2f, 0.5f, cfg.seed + 777u);
    bool cave_air = (cave > 0.35f); // sparse

    BaseSample out{};
    if (r > surface_r) {
        out.material = MAT_AIR; out.density = -1.0f; return out;
    }

    // Water layer around sea level where below surface but near sea level
    if (r > cfg.sea_level_m && (surface_r - r) < 5.0) {
        out.material = MAT_WATER; out.density = 1.0f; return out;
    }

    // Underground: rock with caves, top few meters dirt
    if (cave_air && r < surface_r - 3.0) {
        out.material = MAT_AIR; out.density = -0.5f; return out;
    }

    // Dirt near surface, rock deeper
    if (surface_r - r < 2.0) {
        out.material = MAT_DIRT; out.density = 1.0f; return out;
    } else {
        out.material = MAT_ROCK; out.density = 1.0f; return out;
    }
}

} // namespace wf

