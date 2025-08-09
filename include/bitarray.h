#pragma once

#include <cstdint>
#include <vector>

namespace wf {

// Simple packed bit array for small palettes (1/2/4/8 bpp)
class BitArray {
public:
    BitArray() : bits_per_(8), size_(0) {}
    BitArray(uint32_t n, uint32_t bpp) { reset(n, bpp); }

    void reset(uint32_t n, uint32_t bpp) {
        bits_per_ = bpp; size_ = n;
        data_.assign((size_ * bits_per_ + 63) / 64, 0ull);
    }

    uint32_t size() const { return size_; }
    uint32_t bpp() const { return bits_per_; }

    void set(uint32_t i, uint32_t v) {
        const uint64_t bit = uint64_t(i) * bits_per_;
        const uint32_t w = uint32_t(bit >> 6);
        const uint32_t off = uint32_t(bit & 63);
        const uint64_t mask = ((bits_per_ == 64) ? ~0ull : ((1ull << bits_per_) - 1ull)) << off;
        data_[w] = (data_[w] & ~mask) | (uint64_t(v) << off);
        const uint32_t spill = (off + bits_per_) > 64;
        if (spill) {
            const uint32_t w2 = w + 1;
            const uint32_t r = 64 - off;
            const uint64_t mask2 = (1ull << (bits_per_ - r)) - 1ull;
            data_[w2] = (data_[w2] & ~mask2) | (uint64_t(v) >> r);
        }
    }

    uint32_t get(uint32_t i) const {
        const uint64_t bit = uint64_t(i) * bits_per_;
        const uint32_t w = uint32_t(bit >> 6);
        const uint32_t off = uint32_t(bit & 63);
        uint64_t val = data_[w] >> off;
        if ((off + bits_per_) > 64) {
            val |= data_[w + 1] << (64 - off);
        }
        return uint32_t(val & ((bits_per_ == 64) ? ~0ull : ((1ull << bits_per_) - 1ull)));
    }

private:
    uint32_t bits_per_ = 8;
    uint32_t size_ = 0;
    std::vector<uint64_t> data_;
};

} // namespace wf

