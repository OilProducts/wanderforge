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

bool face_uv_from_direction(Float3 dir, int& face, float& u, float& v) {
    face = face_from_direction(dir);
    Float3 right = FACE_RIGHT[face];
    Float3 up    = FACE_UP[face];
    Float3 fwd   = FACE_FORWARD[face];
    float cF = dot(dir, fwd);
    if (std::fabs(cF) < 1e-8f) { u = v = 0.0f; return false; }
    float cR = dot(dir, right);
    float cU = dot(dir, up);
    u = cR / cF;
    v = cU / cF;
    return true;
}

Float3 direction_from_lat_lon(double lat_rad, double lon_rad) {
    float cl = std::cos(lat_rad), sl = std::sin(lat_rad);
    float co = std::cos(lon_rad), so = std::sin(lon_rad);
    return normalize(Float3{ cl * co, sl, cl * so });
}

void face_basis(int face, Float3& right, Float3& up, Float3& forward) {
    right = FACE_RIGHT[face];
    up    = FACE_UP[face];
    forward = FACE_FORWARD[face];
}

Int3 voxel_from_lat_lon_h(const PlanetConfig& cfg, double lat_rad, double lon_rad, double height_m) {
    Float3 dir = direction_from_lat_lon(lat_rad, lon_rad);
    double r = cfg.radius_m + height_m;
    Float3 p = { dir.x * (float)r, dir.y * (float)r, dir.z * (float)r };
    return Int3{ (i64)std::llround(p.x / cfg.voxel_size_m), (i64)std::llround(p.y / cfg.voxel_size_m), (i64)std::llround(p.z / cfg.voxel_size_m) };
}

void lat_lon_h_from_voxel(const PlanetConfig& cfg, Int3 voxel, double& lat_rad, double& lon_rad, double& height_m) {
    Float3 p = to_float3(voxel, float(cfg.voxel_size_m));
    float r = length(p);
    if (r <= 0.0f) { lat_rad = 0.0; lon_rad = 0.0; height_m = -cfg.radius_m; return; }
    Float3 d = p / r;
    lat_rad = std::asin(d.y);
    lon_rad = std::atan2(d.z, d.x);
    height_m = double(r) - cfg.radius_m;
}

FaceChunkKey face_chunk_from_voxel(const PlanetConfig& cfg, Int3 voxel, int chunk_vox) {
    Float3 p = to_float3(voxel, float(cfg.voxel_size_m));
    float r = length(p);
    Float3 d = (r > 0.0f) ? p / r : Float3{1,0,0};
    int face = face_from_direction(d);
    Float3 right = FACE_RIGHT[face], up = FACE_UP[face];
    double s = double(dot(p, right));
    double t = double(dot(p, up));
    double chunk_m = double(chunk_vox) * cfg.voxel_size_m;
    std::int64_t i = (std::int64_t)std::floor(s / chunk_m);
    std::int64_t j = (std::int64_t)std::floor(t / chunk_m);
    std::int64_t k = (std::int64_t)std::floor(double(r) / chunk_m);
    return FaceChunkKey{face, i, j, k};
}

BaseSample sample_base(const PlanetConfig& cfg, Int3 voxel) {
    // Convert voxel (integer grid at 10cm) to meters and spherical radius
    Float3 pos_m = to_float3(voxel, float(cfg.voxel_size_m));
    float r = length(pos_m);

    // Elevation via FBM on the angular direction; cave noise via 3D fbm
    Float3 dir = (r > 0) ? pos_m / r : Float3{0,1,0};
    double height_m = terrain_height_m(cfg, dir);

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

double terrain_height_m(const PlanetConfig& cfg, Float3 direction) {
    // Map to face-UV and compute the same FBM elevation as sample_base
    int face = 0; float u = 0.0f, v = 0.0f;
    (void)face_uv_from_direction(direction, face, u, v);
    float elev = fbm({u*128.0f, v*128.0f, 0.0f}, 5, 2.0f, 0.5f, cfg.seed); // [-1,1]
    elev = (elev + 1.0f) * 0.5f; // [0,1]
    double height_m = 40.0 * elev; // up to ~40 m hills
    return height_m;
}

} // namespace wf
