#include "chunk_delta.h"
#include "chunk.h"

#include <bit>
#include <algorithm>
#include <limits>

namespace wf {

namespace {
constexpr size_t kDeltaWordCount = (Chunk64::N3 + 63) / 64;
} // namespace

float ChunkDelta::edit_density() const {
    constexpr float inv_total = 1.0f / float(Chunk64::N3);
    return override_count == 0 ? 0.0f : std::min(1.0f, float(override_count) * inv_total);
}

void ChunkDelta::ensure_dense() {
    if (mode == Mode::kDense) return;
    dense_data.assign(Chunk64::N3, kNoOverride);
    override_count = static_cast<uint32_t>(entries.size());
    for (const auto& e : entries) {
        if (e.index < dense_data.size()) dense_data[e.index] = e.material;
    }
    entries.clear();
    mode = Mode::kDense;
}

void ChunkDelta::ensure_sparse() {
    if (mode == Mode::kSparse) return;
    entries.clear();
    entries.reserve(dense_data.size());
    override_count = 0;
    for (uint32_t i = 0; i < dense_data.size(); ++i) {
        uint16_t mat = dense_data[i];
        if (mat != kNoOverride) {
            entries.push_back(ChunkDeltaEntry{ i, mat });
            ++override_count;
        }
    }
    dense_data.clear();
    mode = Mode::kSparse;
}

void ChunkDelta::mark_dirty(uint32_t index) {
    const size_t required = kDeltaWordCount;
    if (dirty_mask.size() < required) dirty_mask.assign(required, 0ull);
    const size_t w = index >> 6;
    const uint64_t bit = 1ull << (index & 63);
    if ((dirty_mask[w] & bit) == 0ull) {
        dirty_mask[w] |= bit;
        dirty = true;
    }
}

bool ChunkDelta::test_dirty(uint32_t index) const {
    if (dirty_mask.empty()) return false;
    const size_t w = index >> 6;
    if (w >= dirty_mask.size()) return false;
    const uint64_t bit = 1ull << (index & 63);
    return (dirty_mask[w] & bit) != 0ull;
}

void ChunkDelta::apply_edit(uint32_t index, uint16_t base_material, uint16_t new_material) {
    if (index >= Chunk64::N3) return;

    bool changed = false;

    if (mode == Mode::kDense) {
        if (dense_data.empty()) dense_data.assign(Chunk64::N3, kNoOverride);
        uint16_t& slot = dense_data[index];
        uint16_t prev = slot;
        if (new_material == base_material) {
            if (prev != kNoOverride) {
                slot = kNoOverride;
                if (override_count > 0) --override_count;
                changed = true;
            }
        } else {
            if (prev == kNoOverride) {
                ++override_count;
            } else if (prev == new_material) {
                // No-op
            }
            if (slot != new_material) {
                slot = new_material;
                changed = true;
            }
        }
    } else {
        auto it = std::find_if(entries.begin(), entries.end(), [&](const ChunkDeltaEntry& e){ return e.index == index; });
        if (new_material == base_material) {
            if (it != entries.end()) {
                *it = entries.back();
                entries.pop_back();
                if (override_count > 0) --override_count;
                changed = true;
            }
        } else {
            if (it != entries.end()) {
                if (it->material != new_material) {
                    it->material = new_material;
                    changed = true;
                }
            } else {
                entries.push_back(ChunkDeltaEntry{ index, new_material });
                ++override_count;
                changed = true;
            }
        }
    }

    if (changed) mark_dirty(index);
}

void apply_chunk_delta(const ChunkDelta& delta, Chunk64& chunk) {
    if (delta.empty()) return;
    const int N = Chunk64::N;
    const int N2 = N * N;

    if (delta.mode == ChunkDelta::Mode::kDense) {
        if (delta.dense_data.size() != Chunk64::N3) return;
        for (int z = 0; z < N; ++z) {
            for (int y = 0; y < N; ++y) {
                for (int x = 0; x < N; ++x) {
                    uint32_t idx = Chunk64::lindex(x, y, z);
                    uint16_t mat = delta.dense_data[idx];
                    if (mat == ChunkDelta::kNoOverride) continue;
                    chunk.set_voxel(x, y, z, mat);
                }
            }
        }
        return;
    }

    for (const ChunkDeltaEntry& entry : delta.entries) {
        uint32_t i = entry.index;
        int x = (int)(i % N);
        int y = (int)((i / N) % N);
        int z = (int)(i / N2);
        chunk.set_voxel(x, y, z, entry.material);
    }
}

} // namespace wf
