#pragma once

#include <cstdint>
#include <cmath>
#include "wf_math.h"

namespace wf {

// Simple 32-bit hash (xorshift mix)
inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 17; x *= 0xED5AD4BBu;
    x ^= x >> 11; x *= 0xAC4C1B51u;
    x ^= x >> 15; x *= 0x31848BABu;
    x ^= x >> 14;
    return x;
}

inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed;
    h ^= hash_u32(uint32_t(x) * 0x9E3779B1u);
    h ^= hash_u32(uint32_t(y) * 0x85EBCA77u);
    h ^= hash_u32(uint32_t(z) * 0xC2B2AE3Du);
    return hash_u32(h);
}

// Value noise in [-1,1]
inline float value_noise(Int3 p, uint32_t seed) {
    return (hash3(int(p.x), int(p.y), int(p.z), seed) / float(0xFFFFFFFFu)) * 2.0f - 1.0f;
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float t) { return t*t*(3.0f - 2.0f*t); }

// Trilinear interpolated value noise at floating point position
inline float value_noise_trilinear(Float3 p, uint32_t seed) {
    int xi = int(std::floor(p.x)); float tx = p.x - xi;
    int yi = int(std::floor(p.y)); float ty = p.y - yi;
    int zi = int(std::floor(p.z)); float tz = p.z - zi;
    tx = smoothstep(tx); ty = smoothstep(ty); tz = smoothstep(tz);

    float c000 = value_noise({xi, yi, zi}, seed);
    float c100 = value_noise({xi+1, yi, zi}, seed);
    float c010 = value_noise({xi, yi+1, zi}, seed);
    float c110 = value_noise({xi+1, yi+1, zi}, seed);
    float c001 = value_noise({xi, yi, zi+1}, seed);
    float c101 = value_noise({xi+1, yi, zi+1}, seed);
    float c011 = value_noise({xi, yi+1, zi+1}, seed);
    float c111 = value_noise({xi+1, yi+1, zi+1}, seed);

    float x00 = lerp(c000, c100, tx);
    float x10 = lerp(c010, c110, tx);
    float x01 = lerp(c001, c101, tx);
    float x11 = lerp(c011, c111, tx);
    float y0 = lerp(x00, x10, ty);
    float y1 = lerp(x01, x11, ty);
    return lerp(y0, y1, tz);
}

// Fractional Brownian motion (FBM) with N octaves
inline float fbm(Float3 p, int octaves, float lacunarity, float gain, uint32_t seed) {
    float amp = 0.5f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i=0; i<octaves; ++i) {
        sum  += amp * value_noise_trilinear({p.x*freq, p.y*freq, p.z*freq}, seed + i*1013u);
        norm += amp;
        freq *= lacunarity;
        amp  *= gain;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

} // namespace wf

