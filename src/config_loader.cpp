#include "config_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

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
void apply_env(const char* key, T& field, Fn&& parse_and_assign) {
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

} // namespace

AppConfig load_app_config(const AppConfig& defaults, const std::string& cli_config_path) {
    AppConfig cfg = defaults;

    // Apply CLI override for config path first.
    if (!cli_config_path.empty()) {
        cfg.config_path = cli_config_path;
        std::cout << "[config] config_path=" << cfg.config_path << " (cli)\n";
    }

    // Environment overrides.
    apply_env_bool("WF_INVERT_MOUSE_X", cfg.invert_mouse_x);
    apply_env_bool("WF_INVERT_MOUSE_Y", cfg.invert_mouse_y);
    apply_env("WF_MOUSE_SENSITIVITY", cfg.cam_sensitivity, [&](const char* s) { cfg.cam_sensitivity = std::stof(s); });
    apply_env("WF_MOVE_SPEED", cfg.cam_speed, [&](const char* s) { cfg.cam_speed = std::stof(s); });
    apply_env("WF_FOV_DEG", cfg.fov_deg, [&](const char* s) { cfg.fov_deg = std::stof(s); });
    apply_env("WF_NEAR_M", cfg.near_m, [&](const char* s) { cfg.near_m = std::stof(s); });
    apply_env("WF_FAR_M", cfg.far_m, [&](const char* s) { cfg.far_m = std::stof(s); });

    apply_env("WF_TERRAIN_AMP_M", cfg.planet_cfg.terrain_amp_m, [&](const char* s) { cfg.planet_cfg.terrain_amp_m = std::stod(s); });
    apply_env("WF_TERRAIN_FREQ", cfg.planet_cfg.terrain_freq, [&](const char* s) { cfg.planet_cfg.terrain_freq = std::stof(s); });
    apply_env("WF_TERRAIN_OCTAVES", cfg.planet_cfg.terrain_octaves, [&](const char* s) { cfg.planet_cfg.terrain_octaves = std::max(1, std::stoi(s)); });
    apply_env("WF_TERRAIN_LACUNARITY", cfg.planet_cfg.terrain_lacunarity, [&](const char* s) { cfg.planet_cfg.terrain_lacunarity = std::stof(s); });
    apply_env("WF_TERRAIN_GAIN", cfg.planet_cfg.terrain_gain, [&](const char* s) { cfg.planet_cfg.terrain_gain = std::stof(s); });
    apply_env("WF_PLANET_SEED", cfg.planet_cfg.seed, [&](const char* s) { cfg.planet_cfg.seed = static_cast<uint32_t>(std::stoul(s)); });
    apply_env("WF_RADIUS_M", cfg.planet_cfg.radius_m, [&](const char* s) { cfg.planet_cfg.radius_m = std::stod(s); });
    apply_env("WF_SEA_LEVEL_M", cfg.planet_cfg.sea_level_m, [&](const char* s) { cfg.planet_cfg.sea_level_m = std::stod(s); });
    apply_env("WF_VOXEL_SIZE_M", cfg.planet_cfg.voxel_size_m, [&](const char* s) { cfg.planet_cfg.voxel_size_m = std::stod(s); });

    apply_env_bool("WF_WALK_MODE", cfg.walk_mode);
    apply_env("WF_EYE_HEIGHT", cfg.eye_height_m, [&](const char* s) { cfg.eye_height_m = std::stof(s); });
    apply_env("WF_WALK_SPEED", cfg.walk_speed, [&](const char* s) { cfg.walk_speed = std::stof(s); });
    apply_env("WF_WALK_PITCH_MAX_DEG", cfg.walk_pitch_max_deg, [&](const char* s) { cfg.walk_pitch_max_deg = std::stof(s); });

    apply_env_bool("WF_USE_CHUNK_RENDERER", cfg.use_chunk_renderer);
    apply_env("WF_RING_RADIUS", cfg.ring_radius, [&](const char* s) { cfg.ring_radius = std::max(0, std::stoi(s)); });
    apply_env("WF_PRUNE_MARGIN", cfg.prune_margin, [&](const char* s) { cfg.prune_margin = std::max(0, std::stoi(s)); });
    apply_env_bool("WF_CULL", cfg.cull_enabled);
    apply_env_bool("WF_DRAW_STATS", cfg.draw_stats_enabled);
    apply_env_bool("WF_LOG_STREAM", cfg.log_stream);
    apply_env_bool("WF_LOG_POOL", cfg.log_pool);
    apply_env_bool("WF_SAVE_CHUNKS", cfg.save_chunks_enabled);
    apply_env_bool("WF_DEBUG_CHUNK_KEYS", cfg.debug_chunk_keys);
    apply_env("WF_SURFACE_PUSH_M", cfg.surface_push_m, [&](const char* s) { cfg.surface_push_m = std::stof(s); });
    apply_env_bool("WF_PROFILE_CSV", cfg.profile_csv_enabled);
    apply_env("WF_PROFILE_CSV_PATH", cfg.profile_csv_path, [&](const char* s) { cfg.profile_csv_path = s; });
    apply_env_bool("WF_DEVICE_LOCAL", cfg.device_local_enabled);
    apply_env("WF_POOL_VTX_MB", cfg.pool_vtx_mb, [&](const char* s) { cfg.pool_vtx_mb = std::max(1, std::stoi(s)); });
    apply_env("WF_POOL_IDX_MB", cfg.pool_idx_mb, [&](const char* s) { cfg.pool_idx_mb = std::max(1, std::stoi(s)); });
    apply_env("WF_UPLOADS_PER_FRAME", cfg.uploads_per_frame_limit, [&](const char* s) { cfg.uploads_per_frame_limit = std::max(1, std::stoi(s)); });
    apply_env("WF_LOADER_THREADS", cfg.loader_threads, [&](const char* s) { cfg.loader_threads = std::max(0, std::stoi(s)); });
    apply_env("WF_K_DOWN", cfg.k_down, [&](const char* s) { cfg.k_down = std::max(0, std::stoi(s)); });
    apply_env("WF_K_UP", cfg.k_up, [&](const char* s) { cfg.k_up = std::max(0, std::stoi(s)); });
    apply_env("WF_K_PRUNE_MARGIN", cfg.k_prune_margin, [&](const char* s) { cfg.k_prune_margin = std::max(0, std::stoi(s)); });
    apply_env("WF_FACE_KEEP_SEC", cfg.face_keep_time_cfg_s, [&](const char* s) { cfg.face_keep_time_cfg_s = std::max(0.0f, std::stof(s)); });
    apply_env("WF_REGION_ROOT", cfg.region_root, [&](const char* s) { cfg.region_root = s; });

    if (cfg.config_path.empty()) {
        std::cout << "[config] config file path disabled; skipping file load\n";
        return cfg;
    }

    std::ifstream in(cfg.config_path);
    if (!in.good()) {
        std::cout << "[config] config file not found: " << cfg.config_path << " (using defaults/env)\n";
        return cfg;
    }

    std::cout << "[config] reading " << cfg.config_path << "\n";

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

    return cfg;
}

} // namespace wf
