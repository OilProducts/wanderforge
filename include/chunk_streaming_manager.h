#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "planet.h"
#include "chunk.h"
#include "chunk_delta.h"
#include "mesh.h"
#include "region_io.h"
#include "wf_math.h"

namespace wf {

struct VoxelHit {
    FaceChunkKey key{};
    int x = 0;
    int y = 0;
    int z = 0;
    Int3 voxel{0, 0, 0};
    double world_pos[3] = {0.0, 0.0, 0.0};
    uint16_t material = MAT_AIR;
};

class ChunkStreamingManager {
public:
    struct MeshResult {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        FaceChunkKey key{0, 0, 0, 0};
        uint64_t job_gen = 0;
        uint32_t first_index = 0;
        int32_t base_vertex = 0;
    };

    struct LoadRequest {
        int face = 0;
        int ring_radius = 0;
        std::int64_t ci = 0;
        std::int64_t cj = 0;
        std::int64_t ck = 0;
        int k_down = 0;
        int k_up = 0;
        float fwd_s = 0.0f;
        float fwd_t = 0.0f;
        uint64_t gen = 0;
    };

    using LoadJob = std::function<void(const LoadRequest&)>;

    ChunkStreamingManager();

    void set_planet_config(const PlanetConfig& cfg);
    const PlanetConfig& planet_config() const { return planet_cfg_; }

    void set_region_root(std::string root);
    const std::string& region_root() const { return region_root_; }

    void set_save_chunks_enabled(bool enabled);
    bool save_chunks_enabled() const { return save_chunks_enabled_; }

    void set_log_stream(bool enabled) { log_stream_ = enabled; }
    bool log_stream() const { return log_stream_; }

    void set_remesh_per_frame_cap(std::size_t cap) { remesh_per_frame_cap_ = cap; }
    std::size_t remesh_per_frame_cap() const { return remesh_per_frame_cap_; }

    void set_load_job(LoadJob job);
    void start();
    void stop();

    uint64_t enqueue_request(LoadRequest req);
    bool try_pop_result(MeshResult& out);
    void push_mesh_result(MeshResult res);

    size_t result_queue_depth() const;
    bool loader_busy() const;
    bool loader_idle() const;
    uint64_t current_request_gen() const;
    bool should_abort(uint64_t job_gen) const;

    void update_generation_stats(double gen_ms, int chunks);
    void update_mesh_stats(double mesh_ms, int meshed, double total_ms);
    double last_generation_ms() const;
    int last_generated_chunks() const;
    double last_mesh_ms() const;
    int last_meshed_chunks() const;
    double last_total_ms() const;

    std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash>& chunk_cache();
    const std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash>& chunk_cache() const;
    std::mutex& chunk_cache_mutex() const;

    std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash>& chunk_deltas();
    const std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash>& chunk_deltas() const;
    std::mutex& chunk_delta_mutex() const;

    std::deque<FaceChunkKey>& remesh_queue();
    std::mutex& remesh_mutex() const;

    void normalize_chunk_delta_representation(ChunkDelta& delta);
    void overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk);
    void flush_dirty_chunk_deltas();

private:
    void loader_thread_main();

    PlanetConfig planet_cfg_{};
    bool save_chunks_enabled_ = false;
    bool log_stream_ = false;
    std::string region_root_ = "regions";
    std::size_t remesh_per_frame_cap_ = 4;

    LoadJob load_job_;
    std::thread loader_thread_;
    mutable std::mutex loader_mutex_;
    std::condition_variable loader_cv_;
    std::atomic<bool> loader_quit_{false};
    std::atomic<bool> loader_busy_atomic_{false};
    std::deque<LoadRequest> request_queue_;
    std::deque<MeshResult> results_queue_;
    std::atomic<uint64_t> request_gen_{0};
    std::atomic<double> loader_last_gen_ms_{0.0};
    std::atomic<int> loader_last_chunks_{0};
    std::atomic<double> loader_last_mesh_ms_{0.0};
    std::atomic<int> loader_last_meshed_{0};
    std::atomic<double> loader_last_total_ms_{0.0};

    std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash> chunk_deltas_;
    mutable std::mutex chunk_delta_mutex_;

    std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash> chunk_cache_;
    mutable std::mutex chunk_cache_mutex_;

    std::deque<FaceChunkKey> remesh_queue_;
    mutable std::mutex remesh_mutex_;
};

} // namespace wf
