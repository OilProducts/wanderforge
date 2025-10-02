#pragma once

#include <cstdint>
#include <cmath>

namespace wf {
    using i64 = std::int64_t;

    struct Int3 {
        i64 x, y, z;

        Int3() : x(0), y(0), z(0) {
        }

        Int3(i64 X, i64 Y, i64 Z) : x(X), y(Y), z(Z) {
        }
    };

    struct Float3 {
        float x, y, z;

        Float3() : x(0), y(0), z(0) {
        }

        Float3(float X, float Y, float Z) : x(X), y(Y), z(Z) {
        }
    };

    inline Float3 operator+(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline Float3 operator-(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline Float3 operator*(Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    inline Float3 operator/(Float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
    inline float dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float length(Float3 a) { return std::sqrt(dot(a, a)); }

    inline Float3 normalize(Float3 a) {
        float l = length(a);
        return (l > 0) ? a / l : Float3{0, 0, 0};
    }

    inline Float3 to_float3(Int3 a, float s = 1.0f) { return {float(a.x) * s, float(a.y) * s, float(a.z) * s}; }
} // namespace wf


// Column-major 4x4 helpers (Vulkan conventions)
namespace wf {
    struct Mat4 {
        float m[16];
        // Matrices are stored column-major to match GLSL/Vulkan expectations.
        float &at(int r, int c) { return m[c * 4 + r]; }
        float at(int r, int c) const { return m[c * 4 + r]; }

        float* data() { return m; }
        const float* data() const { return m; }

        static Mat4 identity() {
            Mat4 I{};
            I.at(0, 0) = I.at(1, 1) = I.at(2, 2) = I.at(3, 3) = 1.0f;
            return I;
        }
    };

    inline Mat4 mul(const Mat4 &A, const Mat4 &B) {
        // A * B
        Mat4 R{};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                R.at(r, c) =
                        A.at(r, 0) * B.at(0, c) +
                        A.at(r, 1) * B.at(1, c) +
                        A.at(r, 2) * B.at(2, c) +
                        A.at(r, 3) * B.at(3, c);
        return R;
    }

    struct Vec3 {
        float x, y, z;
    };

    struct Vec4 {
        float x, y, z, w;
    };

    inline Vec4 mul(const Mat4 &M, const Vec4 &v) {
        // M * v
        return {
            M.at(0, 0) * v.x + M.at(0, 1) * v.y + M.at(0, 2) * v.z + M.at(0, 3) * v.w,
            M.at(1, 0) * v.x + M.at(1, 1) * v.y + M.at(1, 2) * v.z + M.at(1, 3) * v.w,
            M.at(2, 0) * v.x + M.at(2, 1) * v.y + M.at(2, 2) * v.z + M.at(2, 3) * v.w,
            M.at(3, 0) * v.x + M.at(3, 1) * v.y + M.at(3, 2) * v.z + M.at(3, 3) * v.w
        };
    };

    inline Mat4 perspective_vk(float fovy_rad, float aspect, float zn, float zf) {
        const float f = 1.0f / std::tan(fovy_rad * 0.5f);
        Mat4 P{}; // column-major
        P.at(0, 0) = f / aspect;
        P.at(1, 1) = f;
        P.at(2, 2) = zf / (zn - zf); // note the 0..1 mapping
        P.at(2, 3) = (zf * zn) / (zn - zf);
        P.at(3, 2) = -1.0f;
        return P;
    };

    // simple vec helpers
    inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

    inline Vec3 cross(Vec3 a, Vec3 b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    inline Vec3 norm(Vec3 v) {
        float l = std::sqrt(dot(v, v));
        return {v.x / l, v.y / l, v.z / l};
    }

    inline Mat4 look_at_rh(Vec3 eye, Vec3 center, Vec3 up) {
        Vec3 f = norm(sub(center, eye)); // forward
        Vec3 s = norm(cross(f, up)); // right
        Vec3 u = cross(s, f); // up (already normalized)

        Mat4 M{};
        M.at(0, 0) = s.x;   M.at(0, 1) = s.y;   M.at(0, 2) = s.z;   M.at(0, 3) = -dot(s, eye);
        M.at(1, 0) = u.x;   M.at(1, 1) = u.y;   M.at(1, 2) = u.z;   M.at(1, 3) = -dot(u, eye);
        M.at(2, 0) = -f.x;  M.at(2, 1) = -f.y;  M.at(2, 2) = -f.z;  M.at(2, 3) = dot(f, eye);
        M.at(3, 0) = 0.0f;  M.at(3, 1) = 0.0f;  M.at(3, 2) = 0.0f;  M.at(3, 3) = 1.0f;
        return M;
    }


    inline void mat4_mul_row_major(const float A[16], const float B[16], float out[16]) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                out[r * 4 + c] = A[r * 4 + 0] * B[0 * 4 + c] + A[r * 4 + 1] * B[1 * 4 + c] + A[r * 4 + 2] * B[2 * 4 + c]
                                 + A[r * 4 + 3] * B[3 * 4 + c];
            }
    }

    inline void mat4_perspective_gl_row_major(float fov_rad, float aspect, float zn, float zf, float out[16]) {
        float f = 1.0f / std::tan(fov_rad * 0.5f);
        out[0] = f / aspect;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        out[4] = 0;
        out[5] = f;
        out[6] = 0;
        out[7] = 0;
        out[8] = 0;
        out[9] = 0;
        out[10] = (zf + zn) / (zn - zf);
        out[11] = -1;
        out[12] = 0;
        out[13] = 0;
        out[14] = (2 * zf * zn) / (zn - zf);
        out[15] = 0;
    }
}
