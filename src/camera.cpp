#include "camera.h"

#include <cmath>

namespace wf {

Mat4 perspective_from_deg(float fov_deg, float aspect, float zn, float zf) {
    return perspective_vk(fov_deg * 0.01745329252f, aspect, zn, zf);
}

Mat4 view_from_yaw_pitch(float yaw, float pitch, const float eye[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    Float3 fwd{cp * cy, sp, cp * sy};
    Vec3 eye_v{eye[0], eye[1], eye[2]};
    Vec3 center{eye_v.x + fwd.x, eye_v.y + fwd.y, eye_v.z + fwd.z};
    Vec3 up{0.0f, 1.0f, 0.0f};
    return look_at_rh(eye_v, center, up);
}

} // namespace wf
