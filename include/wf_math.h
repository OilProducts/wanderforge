#pragma once

#include <cstdint>
#include <cmath>

namespace wf {

using i64 = std::int64_t;

struct Int3 {
    i64 x, y, z;
    Int3() : x(0), y(0), z(0) {}
    Int3(i64 X, i64 Y, i64 Z) : x(X), y(Y), z(Z) {}
};

struct Float3 {
    float x, y, z;
    Float3() : x(0), y(0), z(0) {}
    Float3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
};

inline Float3 operator+(Float3 a, Float3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Float3 operator-(Float3 a, Float3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Float3 operator*(Float3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline Float3 operator/(Float3 a, float s) { return {a.x/s, a.y/s, a.z/s}; }
inline float dot(Float3 a, Float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float length(Float3 a) { return std::sqrt(dot(a,a)); }
inline Float3 normalize(Float3 a) { float l = length(a); return (l>0)? a/l : Float3{0,0,0}; }

inline Float3 to_float3(Int3 a, float s=1.0f) { return { float(a.x)*s, float(a.y)*s, float(a.z)*s }; }

} // namespace wf

