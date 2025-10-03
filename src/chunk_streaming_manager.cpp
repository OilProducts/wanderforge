#include "chunk_streaming_manager.h"

#include <algorithm>
#include <utility>

namespace wf {

namespace {
constexpr float kDeltaPromoteDensity = 0.18f;
constexpr float kDeltaDemoteDensity = 0.08f;
}

ChunkStreamingManager::ChunkStreamingManager() = default;

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

std::mutex& ChunkStreamingManager::chunk_cache_mutex() const {
    return chunk_cache_mutex_;
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

    for (auto& pair : pending) {
        FaceChunkKey key = pair.first;
        ChunkDelta& delta = pair.second;
        normalize_chunk_delta_representation(delta);
        RegionIO::save_chunk_delta(key, delta, 32, region_root_);
    }
}

} // namespace wf
