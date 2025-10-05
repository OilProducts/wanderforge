#include "config_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>

namespace wf {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_bool(std::string v, bool defv) {
    v = lower(trim(v));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return defv;
}

template <typename T, typename Fn>
void apply_env_value(const char* key, T& field, Fn&& parse_and_assign) {
    if (const char* s = std::getenv(key)) {
        try {
            parse_and_assign(s);
            std::cout << "[config] " << key << "=" << field << " (env)\n";
        } catch (...) {
            // Ignore malformed values, keep previous field state.
        }
    }
}

void apply_env_bool(const char* key, bool& field) {
    if (const char* s = std::getenv(key)) {
        field = parse_bool(s, field);
        std::cout << "[config] " << key << "=" << (field ? "true" : "false") << " (env)\n";
    }
}

bool apply_file_overrides(const std::string& path, AppConfig& cfg) {
    if (path.empty()) {
        return false;
    }

    std::ifstream in(path);
    if (!in.good()) {
        std::cout << "[config] config file not found: " << path << " (using defaults/env)\n";
        return false;
    }

    std::cout << "[config] reading " << path << "\n";

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        try {
            if (key == "invert_mouse_x") { cfg.invert_mouse_x = parse_bool(val, cfg.invert_mouse_x); std::cout << "[config] invert_mouse_x=" << (cfg.invert_mouse_x ? "true" : "false") << " (file)\n"; }
            else if (key == "invert_mouse_y") { cfg.invert_mouse_y = parse_bool(val, cfg.invert_mouse_y); std::cout << "[config] invert_mouse_y=" << (cfg.invert_mouse_y ? "true" : "false") << " (file)\n"; }
            else if (key == "mouse_sensitivity") { cfg.cam_sensitivity = std::stof(val); std::cout << "[config] mouse_sensitivity=" << cfg.cam_sensitivity << " (file)\n"; }
            else if (key == "move_speed") { cfg.cam_speed = std::stof(val); std::cout << "[config] move_speed=" << cfg.cam_speed << " (file)\n"; }
            else if (key == "fov_deg") { cfg.fov_deg = std::stof(val); std::cout << "[config] fov_deg=" << cfg.fov_deg << " (file)\n"; }
            else if (key == "near_m") { cfg.near_m = std::stof(val); std::cout << "[config] near_m=" << cfg.near_m << " (file)\n"; }
            else if (key == "far_m") { cfg.far_m = std::stof(val); std::cout << "[config] far_m=" << cfg.far_m << " (file)\n"; }
            else if (key == "walk_mode") { cfg.walk_mode = parse_bool(val, cfg.walk_mode); std::cout << "[config] walk_mode=" << (cfg.walk_mode ? "true" : "false") << " (file)\n"; }
            else if (key == "eye_height") { cfg.eye_height_m = std::stof(val); std::cout << "[config] eye_height=" << cfg.eye_height_m << " (file)\n"; }
            else if (key == "walk_speed") { cfg.walk_speed = std::stof(val); std::cout << "[config] walk_speed=" << cfg.walk_speed << " (file)\n"; }
            else if (key == "walk_pitch_max_deg") { cfg.walk_pitch_max_deg = std::stof(val); std::cout << "[config] walk_pitch_max_deg=" << cfg.walk_pitch_max_deg << " (file)\n"; }
            else if (key == "walk_surface_bias_m") { cfg.walk_surface_bias_m = std::stof(val); std::cout << "[config] walk_surface_bias_m=" << cfg.walk_surface_bias_m << " (file)\n"; }
            else if (key == "surface_push_m") { cfg.surface_push_m = std::stof(val); std::cout << "[config] surface_push_m=" << cfg.surface_push_m << " (file)\n"; }
            else if (key == "terrain_amp_m") { cfg.planet_cfg.terrain_amp_m = std::stod(val); std::cout << "[config] terrain_amp_m=" << cfg.planet_cfg.terrain_amp_m << " (file)\n"; }
            else if (key == "terrain_freq") { cfg.planet_cfg.terrain_freq = std::stof(val); std::cout << "[config] terrain_freq=" << cfg.planet_cfg.terrain_freq << " (file)\n"; }
            else if (key == "terrain_octaves") { cfg.planet_cfg.terrain_octaves = std::max(1, std::stoi(val)); std::cout << "[config] terrain_octaves=" << cfg.planet_cfg.terrain_octaves << " (file)\n"; }
            else if (key == "terrain_lacunarity") { cfg.planet_cfg.terrain_lacunarity = std::stof(val); std::cout << "[config] terrain_lacunarity=" << cfg.planet_cfg.terrain_lacunarity << " (file)\n"; }
            else if (key == "terrain_gain") { cfg.planet_cfg.terrain_gain = std::stof(val); std::cout << "[config] terrain_gain=" << cfg.planet_cfg.terrain_gain << " (file)\n"; }
            else if (key == "planet_seed") { cfg.planet_cfg.seed = static_cast<uint32_t>(std::stoul(val)); std::cout << "[config] planet_seed=" << cfg.planet_cfg.seed << " (file)\n"; }
            else if (key == "radius_m") { cfg.planet_cfg.radius_m = std::stod(val); std::cout << "[config] radius_m=" << cfg.planet_cfg.radius_m << " (file)\n"; }
            else if (key == "sea_level_m") { cfg.planet_cfg.sea_level_m = std::stod(val); std::cout << "[config] sea_level_m=" << cfg.planet_cfg.sea_level_m << " (file)\n"; }
            else if (key == "voxel_size_m") { cfg.planet_cfg.voxel_size_m = std::stod(val); std::cout << "[config] voxel_size_m=" << cfg.planet_cfg.voxel_size_m << " (file)\n"; }
            else if (key == "use_chunk_renderer") { cfg.use_chunk_renderer = parse_bool(val, cfg.use_chunk_renderer); std::cout << "[config] use_chunk_renderer=" << (cfg.use_chunk_renderer ? "true" : "false") << " (file)\n"; }
            else if (key == "ring_radius") { cfg.ring_radius = std::max(0, std::stoi(val)); std::cout << "[config] ring_radius=" << cfg.ring_radius << " (file)\n"; }
            else if (key == "prune_margin") { cfg.prune_margin = std::max(0, std::stoi(val)); std::cout << "[config] prune_margin=" << cfg.prune_margin << " (file)\n"; }
            else if (key == "cull") { cfg.cull_enabled = parse_bool(val, cfg.cull_enabled); std::cout << "[config] cull=" << (cfg.cull_enabled ? "true" : "false") << " (file)\n"; }
            else if (key == "draw_stats") { cfg.draw_stats_enabled = parse_bool(val, cfg.draw_stats_enabled); std::cout << "[config] draw_stats=" << (cfg.draw_stats_enabled ? "true" : "false") << " (file)\n"; }
            else if (key == "hud_scale") { cfg.hud_scale = std::stof(val); std::cout << "[config] hud_scale=" << cfg.hud_scale << " (file)\n"; }
            else if (key == "hud_shadow") { cfg.hud_shadow = parse_bool(val, cfg.hud_shadow); std::cout << "[config] hud_shadow=" << (cfg.hud_shadow ? "true" : "false") << " (file)\n"; }
            else if (key == "hud_shadow_offset") { cfg.hud_shadow_offset_px = std::stof(val); std::cout << "[config] hud_shadow_offset=" << cfg.hud_shadow_offset_px << " (file)\n"; }
            else if (key == "log_stream") { cfg.log_stream = parse_bool(val, cfg.log_stream); std::cout << "[config] log_stream=" << (cfg.log_stream ? "true" : "false") << " (file)\n"; }
            else if (key == "log_pool") { cfg.log_pool = parse_bool(val, cfg.log_pool); std::cout << "[config] log_pool=" << (cfg.log_pool ? "true" : "false") << " (file)\n"; }
            else if (key == "save_chunks") { cfg.save_chunks_enabled = parse_bool(val, cfg.save_chunks_enabled); std::cout << "[config] save_chunks=" << (cfg.save_chunks_enabled ? "true" : "false") << " (file)\n"; }
            else if (key == "debug_chunk_keys") { cfg.debug_chunk_keys = parse_bool(val, cfg.debug_chunk_keys); std::cout << "[config] debug_chunk_keys=" << (cfg.debug_chunk_keys ? "true" : "false") << " (file)\n"; }
            else if (key == "profile_csv") { cfg.profile_csv_enabled = parse_bool(val, cfg.profile_csv_enabled); std::cout << "[config] profile_csv=" << (cfg.profile_csv_enabled ? "true" : "false") << " (file)\n"; }
            else if (key == "profile_csv_path") { cfg.profile_csv_path = val; std::cout << "[config] profile_csv_path=" << cfg.profile_csv_path << " (file)\n"; }
            else if (key == "device_local") { cfg.device_local_enabled = parse_bool(val, cfg.device_local_enabled); std::cout << "[config] device_local=" << (cfg.device_local_enabled ? "true" : "false") << " (file)\n"; }
            else if (key == "pool_vtx_mb") { cfg.pool_vtx_mb = std::max(1, std::stoi(val)); std::cout << "[config] pool_vtx_mb=" << cfg.pool_vtx_mb << " (file)\n"; }
            else if (key == "pool_idx_mb") { cfg.pool_idx_mb = std::max(1, std::stoi(val)); std::cout << "[config] pool_idx_mb=" << cfg.pool_idx_mb << " (file)\n"; }
            else if (key == "uploads_per_frame") { cfg.uploads_per_frame_limit = std::max(1, std::stoi(val)); std::cout << "[config] uploads_per_frame=" << cfg.uploads_per_frame_limit << " (file)\n"; }
            else if (key == "loader_threads") { cfg.loader_threads = std::max(0, std::stoi(val)); std::cout << "[config] loader_threads=" << cfg.loader_threads << " (file)\n"; }
            else if (key == "k_down") { cfg.k_down = std::max(0, std::stoi(val)); std::cout << "[config] k_down=" << cfg.k_down << " (file)\n"; }
            else if (key == "k_up") { cfg.k_up = std::max(0, std::stoi(val)); std::cout << "[config] k_up=" << cfg.k_up << " (file)\n"; }
            else if (key == "k_prune_margin") { cfg.k_prune_margin = std::max(0, std::stoi(val)); std::cout << "[config] k_prune_margin=" << cfg.k_prune_margin << " (file)\n"; }
            else if (key == "face_keep_sec") { cfg.face_keep_time_cfg_s = std::max(0.0f, std::stof(val)); std::cout << "[config] face_keep_sec=" << cfg.face_keep_time_cfg_s << " (file)\n"; }
            else if (key == "region_root") { cfg.region_root = val; std::cout << "[config] region_root=" << cfg.region_root << " (file)\n"; }
        } catch (...) {
            // Ignore malformed entries; keep previous values.
        }
    }

    return true;
}

void apply_env_overrides(AppConfig& cfg) {
    apply_env_bool("WF_INVERT_MOUSE_X", cfg.invert_mouse_x);
    apply_env_bool("WF_INVERT_MOUSE_Y", cfg.invert_mouse_y);
    apply_env_value("WF_MOUSE_SENSITIVITY", cfg.cam_sensitivity, [&](const char* s) { cfg.cam_sensitivity = std::stof(s); });
    apply_env_value("WF_MOVE_SPEED", cfg.cam_speed, [&](const char* s) { cfg.cam_speed = std::stof(s); });
    apply_env_value("WF_FOV_DEG", cfg.fov_deg, [&](const char* s) { cfg.fov_deg = std::stof(s); });
    apply_env_value("WF_NEAR_M", cfg.near_m, [&](const char* s) { cfg.near_m = std::stof(s); });
    apply_env_value("WF_FAR_M", cfg.far_m, [&](const char* s) { cfg.far_m = std::stof(s); });

    apply_env_value("WF_TERRAIN_AMP_M", cfg.planet_cfg.terrain_amp_m, [&](const char* s) { cfg.planet_cfg.terrain_amp_m = std::stod(s); });
    apply_env_value("WF_TERRAIN_FREQ", cfg.planet_cfg.terrain_freq, [&](const char* s) { cfg.planet_cfg.terrain_freq = std::stof(s); });
    apply_env_value("WF_TERRAIN_OCTAVES", cfg.planet_cfg.terrain_octaves, [&](const char* s) { cfg.planet_cfg.terrain_octaves = std::max(1, std::stoi(s)); });
    apply_env_value("WF_TERRAIN_LACUNARITY", cfg.planet_cfg.terrain_lacunarity, [&](const char* s) { cfg.planet_cfg.terrain_lacunarity = std::stof(s); });
    apply_env_value("WF_TERRAIN_GAIN", cfg.planet_cfg.terrain_gain, [&](const char* s) { cfg.planet_cfg.terrain_gain = std::stof(s); });
    apply_env_value("WF_PLANET_SEED", cfg.planet_cfg.seed, [&](const char* s) { cfg.planet_cfg.seed = static_cast<uint32_t>(std::stoul(s)); });
    apply_env_value("WF_RADIUS_M", cfg.planet_cfg.radius_m, [&](const char* s) { cfg.planet_cfg.radius_m = std::stod(s); });
    apply_env_value("WF_SEA_LEVEL_M", cfg.planet_cfg.sea_level_m, [&](const char* s) { cfg.planet_cfg.sea_level_m = std::stod(s); });
    apply_env_value("WF_VOXEL_SIZE_M", cfg.planet_cfg.voxel_size_m, [&](const char* s) { cfg.planet_cfg.voxel_size_m = std::stod(s); });

    apply_env_value("WF_HUD_SCALE", cfg.hud_scale, [&](const char* s) { cfg.hud_scale = std::stof(s); });
    apply_env_bool("WF_HUD_SHADOW", cfg.hud_shadow);
    apply_env_value("WF_HUD_SHADOW_OFFSET", cfg.hud_shadow_offset_px, [&](const char* s) { cfg.hud_shadow_offset_px = std::stof(s); });

    apply_env_bool("WF_WALK_MODE", cfg.walk_mode);
    apply_env_value("WF_EYE_HEIGHT", cfg.eye_height_m, [&](const char* s) { cfg.eye_height_m = std::stof(s); });
    apply_env_value("WF_WALK_SPEED", cfg.walk_speed, [&](const char* s) { cfg.walk_speed = std::stof(s); });
    apply_env_value("WF_WALK_PITCH_MAX_DEG", cfg.walk_pitch_max_deg, [&](const char* s) { cfg.walk_pitch_max_deg = std::stof(s); });

    apply_env_bool("WF_USE_CHUNK_RENDERER", cfg.use_chunk_renderer);
    apply_env_value("WF_RING_RADIUS", cfg.ring_radius, [&](const char* s) { cfg.ring_radius = std::max(0, std::stoi(s)); });
    apply_env_value("WF_PRUNE_MARGIN", cfg.prune_margin, [&](const char* s) { cfg.prune_margin = std::max(0, std::stoi(s)); });
    apply_env_bool("WF_CULL", cfg.cull_enabled);
    apply_env_bool("WF_DRAW_STATS", cfg.draw_stats_enabled);
    apply_env_bool("WF_LOG_STREAM", cfg.log_stream);
    apply_env_bool("WF_LOG_POOL", cfg.log_pool);
    apply_env_bool("WF_SAVE_CHUNKS", cfg.save_chunks_enabled);
    apply_env_bool("WF_DEBUG_CHUNK_KEYS", cfg.debug_chunk_keys);
    apply_env_value("WF_PROFILE_CSV", cfg.profile_csv_enabled, [&](const char* s) { cfg.profile_csv_enabled = parse_bool(s, cfg.profile_csv_enabled); });
    apply_env_value("WF_PROFILE_CSV_PATH", cfg.profile_csv_path, [&](const char* s) { cfg.profile_csv_path = s; });

    apply_env_bool("WF_DEVICE_LOCAL", cfg.device_local_enabled);
    apply_env_value("WF_POOL_VTX_MB", cfg.pool_vtx_mb, [&](const char* s) { cfg.pool_vtx_mb = std::max(1, std::stoi(s)); });
    apply_env_value("WF_POOL_IDX_MB", cfg.pool_idx_mb, [&](const char* s) { cfg.pool_idx_mb = std::max(1, std::stoi(s)); });

    apply_env_value("WF_UPLOADS_PER_FRAME", cfg.uploads_per_frame_limit, [&](const char* s) { cfg.uploads_per_frame_limit = std::max(1, std::stoi(s)); });
    apply_env_value("WF_LOADER_THREADS", cfg.loader_threads, [&](const char* s) { cfg.loader_threads = std::max(0, std::stoi(s)); });
    apply_env_value("WF_K_DOWN", cfg.k_down, [&](const char* s) { cfg.k_down = std::max(0, std::stoi(s)); });
    apply_env_value("WF_K_UP", cfg.k_up, [&](const char* s) { cfg.k_up = std::max(0, std::stoi(s)); });
    apply_env_value("WF_K_PRUNE_MARGIN", cfg.k_prune_margin, [&](const char* s) { cfg.k_prune_margin = std::max(0, std::stoi(s)); });
    apply_env_value("WF_FACE_KEEP_SEC", cfg.face_keep_time_cfg_s, [&](const char* s) { cfg.face_keep_time_cfg_s = std::max(0.0f, std::stof(s)); });

    apply_env_value("WF_REGION_ROOT", cfg.region_root, [&](const char* s) { cfg.region_root = s; });
}

std::string bool_string(bool v) {
    return v ? "true" : "false";
}

void write_config_file(std::ostream& out, const AppConfig& cfg) {
    out << "# Wanderforge configuration\n";
    out << "# Generated at runtime -- feel free to edit\n\n";

    out << "invert_mouse_x=" << bool_string(cfg.invert_mouse_x) << '\n';
    out << "invert_mouse_y=" << bool_string(cfg.invert_mouse_y) << '\n';
    out << "mouse_sensitivity=" << cfg.cam_sensitivity << '\n';
    out << "move_speed=" << cfg.cam_speed << '\n';
    out << "fov_deg=" << cfg.fov_deg << '\n';
    out << "near_m=" << cfg.near_m << '\n';
    out << "far_m=" << cfg.far_m << '\n';
    out << "walk_mode=" << bool_string(cfg.walk_mode) << '\n';
    out << "eye_height=" << cfg.eye_height_m << '\n';
    out << "walk_speed=" << cfg.walk_speed << '\n';
    out << "walk_pitch_max_deg=" << cfg.walk_pitch_max_deg << '\n';
    out << "walk_surface_bias_m=" << cfg.walk_surface_bias_m << '\n';
    out << "surface_push_m=" << cfg.surface_push_m << '\n';

    out << "terrain_amp_m=" << cfg.planet_cfg.terrain_amp_m << '\n';
    out << "terrain_freq=" << cfg.planet_cfg.terrain_freq << '\n';
    out << "terrain_octaves=" << cfg.planet_cfg.terrain_octaves << '\n';
    out << "terrain_lacunarity=" << cfg.planet_cfg.terrain_lacunarity << '\n';
    out << "terrain_gain=" << cfg.planet_cfg.terrain_gain << '\n';
    out << "planet_seed=" << cfg.planet_cfg.seed << '\n';
    out << "radius_m=" << cfg.planet_cfg.radius_m << '\n';
    out << "sea_level_m=" << cfg.planet_cfg.sea_level_m << '\n';
    out << "voxel_size_m=" << cfg.planet_cfg.voxel_size_m << '\n';

    out << "use_chunk_renderer=" << bool_string(cfg.use_chunk_renderer) << '\n';
    out << "ring_radius=" << cfg.ring_radius << '\n';
    out << "prune_margin=" << cfg.prune_margin << '\n';
    out << "cull=" << bool_string(cfg.cull_enabled) << '\n';
    out << "draw_stats=" << bool_string(cfg.draw_stats_enabled) << '\n';

    out << "hud_scale=" << cfg.hud_scale << '\n';
    out << "hud_shadow=" << bool_string(cfg.hud_shadow) << '\n';
    out << "hud_shadow_offset=" << cfg.hud_shadow_offset_px << '\n';

    out << "log_stream=" << bool_string(cfg.log_stream) << '\n';
    out << "log_pool=" << bool_string(cfg.log_pool) << '\n';
    out << "save_chunks=" << bool_string(cfg.save_chunks_enabled) << '\n';
    out << "debug_chunk_keys=" << bool_string(cfg.debug_chunk_keys) << '\n';
    out << "profile_csv=" << bool_string(cfg.profile_csv_enabled) << '\n';
    out << "profile_csv_path=" << cfg.profile_csv_path << '\n';

    out << "device_local=" << bool_string(cfg.device_local_enabled) << '\n';
    out << "pool_vtx_mb=" << cfg.pool_vtx_mb << '\n';
    out << "pool_idx_mb=" << cfg.pool_idx_mb << '\n';

    out << "uploads_per_frame=" << cfg.uploads_per_frame_limit << '\n';
    out << "loader_threads=" << cfg.loader_threads << '\n';
    out << "k_down=" << cfg.k_down << '\n';
    out << "k_up=" << cfg.k_up << '\n';
    out << "k_prune_margin=" << cfg.k_prune_margin << '\n';
    out << "face_keep_sec=" << cfg.face_keep_time_cfg_s << '\n';

    out << "region_root=" << cfg.region_root << '\n';
}

bool configs_equal(const AppConfig& a, const AppConfig& b) {
    auto tie_planet = [](const PlanetConfig& p) {
        return std::tie(p.radius_m,
                        p.voxel_size_m,
                        p.sea_level_m,
                        p.seed,
                        p.terrain_amp_m,
                        p.terrain_freq,
                        p.terrain_octaves,
                        p.terrain_lacunarity,
                        p.terrain_gain);
    };

    if (tie_planet(a.planet_cfg) != tie_planet(b.planet_cfg)) {
        return false;
    }

    return std::tie(a.invert_mouse_x, a.invert_mouse_y, a.cam_sensitivity, a.cam_speed,
                    a.fov_deg, a.near_m, a.far_m, a.walk_mode, a.eye_height_m, a.walk_speed,
                    a.walk_pitch_max_deg, a.walk_surface_bias_m, a.surface_push_m,
                    a.use_chunk_renderer, a.ring_radius, a.prune_margin, a.cull_enabled,
                    a.draw_stats_enabled, a.hud_scale, a.hud_shadow, a.hud_shadow_offset_px,
                    a.log_stream, a.log_pool, a.save_chunks_enabled, a.debug_chunk_keys,
                    a.profile_csv_enabled, a.profile_csv_path, a.device_local_enabled,
                    a.pool_vtx_mb, a.pool_idx_mb, a.uploads_per_frame_limit, a.loader_threads,
                    a.k_down, a.k_up, a.k_prune_margin, a.face_keep_time_cfg_s,
                    a.region_root, a.config_path)
           ==
           std::tie(b.invert_mouse_x, b.invert_mouse_y, b.cam_sensitivity, b.cam_speed,
                    b.fov_deg, b.near_m, b.far_m, b.walk_mode, b.eye_height_m, b.walk_speed,
                    b.walk_pitch_max_deg, b.walk_surface_bias_m, b.surface_push_m,
                    b.use_chunk_renderer, b.ring_radius, b.prune_margin, b.cull_enabled,
                    b.draw_stats_enabled, b.hud_scale, b.hud_shadow, b.hud_shadow_offset_px,
                    b.log_stream, b.log_pool, b.save_chunks_enabled, b.debug_chunk_keys,
                    b.profile_csv_enabled, b.profile_csv_path, b.device_local_enabled,
                    b.pool_vtx_mb, b.pool_idx_mb, b.uploads_per_frame_limit, b.loader_threads,
                    b.k_down, b.k_up, b.k_prune_margin, b.face_keep_time_cfg_s,
                    b.region_root, b.config_path);
}

} // namespace

bool operator==(const AppConfig& a, const AppConfig& b) {
    return configs_equal(a, b);
}

bool operator!=(const AppConfig& a, const AppConfig& b) {
    return !configs_equal(a, b);
}

AppConfigManager::AppConfigManager(AppConfig defaults)
    : defaults_(std::move(defaults)),
      after_file_(defaults_),
      after_env_(defaults_),
      active_(defaults_) {
    resolved_config_path_ = defaults_.config_path;
}

void AppConfigManager::set_cli_config_path(std::string path) {
    cli_config_path_ = std::move(path);
}

bool AppConfigManager::apply_file_layer(AppConfig& cfg) {
    std::string path = resolved_config_path_;
    if (apply_file_overrides(path, cfg)) {
        file_layer_loaded_ = true;
        after_file_ = cfg;
        update_file_timestamp();
        return true;
    }
    file_layer_loaded_ = false;
    after_file_ = defaults_;
    last_write_time_.reset();
    return false;
}

void AppConfigManager::apply_env_layer(AppConfig& cfg) {
    apply_env_overrides(cfg);
    after_env_ = cfg;
}

bool AppConfigManager::rebuild_active(const AppConfig& base) {
    bool changed = (active_ != base);
    active_ = base;
    if (changed) {
        std::cout << "[config] active configuration updated" << '\n';
    }
    return changed;
}

bool AppConfigManager::update_file_timestamp() {
    if (!file_layer_loaded_) {
        last_write_time_.reset();
        return false;
    }
    std::error_code ec;
    auto timestamp = std::filesystem::last_write_time(resolved_config_path_, ec);
    if (ec) {
        return false;
    }
    last_write_time_ = timestamp;
    return true;
}

bool AppConfigManager::reload() {
    resolved_config_path_ = cli_config_path_.empty() ? defaults_.config_path : cli_config_path_;

    AppConfig merged = defaults_;
    if (!resolved_config_path_.empty()) {
        apply_file_layer(merged);
    } else {
        file_layer_loaded_ = false;
        after_file_ = defaults_;
        last_write_time_.reset();
    }

    apply_env_layer(merged);
    merged.config_path = resolved_config_path_.empty() ? defaults_.config_path : resolved_config_path_;
    return rebuild_active(merged);
}

bool AppConfigManager::reload_if_file_changed() {
    if (!file_layer_loaded_ || resolved_config_path_.empty()) {
        return false;
    }
    std::error_code ec;
    auto current = std::filesystem::last_write_time(resolved_config_path_, ec);
    if (ec) {
        return false;
    }
    if (!last_write_time_.has_value() || current != *last_write_time_) {
        return reload();
    }
    return false;
}

bool AppConfigManager::save_active_to_file() {
    if (resolved_config_path_.empty()) {
        std::cout << "[config] cannot save: config_path is empty" << '\n';
        return false;
    }

    std::ofstream out(resolved_config_path_, std::ios::trunc);
    if (!out.good()) {
        std::cout << "[config] failed to write " << resolved_config_path_ << '\n';
        return false;
    }

    write_config_file(out, active_);
    out.flush();
    file_layer_loaded_ = true;
    after_file_ = active_;
    after_env_ = active_;
    update_file_timestamp();
    std::cout << "[config] wrote " << resolved_config_path_ << '\n';
    return true;
}

void AppConfigManager::adopt_runtime_state(const AppConfig& cfg) {
    active_ = cfg;
    after_env_ = cfg;
}

} // namespace wf
