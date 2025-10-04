#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "chunk_streaming_manager.h"

namespace wf {

class WorldStreamingSubsystem {
public:
    WorldStreamingSubsystem() = default;

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

    size_t result_queue_depth() const { return manager_.result_queue_depth(); }
    double last_generation_ms() const { return manager_.last_generation_ms(); }
    int last_generated_chunks() const { return manager_.last_generated_chunks(); }
    double last_mesh_ms() const { return manager_.last_mesh_ms(); }
    int last_meshed_chunks() const { return manager_.last_meshed_chunks(); }
    bool loader_busy() const { return manager_.loader_busy(); }

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

