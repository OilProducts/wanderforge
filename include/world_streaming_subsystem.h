#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "chunk_streaming_manager.h"

namespace wf {

class WorldStreamingSubsystem {
public:
    WorldStreamingSubsystem() = default;

    using MeshResult = ChunkStreamingManager::MeshResult;
    using LoadRequest = ChunkStreamingManager::LoadRequest;

    void configure(const PlanetConfig& planet_cfg,
                   const std::string& region_root,
                   bool save_chunks,
                   bool log_stream,
                   std::size_t remesh_per_frame_cap,
                   std::size_t worker_count_hint);
    void set_load_job(ChunkStreamingManager::LoadJob job);

    void start();
    void stop();
    void wait_for_pending_saves();

    ChunkStreamingManager& manager() { return manager_; }
    const ChunkStreamingManager& manager() const { return manager_; }

    uint64_t enqueue_request(LoadRequest req);
    bool should_abort(uint64_t job_gen) const;

    void push_mesh_result(MeshResult res);
    bool try_pop_result(MeshResult& out);

    void update_generation_stats(double gen_ms, int chunks);
    void update_mesh_stats(double mesh_ms, int meshed, double total_ms);

    void flush_dirty_chunk_deltas();
    void overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk);

    void erase_chunk(const FaceChunkKey& key);

    size_t result_queue_depth() const { return manager_.result_queue_depth(); }
    double last_generation_ms() const { return manager_.last_generation_ms(); }
    int last_generated_chunks() const { return manager_.last_generated_chunks(); }
    double last_mesh_ms() const { return manager_.last_mesh_ms(); }
    int last_meshed_chunks() const { return manager_.last_meshed_chunks(); }
    bool loader_busy() const { return manager_.loader_busy(); }
    bool loader_idle() const { return manager_.loader_idle(); }
    std::size_t remesh_per_frame_cap() const { return manager_.remesh_per_frame_cap(); }

    template <typename Fn>
    bool with_chunk(const FaceChunkKey& key, Fn&& fn) const {
        return manager_.with_chunk(key, std::forward<Fn>(fn));
    }

    template <typename Fn>
    void visit_neighbors(const FaceChunkKey& key, Fn&& fn) const {
        manager_.visit_neighbors(key, std::forward<Fn>(fn));
    }

    void queue_remesh(const FaceChunkKey& key);
    std::deque<FaceChunkKey> take_remesh_batch(std::size_t max_count);

    std::optional<Chunk64> find_chunk_copy(const FaceChunkKey& key) const;
    void store_chunk(const FaceChunkKey& key, const Chunk64& chunk);

    ChunkDelta load_delta_copy(const FaceChunkKey& key) const;
    void normalize_delta(ChunkDelta& delta) { manager_.normalize_chunk_delta_representation(delta); }

    template <typename Fn>
    void modify_chunk_delta(const FaceChunkKey& key, Fn&& fn) {
        std::unique_lock lock(manager_.chunk_delta_mutex());
        auto& deltas = manager_.chunk_deltas();
        ChunkDelta& delta = deltas.try_emplace(key, ChunkDelta{}).first->second;
        fn(delta);
        manager_.normalize_chunk_delta_representation(delta);
    }

    template <typename Fn>
    bool update_chunk(const FaceChunkKey& key, Fn&& fn) {
        return manager_.update_chunk(key, std::forward<Fn>(fn));
    }

    int stream_face() const { return stream_face_; }
    void set_stream_face(int face) { stream_face_ = face; }

    bool stream_face_ready() const { return stream_face_ready_; }
    void set_stream_face_ready(bool ready) { stream_face_ready_ = ready; }

    uint64_t pending_request_gen() const { return pending_request_gen_; }
    void set_pending_request_gen(uint64_t gen) { pending_request_gen_ = gen; }

    std::int64_t ring_center_i() const { return ring_center_i_; }
    void set_ring_center_i(std::int64_t value) { ring_center_i_ = value; }

    std::int64_t ring_center_j() const { return ring_center_j_; }
    void set_ring_center_j(std::int64_t value) { ring_center_j_ = value; }

    std::int64_t ring_center_k() const { return ring_center_k_; }
    void set_ring_center_k(std::int64_t value) { ring_center_k_ = value; }

    int prev_face() const { return prev_face_; }
    void set_prev_face(int face) { prev_face_ = face; }

    std::int64_t prev_center_i() const { return prev_center_i_; }
    void set_prev_center_i(std::int64_t value) { prev_center_i_ = value; }

    std::int64_t prev_center_j() const { return prev_center_j_; }
    void set_prev_center_j(std::int64_t value) { prev_center_j_ = value; }

    std::int64_t prev_center_k() const { return prev_center_k_; }
    void set_prev_center_k(std::int64_t value) { prev_center_k_ = value; }

    float face_keep_timer_s() const { return face_keep_timer_s_; }
    void set_face_keep_timer_s(float value) { face_keep_timer_s_ = value; }

private:
    ChunkStreamingManager manager_;
    int stream_face_ = 0;
    bool stream_face_ready_ = false;
    uint64_t pending_request_gen_ = 0;
    std::int64_t ring_center_i_ = 0;
    std::int64_t ring_center_j_ = 0;
    std::int64_t ring_center_k_ = 0;
    int prev_face_ = -1;
    std::int64_t prev_center_i_ = 0;
    std::int64_t prev_center_j_ = 0;
    std::int64_t prev_center_k_ = 0;
    float face_keep_timer_s_ = 0.0f;
};

} // namespace wf
