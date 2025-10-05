#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "config_loader.h"
#include "planet.h"
#include "wf_math.h"
#include "mesh.h"

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

struct MovementAxes {
    float forward = 0.0f;
    float strafe = 0.0f;
    float vertical = 0.0f;
};

struct LookInput {
    float yaw_delta = 0.0f;
    float pitch_delta = 0.0f;
};

struct WorldUpdateInput {
    double dt = 0.0;
    MovementAxes move{};
    LookInput look{};
    bool walk_mode = false;
    bool sprint = false;
    bool ground_follow = false;
    bool clamp_pitch = true;
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

    void set_profile_sink(std::function<void(const std::string&)> sink);

    void queue_edit(EditCommand edit);
    void clear_pending_edits();
    void clear_mesh_transfer_queues();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wf
