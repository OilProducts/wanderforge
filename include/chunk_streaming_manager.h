#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
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
    PlanetConfig planet_cfg_{};
    bool save_chunks_enabled_ = false;
    bool log_stream_ = false;
    std::string region_root_ = "regions";
    std::size_t remesh_per_frame_cap_ = 4;

    std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash> chunk_deltas_;
    mutable std::mutex chunk_delta_mutex_;

    std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash> chunk_cache_;
    mutable std::mutex chunk_cache_mutex_;

    std::deque<FaceChunkKey> remesh_queue_;
    mutable std::mutex remesh_mutex_;
};

} // namespace wf
