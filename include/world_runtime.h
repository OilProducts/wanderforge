#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "config_loader.h"
#include "chunk_streaming_manager.h"
#include "mesh.h"
#include "planet.h"
#include "wf_math.h"
#include "camera_controller.h"

namespace wf {

class TaskScheduler;
class WorldStreamingSubsystem;

struct EditCommand {
    enum class Type {
        AddVoxel,
        RemoveVoxel,
        PaintMaterial
    };

    FaceChunkKey key{};
    int16_t local_x = 0;
    int16_t local_y = 0;
    int16_t local_z = 0;
    uint16_t material = 0;
};

struct StreamStatus {
    std::size_t queued_jobs = 0;
    double last_generation_ms = 0.0;
    int last_generated_chunks = 0;
    double last_mesh_ms = 0.0;
    int last_meshed_chunks = 0;
    bool loader_busy = false;
};

struct ChunkRenderable {
    FaceChunkKey key{};
    wf::Float3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::uint64_t mesh_handle = 0;
};

struct AllowRegion {
    int face = -1;
    std::int64_t ci = 0;
    std::int64_t cj = 0;
    std::int64_t ck = 0;
    int span = 0;
    int k_down = 0;
    int k_up = 0;
};

inline bool operator==(const AllowRegion& a, const AllowRegion& b) {
    return a.face == b.face && a.ci == b.ci && a.cj == b.cj && a.ck == b.ck &&
           a.span == b.span && a.k_down == b.k_down && a.k_up == b.k_up;
}

inline bool operator!=(const AllowRegion& a, const AllowRegion& b) {
    return !(a == b);
}

struct MeshUpload {
    FaceChunkKey key{};
    wf::Mesh mesh{};
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    uint64_t job_generation = 0;
};

struct WorldRenderSnapshot {
    CameraSnapshot camera{};
    std::span<const ChunkRenderable> chunks{};
    std::span<const AllowRegion> allow_regions{};
};

struct WorldUpdateInput {
    double dt = 0.0;
    MovementAxes move{};
    LookInput look{};
    bool walk_mode = false;
    bool sprint = false;
    bool ground_follow = false;
    bool clamp_pitch = true;
    bool toggle_walk_mode = false;
    bool toggle_invert_x = false;
    bool toggle_invert_y = false;
    std::span<const EditCommand> edits{};
    bool reload_config = false;
    bool save_config = false;
};

struct WorldUpdateResult {
    bool camera_changed = false;
    bool config_changed = false;
    bool streaming_dirty = false;
};

// Simulation hub for world streaming, camera, and edit application.
class WorldRuntime {
public:
    struct Dependencies {
        WorldStreamingSubsystem* streaming = nullptr;
        TaskScheduler* scheduler = nullptr;
    };

    struct CreateParams {
        Dependencies deps{};
        AppConfig initial_config{};
        std::optional<std::string> config_path_override;
    };

    WorldRuntime();
    ~WorldRuntime();

    bool initialize(const CreateParams& params);
    void shutdown();

    WorldUpdateResult update(const WorldUpdateInput& input);

    CameraSnapshot snapshot_camera() const;
    StreamStatus snapshot_stream_status() const;
    WorldRenderSnapshot snapshot_renderables() const;
    std::span<const AllowRegion> active_allow_regions() const;
    std::span<const MeshUpload> pending_mesh_uploads() const;
    std::span<const FaceChunkKey> pending_mesh_releases() const;

    const AppConfig& active_config() const;
    void apply_config(const AppConfig& cfg);
    AppConfig snapshot_config() const;
    void set_cli_config_path(std::string path);
    bool reload_config();
    bool reload_config_if_file_changed();
    bool save_active_config();
    bool has_config_file() const;
    const std::string& active_config_path() const;

    void set_profile_sink(std::function<void(const std::string&)> sink);

    void queue_edit(EditCommand edit);
    void clear_pending_edits();
    void consume_mesh_transfer_queues(std::size_t uploads_processed, std::size_t releases_processed);
    void sync_camera_state(const wf::Float3& position, float yaw_rad, float pitch_rad, bool walk_mode);
    void queue_chunk_remesh(const FaceChunkKey& key);
    bool apply_voxel_edit(const VoxelHit& target, uint16_t new_material, int brush_dim);
    bool process_pending_remeshes(std::size_t max_count = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wf
