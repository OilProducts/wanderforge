#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include "planet.h"
#include "wf_math.h"

using namespace wf;

static double find_surface_radius(const PlanetConfig& cfg, const Float3& dir) {
    // Binary search for air->solid boundary along radial direction
    double low = cfg.radius_m - 200.0; // inside
    double high = cfg.radius_m + 200.0; // outside
    auto is_solid_at = [&](double r)->bool{
        Float3 p = {dir.x * (float)r, dir.y * (float)r, dir.z * (float)r};
        Int3 v{ (i64)std::llround(p.x / cfg.voxel_size_m), (i64)std::llround(p.y / cfg.voxel_size_m), (i64)std::llround(p.z / cfg.voxel_size_m) };
        auto s = sample_base(cfg, v);
        return s.material != MAT_AIR;
    };
    // Ensure bracket: low should be solid, high should be air
    if (!is_solid_at(low)) low = cfg.radius_m - 400.0;
    if (is_solid_at(high)) high = cfg.radius_m + 400.0;
    for (int i=0; i<24; ++i) {
        double mid = 0.5 * (low + high);
        if (is_solid_at(mid)) low = mid; else high = mid;
    }
    return low; // last solid
}

static void write_ppm(const std::string& path, int W, int H, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror("fopen"); std::exit(1); }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    int W = 1024, H = 256;
    double lat_deg = 0.0; // ring latitude
    std::string out = "ring.ppm";
    if (argc > 1) W = std::atoi(argv[1]);
    if (argc > 2) H = std::atoi(argv[2]);
    if (argc > 3) lat_deg = std::atof(argv[3]);
    if (argc > 4) out = argv[4];

    PlanetConfig cfg;
    std::vector<uint8_t> img(W * H * 3, 0);

    const double span_m = 40.0; // vertical span +-20m around surface
    const double lat_rad = lat_deg * M_PI / 180.0;
    for (int x = 0; x < W; ++x) {
        double theta = (double(x) / double(W)) * 2.0 * M_PI;
        // Direction at constant latitude
        Float3 dir = { (float)(std::cos(lat_rad) * std::cos(theta)), (float)std::sin(lat_rad), (float)(std::cos(lat_rad) * std::sin(theta)) };
        dir = normalize(dir);
        double r_surface = find_surface_radius(cfg, dir);
        for (int y = 0; y < H; ++y) {
            double t = (double(y) / double(H - 1));
            double offset = (t * 2.0 - 1.0) * (span_m * 0.5);
            double r = r_surface + offset;
            Float3 p = {dir.x * (float)r, dir.y * (float)r, dir.z * (float)r};
            Int3 v{ (i64)std::llround(p.x / cfg.voxel_size_m), (i64)std::llround(p.y / cfg.voxel_size_m), (i64)std::llround(p.z / cfg.voxel_size_m) };
            auto s = sample_base(cfg, v);
            uint8_t r8=0,g8=0,b8=0;
            switch (s.material) {
                case MAT_AIR:   r8=0;   g8=0;   b8=0;   break;
                case MAT_WATER: r8=30;  g8=80;  b8=200; break;
                case MAT_DIRT:  r8=120; g8=72;  b8=24;  break;
                case MAT_ROCK:  r8=160; g8=160; b8=160; break;
                case MAT_LAVA:  r8=230; g8=80;  b8=20;  break;
                default:        r8=255; g8=0;   b8=255; break;
            }
            size_t idx = (size_t(y) * W + x) * 3;
            img[idx+0]=r8; img[idx+1]=g8; img[idx+2]=b8;
        }
    }

    write_ppm(out, W, H, img);
    std::printf("Wrote %s (%dx%d)\n", out.c_str(), W, H);
    return 0;
}
