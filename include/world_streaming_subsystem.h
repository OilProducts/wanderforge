#pragma once

#include <cstdint>

#include "chunk_streaming_manager.h"

namespace wf {

class WorldStreamingSubsystem {
public:
    WorldStreamingSubsystem() = default;

    ChunkStreamingManager& manager() { return manager_; }
    const ChunkStreamingManager& manager() const { return manager_; }

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

