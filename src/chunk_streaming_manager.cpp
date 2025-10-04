#include "chunk_streaming_manager.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace wf {

namespace {
constexpr float kDeltaPromoteDensity = 0.18f;
constexpr float kDeltaDemoteDensity = 0.08f;
}

ChunkStreamingManager::ChunkStreamingManager() = default;

void ChunkStreamingManager::set_worker_count(std::size_t count) {
    worker_count_hint_ = count;
}

void ChunkStreamingManager::set_load_job(LoadJob job) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    load_job_ = std::move(job);
}

void ChunkStreamingManager::start() {
    stop_flag_.store(false, std::memory_order_relaxed);
    std::size_t resolved = worker_count_hint_;
    if (resolved == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        resolved = hw == 0 ? 1u : static_cast<std::size_t>(hw);
    }
    worker_pool_.start(resolved);
    save_pool_.start(1);
    save_pool_started_.store(true, std::memory_order_relaxed);
}

void ChunkStreamingManager::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    worker_pool_.stop();
    wait_for_pending_saves();
    save_pool_.stop();
    save_pool_started_.store(false, std::memory_order_relaxed);
}

uint64_t ChunkStreamingManager::enqueue_request(LoadRequest req) {
    uint64_t gen = request_gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    req.gen = gen;

    LoadJob job_copy;
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        job_copy = load_job_;
    }

    if (job_copy) {
        worker_pool_.submit([job_copy, req]() { job_copy(req); }, true);
    }

    return gen;
}

bool ChunkStreamingManager::try_pop_result(MeshResult& out) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    if (results_queue_.empty()) return false;
    out = std::move(results_queue_.front());
    results_queue_.pop_front();
    return true;
}

void ChunkStreamingManager::push_mesh_result(MeshResult res) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    results_queue_.push_back(std::move(res));
}

size_t ChunkStreamingManager::result_queue_depth() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return results_queue_.size();
}

bool ChunkStreamingManager::loader_busy() const {
    return worker_pool_.busy();
}

bool ChunkStreamingManager::loader_idle() const {
    return worker_pool_.idle();
}

uint64_t ChunkStreamingManager::current_request_gen() const {
    return request_gen_.load(std::memory_order_relaxed);
}

bool ChunkStreamingManager::should_abort(uint64_t job_gen) const {
    return stop_flag_.load(std::memory_order_relaxed) || current_request_gen() != job_gen;
}

void ChunkStreamingManager::update_generation_stats(double gen_ms, int chunks) {
    loader_last_gen_ms_.store(gen_ms, std::memory_order_relaxed);
    loader_last_chunks_.store(chunks, std::memory_order_relaxed);
}

void ChunkStreamingManager::update_mesh_stats(double mesh_ms, int meshed, double total_ms) {
    loader_last_mesh_ms_.store(mesh_ms, std::memory_order_relaxed);
    loader_last_meshed_.store(meshed, std::memory_order_relaxed);
    loader_last_total_ms_.store(total_ms, std::memory_order_relaxed);
}

double ChunkStreamingManager::last_generation_ms() const {
    return loader_last_gen_ms_.load(std::memory_order_relaxed);
}

int ChunkStreamingManager::last_generated_chunks() const {
    return loader_last_chunks_.load(std::memory_order_relaxed);
}

double ChunkStreamingManager::last_mesh_ms() const {
    return loader_last_mesh_ms_.load(std::memory_order_relaxed);
}

int ChunkStreamingManager::last_meshed_chunks() const {
    return loader_last_meshed_.load(std::memory_order_relaxed);
}

double ChunkStreamingManager::last_total_ms() const {
    return loader_last_total_ms_.load(std::memory_order_relaxed);
}

void ChunkStreamingManager::set_planet_config(const PlanetConfig& cfg) {
    planet_cfg_ = cfg;
}

void ChunkStreamingManager::set_region_root(std::string root) {
    region_root_ = std::move(root);
}

void ChunkStreamingManager::set_save_chunks_enabled(bool enabled) {
    save_chunks_enabled_ = enabled;
}

std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash>& ChunkStreamingManager::chunk_cache() {
    return chunk_cache_;
}

const std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash>& ChunkStreamingManager::chunk_cache() const {
    return chunk_cache_;
}

void ChunkStreamingManager::store_chunk(const FaceChunkKey& key, const Chunk64& chunk) {
    std::lock_guard<std::mutex> lock(chunk_cache_mutex_);
    chunk_cache_[key] = chunk;
}

std::optional<Chunk64> ChunkStreamingManager::find_chunk(const FaceChunkKey& key) const {
    std::lock_guard<std::mutex> lock(chunk_cache_mutex_);
    auto it = chunk_cache_.find(key);
    if (it == chunk_cache_.end()) return std::nullopt;
    return it->second;
}

void ChunkStreamingManager::erase_chunk(const FaceChunkKey& key) {
    std::lock_guard<std::mutex> lock(chunk_cache_mutex_);
    chunk_cache_.erase(key);
}

std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash>& ChunkStreamingManager::chunk_deltas() {
    return chunk_deltas_;
}

const std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash>& ChunkStreamingManager::chunk_deltas() const {
    return chunk_deltas_;
}

std::mutex& ChunkStreamingManager::chunk_delta_mutex() const {
    return chunk_delta_mutex_;
}

std::deque<FaceChunkKey>& ChunkStreamingManager::remesh_queue() {
    return remesh_queue_;
}

std::mutex& ChunkStreamingManager::remesh_mutex() const {
    return remesh_mutex_;
}

void ChunkStreamingManager::normalize_chunk_delta_representation(ChunkDelta& delta) {
    if (delta.empty()) {
        if (delta.mode != ChunkDelta::Mode::kSparse) delta.clear(ChunkDelta::Mode::kSparse);
        return;
    }

    float density = delta.edit_density();
    if (delta.mode == ChunkDelta::Mode::kSparse) {
        if (density >= kDeltaPromoteDensity) {
            delta.ensure_dense();
        }
    } else {
        if (density <= kDeltaDemoteDensity) {
            delta.ensure_sparse();
        }
    }
}

void ChunkStreamingManager::overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk) {
    {
        std::scoped_lock lock(chunk_delta_mutex_);
        auto it = chunk_deltas_.find(key);
        if (it != chunk_deltas_.end()) {
            normalize_chunk_delta_representation(it->second);
            if (!it->second.empty()) apply_chunk_delta(it->second, chunk);
            return;
        }
    }

    ChunkDelta delta;
    if (!RegionIO::load_chunk_delta(key, delta, 32, region_root_)) {
        std::scoped_lock lock(chunk_delta_mutex_);
        chunk_deltas_.emplace(key, ChunkDelta{});
        return;
    }

    normalize_chunk_delta_representation(delta);
    if (!delta.empty()) apply_chunk_delta(delta, chunk);
    {
        std::scoped_lock lock(chunk_delta_mutex_);
        chunk_deltas_[key] = std::move(delta);
    }
}

void ChunkStreamingManager::flush_dirty_chunk_deltas() {
    if (!save_chunks_enabled_) return;

    std::vector<std::pair<FaceChunkKey, ChunkDelta>> pending;
    {
        std::scoped_lock lock(chunk_delta_mutex_);
        for (auto& kv : chunk_deltas_) {
            ChunkDelta& delta = kv.second;
            if (!delta.dirty) continue;
            pending.emplace_back(kv.first, delta);
            delta.dirty = false;
            if (!delta.dirty_mask.empty()) {
                std::fill(delta.dirty_mask.begin(), delta.dirty_mask.end(), 0ull);
            }
        }
    }

    if (pending.empty()) return;

    if (!save_pool_started_.load(std::memory_order_relaxed)) {
        for (auto& pair : pending) {
            FaceChunkKey key = pair.first;
            ChunkDelta& delta = pair.second;
            normalize_chunk_delta_representation(delta);
            RegionIO::save_chunk_delta(key, delta, 32, region_root_);
        }
        return;
    }

    auto batch = std::make_shared<std::vector<std::pair<FaceChunkKey, ChunkDelta>>>(std::move(pending));
    save_pool_.submit([this, batch]() {
        for (auto& pair : *batch) {
            FaceChunkKey key = pair.first;
            ChunkDelta& delta = pair.second;
            normalize_chunk_delta_representation(delta);
            RegionIO::save_chunk_delta(key, delta, 32, region_root_);
        }
    }, false);
}

void ChunkStreamingManager::wait_for_pending_saves() {
    using namespace std::chrono_literals;
    while (save_pool_started_.load(std::memory_order_relaxed) && !save_pool_.idle()) {
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace wf
