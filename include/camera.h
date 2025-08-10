// Minimal camera math helpers (parity-preserving row-major path)
#pragma once

#include <array>

namespace wf {

// Row-major 4x4 matrix type
using Mat4 = std::array<float, 16>;

// Build a row-major perspective matrix (OpenGL-style) matching current app behavior.
Mat4 perspective_row_major(float fov_deg, float aspect, float zn, float zf);

// Build a row-major view matrix from yaw/pitch/eye (right-handed, +Y up), matching current app behavior.
Mat4 view_row_major(float yaw, float pitch, const float eye[3]);

// Row-major matrix multiply: R = A * B
Mat4 mul_row_major(const Mat4& A, const Mat4& B);

} // namespace wf

