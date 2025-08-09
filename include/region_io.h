// Minimal region IO scaffolding: 32x32 face-local tiles per file, per-k shell.
// Format is versioned with a simple header + TOC + uncompressed chunk blobs.

#pragma once

#include <cstdint>
#include <string>
#include "planet.h"
#include "chunk.h"

namespace wf {

struct RegionHeaderV1 {
    char     magic[8];      // "WFREGN1\0"
    uint32_t version;       // 1
    int32_t  face;          // 0..5
    std::int64_t i0;        // tile origin i (multiple of tile)
    std::int64_t j0;        // tile origin j (multiple of tile)
    std::int64_t k;         // radial shell index
    int32_t  tile;          // tiles per side (default 32)
    int32_t  chunk_vox;     // chunk dimension (64)
    uint32_t flags;         // reserved
    uint32_t toc_entries;   // tile*tile
    std::uint64_t toc_offset;  // absolute file offset of TOC
    std::uint64_t data_offset; // absolute file offset where blobs can start
};

struct RegionTocEntryV1 {
    std::uint64_t offset;   // absolute file offset of chunk blob (0 = empty)
    std::uint32_t size;     // blob size in bytes (compressed or raw)
    std::uint32_t usize;    // uncompressed size (==size for raw)
    std::uint32_t flags;    // 0 = raw; future: 1 = zstd, 2 = lz4
    std::uint32_t checksum; // FNV-1a 32 of blob payload
};

// Simple chunk blob (raw/uncompressed) for V1
struct ChunkBlobHeaderV1 {
    char     magic[8];      // "WFCHK1\0"
    uint32_t version;       // 1
    uint16_t palette_count; // number of materials in palette
    uint8_t  bpp;           // palette index bits (currently 8)
    uint8_t  reserved;      // pad
    std::uint32_t indices_bytes; // N^3 bytes (bpp==8); future: packed
    std::uint32_t occ_words;     // number of 64-bit words in occupancy
};

class RegionIO {
public:
    // Compute region file path for a given face chunk key.
    // Layout: regions/face{f}/k{k}/r_{i0}_{j0}.wfr
    static std::string region_path(const FaceChunkKey& key, int tile = 32, const std::string& root = "regions");

    // Save/load a chunk to/from its region file. Returns true on success.
    static bool save_chunk(const FaceChunkKey& key, const Chunk64& c, int tile = 32, const std::string& root = "regions");
    static bool load_chunk(const FaceChunkKey& key, Chunk64& out, int tile = 32, const std::string& root = "regions");

    // Utility: convert chunk key to its local tile indices and region origin.
    static void region_coords(const FaceChunkKey& key, int tile, std::int64_t& i0, std::int64_t& j0, int& ti, int& tj);
};

} // namespace wf

