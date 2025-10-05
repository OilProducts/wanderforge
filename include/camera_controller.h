#pragma once

#include <cstddef>

#include "wf_math.h"
#include "planet.h"

namespace wf {

struct MovementAxes {
    float forward = 0.0f;
    float strafe = 0.0f;
    float vertical = 0.0f;
};

struct LookInput {
    float yaw_delta = 0.0f;
    float pitch_delta = 0.0f;
};

struct CameraSnapshot {
    wf::Mat4 view{};
    wf::Mat4 projection{};
    wf::Float3 position{0.0f, 0.0f, 0.0f};
    wf::Float3 forward{1.0f, 0.0f, 0.0f};
    wf::Float3 up{0.0f, 1.0f, 0.0f};
    float fov_deg = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 300.0f;
};

struct CameraControllerSettings {
    bool invert_mouse_x = true;
    bool invert_mouse_y = true;
    float cam_sensitivity = 0.0025f;
    float cam_speed = 12.0f;
    bool walk_mode = false;
    float walk_speed = 6.0f;
    float walk_pitch_max_deg = 60.0f;
    float walk_surface_bias_m = 1.0f;
    float eye_height_m = 1.7f;
    float surface_push_m = 0.0f;
};

struct CameraUpdateInput {
    double dt = 0.0;
    MovementAxes move{};
    LookInput look{};
    bool requested_walk_mode = false;
    bool sprint = false;
    bool ground_follow = false;
    bool clamp_pitch = true;
};

struct CameraUpdateResult {
    bool moved = false;
    bool rotated = false;
    bool walk_mode_changed = false;
};

class CameraController {
public:
    CameraController();

    void set_planet_config(const PlanetConfig& cfg);
    void set_aspect_ratio(float aspect);

    void apply_settings(const CameraControllerSettings& settings);
    CameraControllerSettings settings() const;

    void sync_state(const wf::Float3& position, float yaw_rad, float pitch_rad, bool walk_mode);

    CameraUpdateResult update(const CameraUpdateInput& input);

    void toggle_walk_mode();
    void toggle_invert_x();
    void toggle_invert_y();

    wf::Float3 position() const;
    float yaw() const { return cam_yaw_; }
    float pitch() const { return cam_pitch_; }
    bool walk_mode() const { return walk_mode_; }
    bool invert_mouse_x() const { return invert_mouse_x_; }
    bool invert_mouse_y() const { return invert_mouse_y_; }

    wf::Float3 forward() const;
    wf::Float3 up() const;

    CameraSnapshot snapshot(float fov_deg, float near_plane, float far_plane) const;

private:
    static constexpr double kEpsilon = 1e-5;

    void clamp_pitch(float max_pitch_rad);
    void ground_follow();

    PlanetConfig planet_cfg_{};
    double cam_pos_[3] = {1165.0, 12.0, 0.0};
    float cam_yaw_ = 0.0f;
    float cam_pitch_ = 0.0f;
    bool walk_mode_ = false;

    bool invert_mouse_x_ = true;
    bool invert_mouse_y_ = true;
    float cam_sensitivity_ = 0.0025f;
    float cam_speed_ = 12.0f;
    float walk_speed_ = 6.0f;
    float walk_pitch_max_deg_ = 60.0f;
    float walk_surface_bias_m_ = 1.0f;
    float eye_height_m_ = 1.7f;
    float surface_push_m_ = 0.0f;
    float aspect_ratio_ = 16.0f / 9.0f;
    bool ground_follow_enabled_ = false;
};

} // namespace wf

