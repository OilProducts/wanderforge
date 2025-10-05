#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "planet.h"

namespace wf {

struct AppConfig {
    bool invert_mouse_x = true;
    bool invert_mouse_y = false;
    float cam_sensitivity = 0.0025f;
    float cam_speed = 12.0f;
    float fov_deg = 60.0f;
    float near_m = 0.1f;
    float far_m = 300.0f;

    bool walk_mode = false;
    float eye_height_m = 1.7f;
    float walk_speed = 6.0f;
    float walk_pitch_max_deg = 60.0f;
    float walk_surface_bias_m = 1.0f;
    float surface_push_m = 0.0f;

    bool use_chunk_renderer = true;
    int ring_radius = 14;
    int prune_margin = 3;
    bool cull_enabled = true;
    bool draw_stats_enabled = true;

    float hud_scale = 2.0f;
    bool hud_shadow = false;
    float hud_shadow_offset_px = 1.5f;

    bool log_stream = false;
    bool log_pool = false;
    bool save_chunks_enabled = false;
    bool debug_chunk_keys = false;

    bool profile_csv_enabled = true;
    std::string profile_csv_path = "profile.csv";

    bool device_local_enabled = true;
    int pool_vtx_mb = 256;
    int pool_idx_mb = 128;

    int uploads_per_frame_limit = 16;
    int loader_threads = 0;
    int k_down = 3;
    int k_up = 3;
    int k_prune_margin = 1;
    float face_keep_time_cfg_s = 0.75f;

    PlanetConfig planet_cfg{};

    std::string config_path = "wanderforge.cfg";
    std::string region_root = "regions";
};

bool operator==(const AppConfig& a, const AppConfig& b);
bool operator!=(const AppConfig& a, const AppConfig& b);

class AppConfigManager {
public:
    explicit AppConfigManager(AppConfig defaults);

    void set_cli_config_path(std::string path);

    bool reload();
    bool reload_if_file_changed();
    bool save_active_to_file();
    void adopt_runtime_state(const AppConfig& cfg);

    const AppConfig& defaults() const { return defaults_; }
    const AppConfig& file_layer() const { return after_file_; }
    const AppConfig& env_layer() const { return after_env_; }
    const AppConfig& active() const { return active_; }
    const std::string& config_path() const { return resolved_config_path_; }
    bool has_config_file() const { return file_layer_loaded_; }

private:
    bool apply_file_layer(AppConfig& cfg);
    void apply_env_layer(AppConfig& cfg);
    bool rebuild_active(const AppConfig& base);
    bool update_file_timestamp();

    AppConfig defaults_{};
    AppConfig after_file_{};
    AppConfig after_env_{};
    AppConfig active_{};

    std::string cli_config_path_;
    std::string resolved_config_path_;
    bool file_layer_loaded_ = false;
    std::optional<std::filesystem::file_time_type> last_write_time_;
};

} // namespace wf
