#include "world_streaming_subsystem.h"

#include <mutex>
#include <utility>

namespace wf {

void WorldStreamingSubsystem::configure(const PlanetConfig& planet_cfg,
                                        const std::string& region_root,
                                        bool save_chunks,
                                        bool log_stream,
                                        std::size_t remesh_per_frame_cap,
                                        std::size_t worker_count_hint) {
    manager_.set_planet_config(planet_cfg);
    manager_.set_region_root(region_root);
    manager_.set_save_chunks_enabled(save_chunks);
    manager_.set_log_stream(log_stream);
    manager_.set_remesh_per_frame_cap(remesh_per_frame_cap);
    manager_.set_worker_count(worker_count_hint);
}

void WorldStreamingSubsystem::set_load_job(ChunkStreamingManager::LoadJob job) {
    manager_.set_load_job(std::move(job));
}

void WorldStreamingSubsystem::start() {
    manager_.start();
}

void WorldStreamingSubsystem::stop() {
    manager_.stop();
}

void WorldStreamingSubsystem::wait_for_pending_saves() {
    manager_.wait_for_pending_saves();
}

uint64_t WorldStreamingSubsystem::enqueue_request(LoadRequest req) {
    return manager_.enqueue_request(std::move(req));
}

bool WorldStreamingSubsystem::should_abort(uint64_t job_gen) const {
    return manager_.should_abort(job_gen);
}

void WorldStreamingSubsystem::push_mesh_result(MeshResult res) {
    manager_.push_mesh_result(std::move(res));
}

bool WorldStreamingSubsystem::try_pop_result(MeshResult& out) {
    return manager_.try_pop_result(out);
}

void WorldStreamingSubsystem::update_generation_stats(double gen_ms, int chunks) {
    manager_.update_generation_stats(gen_ms, chunks);
}

void WorldStreamingSubsystem::update_mesh_stats(double mesh_ms, int meshed, double total_ms) {
    manager_.update_mesh_stats(mesh_ms, meshed, total_ms);
}

void WorldStreamingSubsystem::flush_dirty_chunk_deltas() {
    manager_.flush_dirty_chunk_deltas();
}

void WorldStreamingSubsystem::overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk) {
    manager_.overlay_chunk_delta(key, chunk);
}

void WorldStreamingSubsystem::erase_chunk(const FaceChunkKey& key) {
    manager_.erase_chunk(key);
}

void WorldStreamingSubsystem::queue_remesh(const FaceChunkKey& key) {
    std::scoped_lock lock(manager_.remesh_mutex());
    manager_.remesh_queue().push_back(key);
}

std::deque<FaceChunkKey> WorldStreamingSubsystem::take_remesh_batch(std::size_t max_count) {
    std::deque<FaceChunkKey> batch;
    std::scoped_lock lock(manager_.remesh_mutex());
    auto& queue = manager_.remesh_queue();
    std::size_t taken = 0;
    while (!queue.empty() && taken < max_count) {
        batch.push_back(queue.front());
        queue.pop_front();
        ++taken;
    }
    return batch;
}

std::optional<Chunk64> WorldStreamingSubsystem::find_chunk_copy(const FaceChunkKey& key) const {
    std::optional<Chunk64> copy;
    manager_.with_chunk(key, [&](const Chunk64& chunk) {
        copy = chunk;
    });
    return copy;
}

void WorldStreamingSubsystem::store_chunk(const FaceChunkKey& key, const Chunk64& chunk) {
    manager_.store_chunk(key, chunk);
}

ChunkDelta WorldStreamingSubsystem::load_delta_copy(const FaceChunkKey& key) const {
    std::scoped_lock lock(manager_.chunk_delta_mutex());
    auto it = manager_.chunk_deltas().find(key);
    if (it != manager_.chunk_deltas().end()) {
        return it->second;
    }
    return ChunkDelta{};
}

} // namespace wf
