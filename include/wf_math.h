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


// Row-major 4x4 helpers (current Phase 3 conventions)
namespace wf {
inline void mat4_mul_row_major(const float A[16], const float B[16], float out[16]) {
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
        out[r*4+c] = A[r*4+0]*B[0*4+c] + A[r*4+1]*B[1*4+c] + A[r*4+2]*B[2*4+c] + A[r*4+3]*B[3*4+c];
    }
}
inline void mat4_perspective_gl_row_major(float fov_rad, float aspect, float zn, float zf, float out[16]) {
    float f = 1.0f / std::tan(fov_rad * 0.5f);
    out[0]=f/aspect; out[1]=0; out[2]=0; out[3]=0;
    out[4]=0; out[5]=f; out[6]=0; out[7]=0;
    out[8]=0; out[9]=0; out[10]=(zf+zn)/(zn-zf); out[11]=-1;
    out[12]=0; out[13]=0; out[14]=(2*zf*zn)/(zn-zf); out[15]=0;
}
}
