// Minimal camera math helpers (column-major, Vulkan-friendly)
#pragma once

#include "wf_math.h"

namespace wf {

// Build a column-major perspective matrix from a FOV in degrees (Vulkan depth 0..1).
Mat4 perspective_from_deg(float fov_deg, float aspect, float zn, float zf);

// Build a column-major view matrix from yaw/pitch/eye (right-handed, +Y up).
Mat4 view_from_yaw_pitch(float yaw, float pitch, const float eye[3]);

} // namespace wf
