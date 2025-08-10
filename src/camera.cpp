#include "camera.h"

#include <cmath>

namespace wf {

Mat4 perspective_row_major(float fov_deg, float aspect, float zn, float zf) {
    const float fov = fov_deg * 0.01745329252f; // deg -> rad
    const float f = 1.0f / std::tan(fov * 0.5f);
    // Matches existing app's row-major P matrix
    Mat4 P = {
        f/aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (zf+zn)/(zn-zf), -1,
        0, 0, (2*zf*zn)/(zn-zf), 0
    };
    return P;
}

Mat4 view_row_major(float yaw, float pitch, const float eye[3]) {
    auto norm3 = [](float v[3]) {
        float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l > 0) { v[0]/=l; v[1]/=l; v[2]/=l; }
    };
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    float fwd[3] = { cp*cy, sp, cp*sy };
    float upv[3] = { 0.0f, 1.0f, 0.0f };
    // right = fwd x up
    float s[3] = { fwd[1]*upv[2] - fwd[2]*upv[1], fwd[2]*upv[0] - fwd[0]*upv[2], fwd[0]*upv[1] - fwd[1]*upv[0] };
    norm3(s);
    // u = s x fwd
    float u[3] = { s[1]*fwd[2] - s[2]*fwd[1], s[2]*fwd[0] - s[0]*fwd[2], s[0]*fwd[1] - s[1]*fwd[0] };
    Mat4 V = {
        s[0], u[0], -fwd[0], 0,
        s[1], u[1], -fwd[1], 0,
        s[2], u[2], -fwd[2], 0,
        -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]),
        -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]),
        ( fwd[0]*eye[0] + fwd[1]*eye[1] + fwd[2]*eye[2]),
        1
    };
    return V;
}

Mat4 mul_row_major(const Mat4& A, const Mat4& B) {
    Mat4 R{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            R[r*4 + c] = A[r*4 + 0] * B[0*4 + c]
                       + A[r*4 + 1] * B[1*4 + c]
                       + A[r*4 + 2] * B[2*4 + c]
                       + A[r*4 + 3] * B[3*4 + c];
        }
    }
    return R;
}

} // namespace wf

