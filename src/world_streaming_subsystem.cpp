#include "world_streaming_subsystem.h"

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

} // namespace wf

