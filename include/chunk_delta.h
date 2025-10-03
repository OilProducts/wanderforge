#pragma once

#include <cstdint>
#include <vector>

namespace wf {

struct Chunk64;

struct ChunkDeltaEntry {
    uint32_t index = 0;   // linear voxel index within Chunk64
    uint16_t material = 0;
};

struct ChunkDelta {
    enum class Mode : uint8_t {
        kSparse = 0,
        kDense  = 1,
    };

    static constexpr uint16_t kNoOverride = 0xFFFFu;

    Mode mode = Mode::kSparse;
    std::vector<ChunkDeltaEntry> entries;        // Sparse representation (default)
    std::vector<uint16_t>         dense_data;    // Dense representation (Chunk64::N3 elements)
    std::vector<uint64_t>         dirty_mask;    // Bitset tracking touched voxels (shared across modes)
    bool                          dirty = false; // Set when runtime edits require persistence
    uint32_t                      override_count = 0; // number of active overrides

    bool empty() const { return override_count == 0; }

    void clear(Mode new_mode = Mode::kSparse) {
        mode = new_mode;
        entries.clear();
        dense_data.clear();
        dirty_mask.clear();
        dirty = false;
        override_count = 0;
    }

    size_t edit_count() const {
        return (size_t)override_count;
    }

    float edit_density() const;
    void ensure_dense();
    void ensure_sparse();
    void mark_dirty(uint32_t index);
    bool test_dirty(uint32_t index) const;
    void apply_edit(uint32_t index, uint16_t base_material, uint16_t new_material);
};

void apply_chunk_delta(const ChunkDelta& delta, Chunk64& chunk);

} // namespace wf
