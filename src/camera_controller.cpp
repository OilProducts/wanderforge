#include "camera_controller.h"

#include <algorithm>
#include <cmath>

#include "wf_math.h"

namespace wf {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = 1.57079632679489661923f;

inline Float3 cross3(const Float3& a, const Float3& b) {
    return Float3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float dot3(const Float3& a, const Float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Float3 normalize_or(const Float3& v, const Float3& fallback) {
    float len = wf::length(v);
    return (len > 1e-5f) ? (v / len) : fallback;
}

inline Float3 to_float3(const double pos[3]) {
    return Float3{static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
}

inline float deg_to_rad(float deg) {
    return deg * 0.01745329252f;
}

} // namespace

CameraController::CameraController() = default;

void CameraController::set_planet_config(const PlanetConfig& cfg) {
    planet_cfg_ = cfg;
}

void CameraController::set_aspect_ratio(float aspect) {
    if (aspect > 0.0f) {
        aspect_ratio_ = aspect;
    }
}

void CameraController::apply_settings(const CameraControllerSettings& settings) {
    invert_mouse_x_ = settings.invert_mouse_x;
    invert_mouse_y_ = settings.invert_mouse_y;
    cam_sensitivity_ = settings.cam_sensitivity;
    cam_speed_ = settings.cam_speed;
    walk_mode_ = settings.walk_mode;
    walk_speed_ = settings.walk_speed;
    walk_pitch_max_deg_ = settings.walk_pitch_max_deg;
    walk_surface_bias_m_ = settings.walk_surface_bias_m;
    eye_height_m_ = settings.eye_height_m;
    surface_push_m_ = settings.surface_push_m;
}

CameraControllerSettings CameraController::settings() const {
    CameraControllerSettings cfg;
    cfg.invert_mouse_x = invert_mouse_x_;
    cfg.invert_mouse_y = invert_mouse_y_;
    cfg.cam_sensitivity = cam_sensitivity_;
    cfg.cam_speed = cam_speed_;
    cfg.walk_mode = walk_mode_;
    cfg.walk_speed = walk_speed_;
    cfg.walk_pitch_max_deg = walk_pitch_max_deg_;
    cfg.walk_surface_bias_m = walk_surface_bias_m_;
    cfg.eye_height_m = eye_height_m_;
    cfg.surface_push_m = surface_push_m_;
    return cfg;
}

void CameraController::sync_state(const Float3& position, float yaw_rad, float pitch_rad, bool walk_mode) {
    cam_pos_[0] = position.x;
    cam_pos_[1] = position.y;
    cam_pos_[2] = position.z;
    cam_yaw_ = yaw_rad;
    cam_pitch_ = pitch_rad;
    walk_mode_ = walk_mode;
}

CameraUpdateResult CameraController::update(const CameraUpdateInput& input) {
    CameraUpdateResult result{};

    double dt = std::max(0.0, input.dt);
    if (input.requested_walk_mode != walk_mode_) {
        walk_mode_ = input.requested_walk_mode;
        result.walk_mode_changed = true;
    }

    ground_follow_enabled_ = input.ground_follow;

    float prev_yaw = cam_yaw_;
    float prev_pitch = cam_pitch_;
    double prev_pos[3] = {cam_pos_[0], cam_pos_[1], cam_pos_[2]};

    float yaw_delta = input.look.yaw_delta * cam_sensitivity_ * (invert_mouse_x_ ? -1.0f : 1.0f);
    float pitch_delta = input.look.pitch_delta * cam_sensitivity_ * (invert_mouse_y_ ? 1.0f : -1.0f);

    if (!walk_mode_) {
        cam_yaw_ += yaw_delta;
        if (cam_yaw_ > kPi) cam_yaw_ -= 2.0f * kPi;
        if (cam_yaw_ < -kPi) cam_yaw_ += 2.0f * kPi;
        cam_pitch_ += pitch_delta;
        if (input.clamp_pitch) {
            float max_pitch = kHalfPi - 0.01745329252f;
            cam_pitch_ = std::clamp(cam_pitch_, -max_pitch, max_pitch);
        }
    } else {
        Float3 pos = to_float3(cam_pos_);
        Float3 updir = wf::normalize(pos);

        auto rotate_axis = [](const Float3& v, const Float3& axis, float angle) {
            Float3 n = normalize_or(axis, Float3{0.0f, 1.0f, 0.0f});
            float c = std::cos(angle);
            float s = std::sin(angle);
            float dot = dot3(n, v);
            Float3 cross_nv = cross3(n, v);
            return Float3{
                v.x * c + cross_nv.x * s + n.x * dot * (1.0f - c),
                v.y * c + cross_nv.y * s + n.y * dot * (1.0f - c),
                v.z * c + cross_nv.z * s + n.z * dot * (1.0f - c)
            };
        };

        float cyaw = std::cos(cam_yaw_);
        float syaw = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_);
        float sp = std::sin(cam_pitch_);
        Float3 forward = normalize_or(Float3{cp * cyaw, sp, cp * syaw}, Float3{1.0f, 0.0f, 0.0f});

        if (yaw_delta != 0.0f) {
            forward = rotate_axis(forward, updir, yaw_delta);
            forward = wf::normalize(forward);
        }
        if (pitch_delta != 0.0f) {
            Float3 right_axis = normalize_or(cross3(updir, forward), Float3{0.0f, 1.0f, 0.0f});
            Float3 candidate = rotate_axis(forward, right_axis, pitch_delta);
            candidate = wf::normalize(candidate);
            if (input.clamp_pitch) {
                float sin_pitch = std::clamp(dot3(candidate, updir), -1.0f, 1.0f);
                float max_pitch = deg_to_rad(walk_pitch_max_deg_);
                float max_s = std::sin(max_pitch);
                if (sin_pitch > max_s || sin_pitch < -max_s) {
                    float clamped = std::clamp(sin_pitch, -max_s, max_s);
                    Float3 tangent = normalize_or(candidate - updir * sin_pitch, forward);
                    float tangent_scale = std::sqrt(std::max(0.0f, 1.0f - clamped * clamped));
                    candidate = wf::normalize(tangent * tangent_scale + updir * clamped);
                }
            }
            forward = candidate;
        }

        cam_yaw_ = std::atan2(forward.z, forward.x);
        cam_pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    }

    Float3 pos_f = to_float3(cam_pos_);
    float cy = std::cos(cam_yaw_);
    float sy = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_);
    float sp = std::sin(cam_pitch_);
    Float3 forward = normalize_or(Float3{cp * cy, sp, cp * sy}, Float3{1.0f, 0.0f, 0.0f});
    Float3 world_up = walk_mode_ ? wf::normalize(pos_f) : Float3{0.0f, 1.0f, 0.0f};
    Float3 right = normalize_or(cross3(forward, world_up), Float3{0.0f, 0.0f, 1.0f});

    if (!walk_mode_) {
        Float3 delta = Float3{0.0f, 0.0f, 0.0f};
        delta = delta + forward * input.move.forward;
        delta = delta + right * input.move.strafe;
        delta = delta + Float3{0.0f, 1.0f, 0.0f} * input.move.vertical;
        if (wf::length(delta) > 0.0f) {
            delta = wf::normalize(delta) * (cam_speed_ * static_cast<float>(dt) * (input.sprint ? 3.0f : 1.0f));
            cam_pos_[0] += delta.x;
            cam_pos_[1] += delta.y;
            cam_pos_[2] += delta.z;
        }
    } else {
        Float3 updir_norm = wf::normalize(to_float3(cam_pos_));
        Float3 tangent_forward = normalize_or(forward - updir_norm * dot3(forward, updir_norm), right);
        Float3 tangent_right = normalize_or(cross3(tangent_forward, updir_norm), Float3{0.0f, 1.0f, 0.0f});
        Float3 step = Float3{0.0f, 0.0f, 0.0f};
        if (std::abs(input.move.forward) > 1e-5f) {
            step = step + tangent_forward * input.move.forward;
        }
        if (std::abs(input.move.strafe) > 1e-5f) {
            step = step + tangent_right * input.move.strafe;
        }
        float step_len = wf::length(step);
        if (step_len > 0.0f) {
            step = wf::normalize(step);
            double cam_radius = std::sqrt(cam_pos_[0] * cam_pos_[0] + cam_pos_[1] * cam_pos_[1] + cam_pos_[2] * cam_pos_[2]);
            float angle = static_cast<float>((walk_speed_ * (input.sprint ? 2.0f : 1.0f) * dt) / std::max(cam_radius, 1e-6));
            Float3 rotated = updir_norm * std::cos(angle) + step * std::sin(angle);
            rotated = wf::normalize(rotated);
            cam_pos_[0] = rotated.x * cam_radius;
            cam_pos_[1] = rotated.y * cam_radius;
            cam_pos_[2] = rotated.z * cam_radius;
        }

        if (ground_follow_enabled_) {
            ground_follow();
        }
    }

    bool moved = (std::fabs(cam_pos_[0] - prev_pos[0]) > kEpsilon ||
                  std::fabs(cam_pos_[1] - prev_pos[1]) > kEpsilon ||
                  std::fabs(cam_pos_[2] - prev_pos[2]) > kEpsilon);
    bool rotated = (std::fabs(cam_yaw_ - prev_yaw) > 1e-5f || std::fabs(cam_pitch_ - prev_pitch) > 1e-5f);

    result.moved = moved;
    result.rotated = rotated;

    return result;
}

void CameraController::toggle_walk_mode() {
    walk_mode_ = !walk_mode_;
    if (walk_mode_) {
        ground_follow();
        clamp_pitch(deg_to_rad(walk_pitch_max_deg_));
    }
}

void CameraController::toggle_invert_x() {
    invert_mouse_x_ = !invert_mouse_x_;
}

void CameraController::toggle_invert_y() {
    invert_mouse_y_ = !invert_mouse_y_;
}

Float3 CameraController::position() const {
    return to_float3(cam_pos_);
}

Float3 CameraController::forward() const {
    float cy = std::cos(cam_yaw_);
    float sy = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_);
    float sp = std::sin(cam_pitch_);
    return wf::normalize(Float3{cp * cy, sp, cp * sy});
}

Float3 CameraController::up() const {
    if (walk_mode_) {
        return wf::normalize(position());
    }
    return Float3{0.0f, 1.0f, 0.0f};
}

CameraSnapshot CameraController::snapshot(float fov_deg, float near_plane, float far_plane) const {
    CameraSnapshot snap{};
    Float3 pos = position();
    Float3 fwd = forward();
    Float3 updir = walk_mode_ ? wf::normalize(pos) : Float3{0.0f, 1.0f, 0.0f};
    Float3 center = pos + fwd;
    snap.view = wf::look_at_rh({pos.x, pos.y, pos.z}, {center.x, center.y, center.z}, {updir.x, updir.y, updir.z});
    float fovy = deg_to_rad(fov_deg);
    snap.projection = wf::perspective_vk(fovy, aspect_ratio_, near_plane, far_plane);
    snap.position = pos;
    snap.forward = fwd;
    snap.up = updir;
    snap.fov_deg = fov_deg;
    snap.near_plane = near_plane;
    snap.far_plane = far_plane;
    return snap;
}

void CameraController::clamp_pitch(float max_pitch_rad) {
    cam_pitch_ = std::clamp(cam_pitch_, -max_pitch_rad, max_pitch_rad);
}

void CameraController::ground_follow() {
    Float3 ndir = wf::normalize(position());
    double h = terrain_height_m(planet_cfg_, ndir);
    double surface_r = planet_cfg_.radius_m + h;
    if (surface_r < planet_cfg_.sea_level_m) {
        surface_r = planet_cfg_.sea_level_m;
    }
    double s_m = planet_cfg_.voxel_size_m;
    double mesh_r = std::floor(surface_r / s_m) * s_m + 0.5 * s_m;
    double target_r = mesh_r + static_cast<double>(eye_height_m_ + walk_surface_bias_m_);
    cam_pos_[0] = ndir.x * target_r;
    cam_pos_[1] = ndir.y * target_r;
    cam_pos_[2] = ndir.z * target_r;
}

} // namespace wf
