#include "world_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "world_streaming_subsystem.h"
#include "chunk.h"
#include "chunk_delta.h"

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

inline Float3 normalize_or(Float3 v, Float3 fallback) {
    float len = wf::length(v);
    return (len > 1e-5f) ? (v / len) : fallback;
}

inline Float3 to_float3(const double pos[3]) {
    return Float3{static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
}

inline float deg_to_rad(float deg) {
    return deg * 0.01745329252f;
}
}

struct WorldRuntime::Impl {
    Dependencies deps_{};
    std::unique_ptr<AppConfigManager> config_manager_;
    AppConfig active_config_{};
    bool config_loaded_ = false;
    std::chrono::steady_clock::time_point start_tp_ = std::chrono::steady_clock::now();

    float cam_yaw_ = 0.0f;
    float cam_pitch_ = 0.0f;
    double cam_pos_[3] = {1165.0, 12.0, 0.0};

    float cam_speed_ = 12.0f;
    float cam_sensitivity_ = 0.0025f;
    bool walk_mode_ = false;
    bool invert_mouse_x_ = true;
    bool invert_mouse_y_ = true;
    float walk_speed_ = 6.0f;
    float walk_pitch_max_deg_ = 60.0f;
    float walk_surface_bias_m_ = 1.0f;
    float eye_height_m_ = 1.7f;
    float surface_push_m_ = 0.0f;

    float aspect_ratio_ = 16.0f / 9.0f;

    bool config_dirty_ = false;

    std::vector<ChunkRenderable> chunk_renderables_;
    std::vector<EditCommand> pending_edits_;
    std::vector<AllowRegion> allow_regions_;
    std::vector<AllowRegion> allow_regions_prev_;
    std::vector<MeshUpload> mesh_uploads_;
    std::vector<FaceChunkKey> mesh_releases_;
    std::unordered_map<FaceChunkKey, std::size_t, FaceChunkKeyHash> renderable_lookup_;

    float face_switch_hysteresis_ = 0.05f;
    std::function<void(const std::string&)> profile_sink_;

    bool initialize(const CreateParams& params) {
        deps_ = params.deps;
        config_manager_ = std::make_unique<AppConfigManager>(params.initial_config);
        if (params.config_path_override && !params.config_path_override->empty()) {
            config_manager_->set_cli_config_path(*params.config_path_override);
        }
        if (deps_.streaming) {
            deps_.streaming->start();
            deps_.streaming->set_prev_face(-1);
            deps_.streaming->set_stream_face(-1);
            deps_.streaming->set_stream_face_ready(false);
        }
        config_manager_->reload();
        active_config_ = config_manager_->active();
        apply_config_internal(active_config_);
        config_loaded_ = true;
        start_tp_ = std::chrono::steady_clock::now();
        return true;
    }

    void shutdown() {
        chunk_renderables_.clear();
        pending_edits_.clear();
    }

    void apply_config(const AppConfig& cfg) {
        apply_config_internal(cfg);
        if (config_manager_) {
            config_manager_->adopt_runtime_state(active_config_);
        }
    }

    const AppConfig& active_config() const {
        return active_config_;
    }

    AppConfig snapshot_config() const {
        AppConfig cfg = active_config_;
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

    bool reload_config() {
        if (!config_manager_) {
            return false;
        }
        if (config_manager_->reload()) {
            active_config_ = config_manager_->active();
            apply_config_internal(active_config_);
            return true;
        }
        return false;
    }

    bool save_config() {
        if (!config_manager_) {
            return false;
        }
        AppConfig runtime = snapshot_config();
        const std::string& path = config_manager_->config_path();
        if (!path.empty()) {
            runtime.config_path = path;
        }
        config_manager_->adopt_runtime_state(runtime);
        if (config_manager_->save_active_to_file()) {
            if (config_manager_->reload()) {
                active_config_ = config_manager_->active();
                apply_config_internal(active_config_);
            }
            return true;
        }
        return false;
    }

    WorldUpdateResult update(const WorldUpdateInput& input) {
        WorldUpdateResult result{};
        if (!config_loaded_) {
            return result;
        }

        double dt = std::max(0.0, input.dt);
        bool requested_walk_mode = input.walk_mode;
        if (requested_walk_mode != walk_mode_) {
            walk_mode_ = requested_walk_mode;
            active_config_.walk_mode = walk_mode_;
            config_dirty_ = true;
        } else {
            walk_mode_ = requested_walk_mode;
            active_config_.walk_mode = walk_mode_;
        }

        bool reloaded = input.reload_config ? reload_config() : false;
        bool saved = input.save_config ? save_config() : false;

        if (!input.edits.empty()) {
            pending_edits_.insert(pending_edits_.end(), input.edits.begin(), input.edits.end());
            result.streaming_dirty = true;
        }

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
        Float3 updir = walk_mode_ ? wf::normalize(pos_f) : Float3{0.0f, 1.0f, 0.0f};
        float cy = std::cos(cam_yaw_);
        float sy = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_);
        float sp = std::sin(cam_pitch_);
        Float3 forward = normalize_or(Float3{cp * cy, sp, cp * sy}, Float3{1.0f, 0.0f, 0.0f});
        Float3 world_up = walk_mode_ ? updir : Float3{0.0f, 1.0f, 0.0f};
        Float3 right = normalize_or(cross3(forward, world_up), Float3{0.0f, 0.0f, 1.0f});

        float speed_scale = input.sprint ? (walk_mode_ ? 2.0f : 3.0f) : 1.0f;
        float base_speed = walk_mode_ ? walk_speed_ : cam_speed_;
        float step_scale = base_speed * static_cast<float>(dt) * speed_scale;

        if (!walk_mode_) {
            Float3 delta = Float3{0.0f, 0.0f, 0.0f};
            delta = delta + forward * input.move.forward;
            delta = delta + right * input.move.strafe;
            delta = delta + Float3{0.0f, 1.0f, 0.0f} * input.move.vertical;
            if (wf::length(delta) > 0.0f) {
                delta = wf::normalize(delta) * step_scale;
                cam_pos_[0] += delta.x;
                cam_pos_[1] += delta.y;
                cam_pos_[2] += delta.z;
            }
        } else {
            Float3 tangent_forward = normalize_or(forward - updir * dot3(forward, updir), right);
            Float3 tangent_right = normalize_or(cross3(tangent_forward, updir), Float3{0.0f, 1.0f, 0.0f});
            Float3 step = Float3{0.0f, 0.0f, 0.0f};
            step = step + tangent_forward * input.move.forward;
            step = step + tangent_right * input.move.strafe;
            if (wf::length(step) > 0.0f) {
                step = wf::normalize(step) * step_scale;
                Float3 up = wf::normalize(pos_f);
                Float3 direction = wf::normalize(step);
                double cam_radius = std::sqrt(cam_pos_[0] * cam_pos_[0] + cam_pos_[1] * cam_pos_[1] + cam_pos_[2] * cam_pos_[2]);
                float angle = static_cast<float>(step_scale / std::max(cam_radius, 1e-6));
                Float3 rotated = up * std::cos(angle) + direction * std::sin(angle);
                rotated = wf::normalize(rotated);
                pos_f = rotated;
                double target_radius = cam_radius;
                cam_pos_[0] = rotated.x * target_radius;
                cam_pos_[1] = rotated.y * target_radius;
                cam_pos_[2] = rotated.z * target_radius;
            }

            if (input.ground_follow) {
                Float3 ndir = wf::normalize(to_float3(cam_pos_));
                double h = terrain_height_m(active_config_.planet_cfg, ndir);
                double surface_r = active_config_.planet_cfg.radius_m + h;
                if (surface_r < active_config_.planet_cfg.sea_level_m) {
                    surface_r = active_config_.planet_cfg.sea_level_m;
                }
                double target_r = surface_r + static_cast<double>(eye_height_m_ + walk_surface_bias_m_);
                cam_pos_[0] = ndir.x * target_r;
                cam_pos_[1] = ndir.y * target_r;
                cam_pos_[2] = ndir.z * target_r;
            }
        }

        bool moved = (std::fabs(cam_pos_[0] - prev_pos[0]) > 1e-5 ||
                      std::fabs(cam_pos_[1] - prev_pos[1]) > 1e-5 ||
                      std::fabs(cam_pos_[2] - prev_pos[2]) > 1e-5);
        bool rotated = (std::fabs(cam_yaw_ - prev_yaw) > 1e-5f || std::fabs(cam_pitch_ - prev_pitch) > 1e-5f);
        result.camera_changed = moved || rotated;
        result.streaming_dirty = result.streaming_dirty || result.camera_changed;

        bool streaming_changed = update_streaming_state(dt, forward);
        bool uploads = drain_mesh_results();
        bool releases = prune_renderables();
        if (streaming_changed || uploads || releases) {
            result.streaming_dirty = true;
        }

        if (config_dirty_ || reloaded || saved) {
            result.config_changed = true;
            config_dirty_ = false;
        }

        return result;
    }

    CameraSnapshot snapshot_camera() const {
        CameraSnapshot snap{};
        snap.position = to_float3(cam_pos_);
        float cy = std::cos(cam_yaw_);
        float sy = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_);
        float sp = std::sin(cam_pitch_);
        snap.forward = normalize_or(Float3{cp * cy, sp, cp * sy}, Float3{1.0f, 0.0f, 0.0f});
        snap.up = walk_mode_ ? wf::normalize(snap.position) : Float3{0.0f, 1.0f, 0.0f};
        Float3 center = snap.position + snap.forward;
        snap.view = wf::look_at_rh({snap.position.x, snap.position.y, snap.position.z},
                                   {center.x, center.y, center.z},
                                   {snap.up.x, snap.up.y, snap.up.z});
        float fovy = deg_to_rad(active_config_.fov_deg);
        snap.projection = wf::perspective_vk(fovy, aspect_ratio_, active_config_.near_m, active_config_.far_m);
        snap.fov_deg = active_config_.fov_deg;
        snap.near_plane = active_config_.near_m;
        snap.far_plane = active_config_.far_m;
        return snap;
    }

    StreamStatus snapshot_stream_status() const {
        StreamStatus status{};
        if (!deps_.streaming) {
            return status;
        }
        status.queued_jobs = deps_.streaming->result_queue_depth();
        status.last_generation_ms = deps_.streaming->last_generation_ms();
        status.last_generated_chunks = deps_.streaming->last_generated_chunks();
        status.last_mesh_ms = deps_.streaming->last_mesh_ms();
        status.last_meshed_chunks = deps_.streaming->last_meshed_chunks();
        status.loader_busy = deps_.streaming->loader_busy();
        return status;
    }

    WorldRenderSnapshot snapshot_renderables() const {
        WorldRenderSnapshot snap{};
        snap.camera = snapshot_camera();
        snap.chunks = std::span<const ChunkRenderable>(chunk_renderables_.data(), chunk_renderables_.size());
        snap.allow_regions = std::span<const AllowRegion>(allow_regions_.data(), allow_regions_.size());
        return snap;
    }

    void queue_edit(EditCommand edit) {
        pending_edits_.push_back(edit);
    }

    void clear_pending_edits() {
        pending_edits_.clear();
    }

    std::span<const AllowRegion> allow_regions() const {
        return std::span<const AllowRegion>(allow_regions_.data(), allow_regions_.size());
    }

    void set_profile_sink(std::function<void(const std::string&)> sink) {
        profile_sink_ = std::move(sink);
    }

    void sync_camera_state(const Float3& position, float yaw_rad, float pitch_rad, bool walk_mode) {
        cam_pos_[0] = position.x;
        cam_pos_[1] = position.y;
        cam_pos_[2] = position.z;
        cam_yaw_ = yaw_rad;
        cam_pitch_ = pitch_rad;
        walk_mode_ = walk_mode;
        active_config_.walk_mode = walk_mode;
        config_loaded_ = true;
    }

    std::span<const MeshUpload> mesh_uploads() const {
        return std::span<const MeshUpload>(mesh_uploads_.data(), mesh_uploads_.size());
    }

    std::span<const FaceChunkKey> mesh_releases() const {
        return std::span<const FaceChunkKey>(mesh_releases_.data(), mesh_releases_.size());
    }

    void consume_mesh_transfers(std::size_t uploads_processed, std::size_t releases_processed) {
        uploads_processed = std::min(uploads_processed, mesh_uploads_.size());
        releases_processed = std::min(releases_processed, mesh_releases_.size());
        if (uploads_processed > 0) {
            mesh_uploads_.erase(mesh_uploads_.begin(), mesh_uploads_.begin() + uploads_processed);
        }
        if (releases_processed > 0) {
            mesh_releases_.erase(mesh_releases_.begin(), mesh_releases_.begin() + releases_processed);
        }
    }

    void queue_chunk_remesh(const FaceChunkKey& key) {
        if (!deps_.streaming) {
            return;
        }
        deps_.streaming->queue_remesh(key);
    }

    bool process_pending_remeshes(std::size_t max_count) {
        if (!deps_.streaming) {
            return false;
        }

        std::size_t count = max_count > 0 ? max_count : deps_.streaming->remesh_per_frame_cap();
        auto batch = deps_.streaming->take_remesh_batch(count);
        if (batch.empty()) {
            return false;
        }

        bool any_uploads = false;
        for (const auto& key : batch) {
            auto chunk_opt = deps_.streaming->find_chunk_copy(key);
            if (!chunk_opt.has_value()) {
                continue;
            }

            Chunk64 chunk = *chunk_opt;
            ChunkDelta delta = deps_.streaming->load_delta_copy(key);
            deps_.streaming->normalize_delta(delta);
            apply_chunk_delta(delta, chunk);

            ChunkStreamingManager::MeshResult res;
            if (!deps_.streaming->build_chunk_mesh(key, chunk, res)) {
                deps_.streaming->store_chunk(key, chunk);
                continue;
            }

            MeshUpload upload{};
            upload.key = key;
            upload.mesh.vertices = std::move(res.vertices);
            upload.mesh.indices = std::move(res.indices);
            upload.center[0] = res.center[0];
            upload.center[1] = res.center[1];
            upload.center[2] = res.center[2];
            upload.radius = res.radius;
            upload.job_generation = res.job_gen;
            mesh_uploads_.push_back(std::move(upload));

            deps_.streaming->store_chunk(key, chunk);
            any_uploads = true;
        }

        return any_uploads;
    }

    bool apply_voxel_edit(const VoxelHit& target,
                          uint16_t new_material,
                          int brush_dim) {
        if (!deps_.streaming) {
            return false;
        }

        brush_dim = std::max(brush_dim, 1);
        if (brush_dim > Chunk64::N) {
            brush_dim = Chunk64::N;
        }

        const PlanetConfig& cfg = active_config_.planet_cfg;
        int half = brush_dim / 2;
        bool even = (brush_dim % 2) == 0;
        int start = -half;
        int end = even ? half - 1 : half;

        struct PendingEdit {
            int lx;
            int ly;
            int lz;
            uint16_t base_material;
        };

        std::vector<PendingEdit> edits;
        edits.reserve(static_cast<std::size_t>(brush_dim * brush_dim * brush_dim));

        for (int dz = start; dz <= end; ++dz) {
            int lz = target.z + dz;
            if (lz < 0 || lz >= Chunk64::N) continue;
            for (int dy = start; dy <= end; ++dy) {
                int ly = target.y + dy;
                if (ly < 0 || ly >= Chunk64::N) continue;
                for (int dx = start; dx <= end; ++dx) {
                    int lx = target.x + dx;
                    if (lx < 0 || lx >= Chunk64::N) continue;
                    wf::i64 vx = target.voxel.x + static_cast<wf::i64>(dx);
                    wf::i64 vy = target.voxel.y + static_cast<wf::i64>(dy);
                    wf::i64 vz = target.voxel.z + static_cast<wf::i64>(dz);
                    BaseSample base = sample_base(cfg, Int3{vx, vy, vz});
                    edits.push_back(PendingEdit{lx, ly, lz, base.material});
                }
            }
        }

        if (edits.empty()) {
            return false;
        }

        std::vector<FaceChunkKey> neighbors_to_remesh;
        bool updated = deps_.streaming->modify_chunk_and_delta(
            target.key,
            [&](Chunk64& chunk_in_cache) {
                for (const PendingEdit& edit : edits) {
                    chunk_in_cache.set_voxel(edit.lx, edit.ly, edit.lz, new_material);
                }
            },
            [&](ChunkDelta& delta) {
                for (const PendingEdit& edit : edits) {
                    uint32_t lidx = Chunk64::lindex(edit.lx, edit.ly, edit.lz);
                    delta.apply_edit(lidx, edit.base_material, new_material);
                }
            },
            neighbors_to_remesh);

        if (!updated) {
            return false;
        }

        deps_.streaming->queue_remesh(target.key);
        for (const FaceChunkKey& neighbor : neighbors_to_remesh) {
            deps_.streaming->queue_remesh(neighbor);
        }

        return true;
    }

private:
    bool update_streaming_state(double dt, const Float3& forward) {
        if (!deps_.streaming) {
            return false;
        }

        bool changed = false;
        const PlanetConfig& cfg = active_config_.planet_cfg;
        const int N = Chunk64::N;
        const double chunk_m = static_cast<double>(N) * cfg.voxel_size_m;

        Float3 eye = to_float3(cam_pos_);
        Float3 dir = wf::normalize(eye);

        int raw_face = face_from_direction(dir);
        int prev_face = deps_.streaming->stream_face();
        int chosen_face = raw_face;
        if (prev_face >= 0) {
            Float3 prev_right, prev_up, prev_forward;
            face_basis(prev_face, prev_right, prev_up, prev_forward);
            float prev_align = std::fabs(dot3(dir, prev_forward));
            if (raw_face != prev_face) {
                Float3 cand_right, cand_up, cand_forward;
                face_basis(raw_face, cand_right, cand_up, cand_forward);
                float cand_align = std::fabs(dot3(dir, cand_forward));
                if (cand_align < prev_align + face_switch_hysteresis_) {
                    chosen_face = prev_face;
                }
            }
        }

        Float3 face_right, face_up, face_forward;
        face_basis(chosen_face, face_right, face_up, face_forward);

        double s = eye.x * face_right.x + eye.y * face_right.y + eye.z * face_right.z;
        double t = eye.x * face_up.x + eye.y * face_up.y + eye.z * face_up.z;
        std::int64_t ci = static_cast<std::int64_t>(std::floor(s / chunk_m));
        std::int64_t cj = static_cast<std::int64_t>(std::floor(t / chunk_m));
        std::int64_t ck = static_cast<std::int64_t>(std::floor(wf::length(eye) / chunk_m));

        bool face_changed = (chosen_face != prev_face);
        if (prev_face < 0) {
            face_changed = true;
        }

        float fwd_s = forward.x * face_right.x + forward.y * face_right.y + forward.z * face_right.z;
        float fwd_t = forward.x * face_up.x + forward.y * face_up.y + forward.z * face_up.z;

        if (face_changed) {
            deps_.streaming->set_prev_face(prev_face);
            deps_.streaming->set_prev_center_i(deps_.streaming->ring_center_i());
            deps_.streaming->set_prev_center_j(deps_.streaming->ring_center_j());
            deps_.streaming->set_prev_center_k(deps_.streaming->ring_center_k());
            deps_.streaming->set_face_keep_timer_s(active_config_.face_keep_time_cfg_s);
            deps_.streaming->set_stream_face(chosen_face);
            deps_.streaming->set_ring_center_i(ci);
            deps_.streaming->set_ring_center_j(cj);
            deps_.streaming->set_ring_center_k(ck);
            enqueue_ring_request(chosen_face,
                                 active_config_.ring_radius,
                                 ci,
                                 cj,
                                 ck,
                                 active_config_.k_down,
                                 active_config_.k_up,
                                 fwd_s,
                                 fwd_t);
            changed = true;
        } else {
            bool moved_tile = (ci != deps_.streaming->ring_center_i() ||
                               cj != deps_.streaming->ring_center_j() ||
                               ck != deps_.streaming->ring_center_k());
            if (moved_tile) {
                deps_.streaming->set_ring_center_i(ci);
                deps_.streaming->set_ring_center_j(cj);
                deps_.streaming->set_ring_center_k(ck);
                enqueue_ring_request(chosen_face,
                                     active_config_.ring_radius,
                                     ci,
                                     cj,
                                     ck,
                                     active_config_.k_down,
                                     active_config_.k_up,
                                     fwd_s,
                                     fwd_t);
                changed = true;
            }
        }

        float timer = deps_.streaming->face_keep_timer_s();
        if (timer > 0.0f) {
            deps_.streaming->set_face_keep_timer_s(std::max(0.0f, timer - static_cast<float>(dt)));
        }

        if (!deps_.streaming->stream_face_ready() && deps_.streaming->loader_idle()) {
            deps_.streaming->set_stream_face_ready(true);
        }

        allow_regions_prev_ = allow_regions_;
        allow_regions_.clear();
        auto build_region = [&](int face, std::int64_t aci, std::int64_t acj, std::int64_t ack, bool relaxed) {
            AllowRegion region{};
            region.face = face;
            region.ci = aci;
            region.cj = acj;
            region.ck = ack;
            region.span = active_config_.ring_radius + active_config_.prune_margin + (relaxed ? 1 : 0);
            region.k_down = active_config_.k_down + active_config_.k_prune_margin + (relaxed ? 1 : 0);
            region.k_up = active_config_.k_up + active_config_.k_prune_margin + (relaxed ? 1 : 0);
            allow_regions_.push_back(region);
        };

        bool relaxed = !deps_.streaming->stream_face_ready();
        build_region(deps_.streaming->stream_face(),
                     deps_.streaming->ring_center_i(),
                     deps_.streaming->ring_center_j(),
                     deps_.streaming->ring_center_k(),
                     relaxed);

        bool keep_prev = (deps_.streaming->prev_face() >= 0) &&
                         (deps_.streaming->face_keep_timer_s() > 0.0f || !deps_.streaming->stream_face_ready());
        if (keep_prev) {
            build_region(deps_.streaming->prev_face(),
                         deps_.streaming->prev_center_i(),
                         deps_.streaming->prev_center_j(),
                         deps_.streaming->prev_center_k(),
                         relaxed);
        }

        if (allow_regions_ != allow_regions_prev_) {
            changed = true;
        }

        return changed;
    }

    void enqueue_ring_request(int face,
                              int ring_radius,
                              std::int64_t center_i,
                              std::int64_t center_j,
                              std::int64_t center_k,
                              int k_down,
                              int k_up,
                              float fwd_s,
                              float fwd_t) {
        if (!deps_.streaming) {
            return;
        }
        WorldStreamingSubsystem::LoadRequest req{};
        req.face = face;
        req.ring_radius = ring_radius;
        req.ci = center_i;
        req.cj = center_j;
        req.ck = center_k;
        req.k_down = k_down;
        req.k_up = k_up;
        req.fwd_s = fwd_s;
        req.fwd_t = fwd_t;
        uint64_t gen = deps_.streaming->enqueue_request(req);
        deps_.streaming->set_stream_face_ready(false);
        deps_.streaming->set_pending_request_gen(gen);
    }

    bool drain_mesh_results() {
        if (!deps_.streaming) {
            return false;
        }

        bool any = false;
        WorldStreamingSubsystem::MeshResult res;
        while (deps_.streaming->try_pop_result(res)) {
            MeshUpload upload{};
            upload.key = res.key;
            upload.mesh.vertices = std::move(res.vertices);
            upload.mesh.indices = std::move(res.indices);
            upload.center[0] = res.center[0];
            upload.center[1] = res.center[1];
            upload.center[2] = res.center[2];
            upload.radius = res.radius;
            upload.job_generation = res.job_gen;
            mesh_uploads_.push_back(std::move(upload));

            if (!deps_.streaming->stream_face_ready() &&
                res.key.face == deps_.streaming->stream_face() &&
                res.job_gen >= deps_.streaming->pending_request_gen()) {
                deps_.streaming->set_stream_face_ready(true);
            }

            update_renderable_entry(res.key, res.center, res.radius, res.job_gen);
            any = true;
        }
        return any;
    }

    bool prune_renderables() {
        if (allow_regions_.empty()) {
            return false;
        }

        auto inside_any = [&](const FaceChunkKey& key) -> bool {
            for (const auto& region : allow_regions_) {
                if (region.face != key.face) {
                    continue;
                }
                if (std::llabs(key.i - region.ci) > region.span ||
                    std::llabs(key.j - region.cj) > region.span) {
                    continue;
                }
                std::int64_t k_min = region.ck - region.k_down;
                std::int64_t k_max = region.ck + region.k_up;
                if (key.k < k_min || key.k > k_max) {
                    continue;
                }
                return true;
            }
            return false;
        };

        bool removed = false;
        for (std::size_t idx = 0; idx < chunk_renderables_.size();) {
            const auto& renderable = chunk_renderables_[idx];
            if (!inside_any(renderable.key)) {
                mesh_releases_.push_back(renderable.key);
                if (deps_.streaming) {
                    deps_.streaming->erase_chunk(renderable.key);
                }
                remove_renderable(idx);
                removed = true;
            } else {
                ++idx;
            }
        }
        return removed;
    }

    void update_renderable_entry(const FaceChunkKey& key,
                                 const float center[3],
                                 float radius,
                                 uint64_t handle) {
        auto it = renderable_lookup_.find(key);
        if (it == renderable_lookup_.end()) {
            ChunkRenderable renderable{};
            renderable.key = key;
            renderable.center = Float3{center[0], center[1], center[2]};
            renderable.radius = radius;
            renderable.mesh_handle = handle;
            renderable_lookup_[key] = chunk_renderables_.size();
            chunk_renderables_.push_back(renderable);
        } else {
            auto& renderable = chunk_renderables_[it->second];
            renderable.center = Float3{center[0], center[1], center[2]};
            renderable.radius = radius;
            renderable.mesh_handle = handle;
        }
    }

    void remove_renderable(std::size_t index) {
        if (index >= chunk_renderables_.size()) {
            return;
        }

        const FaceChunkKey key = chunk_renderables_[index].key;
        const std::size_t last_index = chunk_renderables_.size() - 1;
        if (index != last_index) {
            chunk_renderables_[index] = chunk_renderables_[last_index];
            renderable_lookup_[chunk_renderables_[index].key] = index;
        }
        chunk_renderables_.pop_back();
        renderable_lookup_.erase(key);
    }

    void apply_config_internal(const AppConfig& cfg) {
        active_config_ = cfg;
        cam_speed_ = cfg.cam_speed;
        cam_sensitivity_ = cfg.cam_sensitivity;
        invert_mouse_x_ = cfg.invert_mouse_x;
        invert_mouse_y_ = cfg.invert_mouse_y;
        walk_speed_ = cfg.walk_speed;
        walk_mode_ = cfg.walk_mode;
        walk_pitch_max_deg_ = cfg.walk_pitch_max_deg;
        walk_surface_bias_m_ = cfg.walk_surface_bias_m;
        eye_height_m_ = cfg.eye_height_m;
        surface_push_m_ = cfg.surface_push_m;
        config_dirty_ = true;

        if (deps_.streaming) {
            std::size_t remesh_cap = 4;
            std::size_t worker_hint = cfg.loader_threads > 0 ? static_cast<std::size_t>(cfg.loader_threads) : 0;
            deps_.streaming->configure(cfg.planet_cfg,
                                       cfg.region_root,
                                       cfg.save_chunks_enabled,
                                       cfg.log_stream,
                                       remesh_cap,
                                       worker_hint);
            std::function<void(const std::string&)> sink;
            if (cfg.profile_csv_enabled && profile_sink_) {
                sink = profile_sink_;
            }
            deps_.streaming->apply_runtime_settings(surface_push_m_,
                                                    cfg.debug_chunk_keys,
                                                    cfg.profile_csv_enabled,
                                                    std::move(sink),
                                                    start_tp_);
        }
    }
};

WorldRuntime::WorldRuntime() : impl_(std::make_unique<Impl>()) {}
WorldRuntime::~WorldRuntime() = default;

bool WorldRuntime::initialize(const CreateParams& params) {
    return impl_->initialize(params);
}

void WorldRuntime::shutdown() {
    impl_->shutdown();
}

WorldUpdateResult WorldRuntime::update(const WorldUpdateInput& input) {
    return impl_->update(input);
}

CameraSnapshot WorldRuntime::snapshot_camera() const {
    return impl_->snapshot_camera();
}

StreamStatus WorldRuntime::snapshot_stream_status() const {
    return impl_->snapshot_stream_status();
}

WorldRenderSnapshot WorldRuntime::snapshot_renderables() const {
    return impl_->snapshot_renderables();
}

std::span<const AllowRegion> WorldRuntime::active_allow_regions() const {
    return impl_->allow_regions();
}

std::span<const MeshUpload> WorldRuntime::pending_mesh_uploads() const {
    return impl_->mesh_uploads();
}

std::span<const FaceChunkKey> WorldRuntime::pending_mesh_releases() const {
    return impl_->mesh_releases();
}

const AppConfig& WorldRuntime::active_config() const {
    return impl_->active_config();
}

void WorldRuntime::apply_config(const AppConfig& cfg) {
    impl_->apply_config(cfg);
}

AppConfig WorldRuntime::snapshot_config() const {
    return impl_->snapshot_config();
}

void WorldRuntime::queue_edit(EditCommand edit) {
    impl_->queue_edit(edit);
}

void WorldRuntime::clear_pending_edits() {
    impl_->clear_pending_edits();
}

void WorldRuntime::consume_mesh_transfer_queues(std::size_t uploads_processed, std::size_t releases_processed) {
    impl_->consume_mesh_transfers(uploads_processed, releases_processed);
}

void WorldRuntime::set_profile_sink(std::function<void(const std::string&)> sink) {
    impl_->set_profile_sink(std::move(sink));
}

void WorldRuntime::sync_camera_state(const Float3& position, float yaw_rad, float pitch_rad, bool walk_mode) {
    impl_->sync_camera_state(position, yaw_rad, pitch_rad, walk_mode);
}

void WorldRuntime::queue_chunk_remesh(const FaceChunkKey& key) {
    impl_->queue_chunk_remesh(key);
}

bool WorldRuntime::apply_voxel_edit(const VoxelHit& target,
                                    uint16_t new_material,
                                    int brush_dim) {
    return impl_->apply_voxel_edit(target, new_material, brush_dim);
}

bool WorldRuntime::process_pending_remeshes(std::size_t max_count) {
    return impl_->process_pending_remeshes(max_count);
}

} // namespace wf
