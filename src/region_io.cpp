#include "region_io.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace wf {

static constexpr uint32_t kRegionFlag_Delta = 1u << 0;

static inline uint32_t fnv1a32(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// floorDiv for negatives
static inline std::int64_t floordiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    std::int64_t r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) --q;
    return q;
}

void RegionIO::region_coords(const FaceChunkKey& key, int tile, std::int64_t& i0, std::int64_t& j0, int& ti, int& tj) {
    i0 = floordiv(key.i, tile) * (std::int64_t)tile;
    j0 = floordiv(key.j, tile) * (std::int64_t)tile;
    ti = int(key.i - i0);
    tj = int(key.j - j0);
}

std::string RegionIO::region_path(const FaceChunkKey& key, int tile, const std::string& root) {
    std::int64_t i0, j0; int ti, tj; (void)ti; (void)tj;
    region_coords(key, tile, i0, j0, ti, tj);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "r_%lld_%lld.wfr", (long long)i0, (long long)j0);
    fs::path p = fs::path(root) / (std::string("face") + std::to_string(key.face)) / (std::string("k") + std::to_string((long long)key.k)) / buf;
    return p.string();
}

static bool read_all(FILE* f, void* dst, size_t n) {
    return std::fread(dst, 1, n, f) == n;
}

static bool write_all(FILE* f, const void* src, size_t n) {
    return std::fwrite(src, 1, n, f) == n;
}

static bool load_region_header(FILE* f, RegionHeaderV1& hdr) {
    if (!read_all(f, &hdr, sizeof(hdr))) return false;
    if (std::strncmp(hdr.magic, "WFREGN1", 7) != 0 || hdr.version != 1) return false;
    if (hdr.toc_entries != (uint32_t)(hdr.tile * hdr.tile)) return false;
    return true;
}

static void init_region_header(RegionHeaderV1& hdr, const FaceChunkKey& key, int tile) {
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "WFREGN1", 7);
    hdr.version = 1;
    hdr.face = key.face;
    std::int64_t i0, j0; int ti, tj; RegionIO::region_coords(key, tile, i0, j0, ti, tj);
    hdr.i0 = i0; hdr.j0 = j0; hdr.k = key.k;
    hdr.tile = tile; hdr.chunk_vox = wf::Chunk64::N;
    hdr.flags = 0;
    hdr.toc_entries = (uint32_t)(tile * tile);
    hdr.toc_offset = sizeof(RegionHeaderV1);
    hdr.data_offset = hdr.toc_offset + sizeof(RegionTocEntryV1) * hdr.toc_entries;
}

static FILE* open_region_rw(const std::string& path) {
    // Try open existing
    FILE* f = std::fopen(path.c_str(), "rb+");
    if (f) return f;
    // Create new
    f = std::fopen(path.c_str(), "wb+");
    return f;
}

static bool ensure_dirs_for(const std::string& path) {
    fs::path p(path);
    fs::path d = p.parent_path();
    std::error_code ec; fs::create_directories(d, ec);
    return !ec;
}

bool RegionIO::save_chunk(const FaceChunkKey& key, const Chunk64& c, int tile, const std::string& root) {
    std::string path = region_path(key, tile, root);
    if (!ensure_dirs_for(path)) return false;
    FILE* f = open_region_rw(path);
    if (!f) return false;

    RegionHeaderV1 hdr{}; bool exists = false;
    std::vector<RegionTocEntryV1> toc;
    {
        // Read existing header if present, else initialize new
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        if (sz >= (long)sizeof(RegionHeaderV1)) {
            std::fseek(f, 0, SEEK_SET);
            if (load_region_header(f, hdr)) {
                exists = true;
                toc.resize(hdr.toc_entries);
                std::fseek(f, (long)hdr.toc_offset, SEEK_SET);
                if (!read_all(f, toc.data(), sizeof(RegionTocEntryV1) * toc.size())) { std::fclose(f); return false; }
            }
        }
        if (!exists) {
            init_region_header(hdr, key, tile);
            toc.assign(hdr.toc_entries, RegionTocEntryV1{});
            std::fseek(f, 0, SEEK_SET);
            if (!write_all(f, &hdr, sizeof(hdr))) { std::fclose(f); return false; }
            if (!write_all(f, toc.data(), sizeof(RegionTocEntryV1) * toc.size())) { std::fclose(f); return false; }
        }
    }

    // Prepare chunk blob (raw)
    const int N = Chunk64::N;
    const size_t N3 = size_t(N) * N * N;
    ChunkBlobHeaderV1 ch{};
    std::memset(&ch, 0, sizeof(ch));
    std::memcpy(ch.magic, "WFCHK1", 6);
    ch.version = 1;
    ch.palette_count = (uint16_t)c.palette.size();
    ch.bpp = 8;
    ch.indices_bytes = (uint32_t)N3; // 8-bit per index
    ch.occ_words = (uint32_t)((N3 + 63) / 64);

    std::vector<uint8_t> blob;
    blob.reserve(sizeof(ChunkBlobHeaderV1) + ch.palette_count * 2 + ch.indices_bytes + ch.occ_words * 8);
    // Header
    blob.insert(blob.end(), reinterpret_cast<uint8_t*>(&ch), reinterpret_cast<uint8_t*>(&ch) + sizeof(ch));
    // Palette
    if (ch.palette_count) {
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(c.palette.data()), reinterpret_cast<const uint8_t*>(c.palette.data()) + ch.palette_count * sizeof(uint16_t));
    }
    // Indices (8-bit each)
    size_t start_idx = blob.size();
    blob.resize(blob.size() + ch.indices_bytes);
    for (size_t i = 0; i < N3; ++i) {
        blob[start_idx + i] = (uint8_t)c.indices.get((uint32_t)i);
    }
    // Occupancy words
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(c.occ.data()), reinterpret_cast<const uint8_t*>(c.occ.data()) + ch.occ_words * sizeof(uint64_t));

    uint32_t checksum = fnv1a32(blob.data(), blob.size());

    // Determine TOC slot
    std::int64_t i0, j0; int ti, tj; region_coords(key, tile, i0, j0, ti, tj);
    const size_t idx = (size_t)(tj * tile + ti);

    // Append blob to end and update TOC entry
    std::fseek(f, 0, SEEK_END);
    std::uint64_t off = (std::uint64_t)std::ftell(f);
    if (!write_all(f, blob.data(), blob.size())) { std::fclose(f); return false; }

    RegionTocEntryV1 ent{};
    ent.offset = off; ent.size = (uint32_t)blob.size(); ent.usize = ent.size; ent.flags = 0; ent.checksum = checksum;
    toc[idx] = ent;

    // Write TOC entry back
    std::fseek(f, (long)(hdr.toc_offset + idx * sizeof(RegionTocEntryV1)), SEEK_SET);
    if (!write_all(f, &ent, sizeof(ent))) { std::fclose(f); return false; }

    std::fclose(f);
    return true;
}

bool RegionIO::load_chunk(const FaceChunkKey& key, Chunk64& out, int tile, const std::string& root) {
    std::string path = region_path(key, tile, root);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    RegionHeaderV1 hdr{};
    if (!load_region_header(f, hdr)) { std::fclose(f); return false; }
    std::int64_t i0, j0; int ti, tj; region_coords(key, tile, i0, j0, ti, tj);
    if (hdr.face != key.face || hdr.k != key.k || hdr.i0 != i0 || hdr.j0 != j0) { std::fclose(f); return false; }
    const size_t idx = (size_t)(tj * tile + ti);
    std::fseek(f, (long)(hdr.toc_offset + idx * sizeof(RegionTocEntryV1)), SEEK_SET);
    RegionTocEntryV1 ent{};
    if (!read_all(f, &ent, sizeof(ent))) { std::fclose(f); return false; }
    if (ent.offset == 0 || ent.size == 0) { std::fclose(f); return false; }

    std::vector<uint8_t> blob(ent.size);
    std::fseek(f, (long)ent.offset, SEEK_SET);
    if (!read_all(f, blob.data(), blob.size())) { std::fclose(f); return false; }
    std::fclose(f);

    if (fnv1a32(blob.data(), blob.size()) != ent.checksum) return false;

    if (blob.size() < sizeof(ChunkBlobHeaderV1)) return false;
    const ChunkBlobHeaderV1* ch = reinterpret_cast<const ChunkBlobHeaderV1*>(blob.data());
    if (std::strncmp(ch->magic, "WFCHK1", 6) != 0 || ch->version != 1) return false;
    const uint8_t* p = blob.data() + sizeof(ChunkBlobHeaderV1);
    const uint8_t* end = blob.data() + blob.size();

    // Load palette
    out.palette.clear(); out.palette_lut.clear();
    out.palette.reserve(ch->palette_count);
    if (p + ch->palette_count * sizeof(uint16_t) > end) return false;
    const uint16_t* pal = reinterpret_cast<const uint16_t*>(p);
    for (uint32_t i = 0; i < ch->palette_count; ++i) {
        out.palette.push_back(pal[i]);
        out.palette_lut.emplace(pal[i], (uint16_t)i);
    }
    p += ch->palette_count * sizeof(uint16_t);

    // Indices (expect N^3 bytes for bpp==8)
    const int N = Chunk64::N; const size_t N3 = size_t(N) * N * N;
    if (ch->bpp != 8 || ch->indices_bytes != (uint32_t)N3) return false;
    if (p + N3 > end) return false;
    out.indices.reset((uint32_t)N3, 8);
    for (size_t i = 0; i < N3; ++i) out.indices.set((uint32_t)i, p[i]);
    p += N3;

    // Occupancy
    size_t occ_bytes = (size_t)ch->occ_words * sizeof(uint64_t);
    if (p + occ_bytes > end) return false;
    std::memcpy(out.occ.data(), p, std::min(occ_bytes, sizeof(out.occ)));
    p += occ_bytes;

    out.dirty_mesh = true;
    return true;
}

bool RegionIO::save_chunk_delta(const FaceChunkKey& key, const ChunkDelta& delta, int tile, const std::string& root) {
    std::string path = region_path(key, tile, root);
    if (!ensure_dirs_for(path)) return false;
    FILE* f = open_region_rw(path);
    if (!f) return false;

    RegionHeaderV1 hdr{}; bool exists = false;
    std::vector<RegionTocEntryV1> toc;
    {
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        if (sz >= (long)sizeof(RegionHeaderV1)) {
            std::fseek(f, 0, SEEK_SET);
            if (load_region_header(f, hdr)) {
                exists = true;
                toc.resize(hdr.toc_entries);
                std::fseek(f, (long)hdr.toc_offset, SEEK_SET);
                if (!read_all(f, toc.data(), sizeof(RegionTocEntryV1) * toc.size())) {
                    std::fclose(f);
                    return false;
                }
            }
        }
        if (!exists) {
            init_region_header(hdr, key, tile);
            toc.assign(hdr.toc_entries, RegionTocEntryV1{});
            std::fseek(f, 0, SEEK_SET);
            if (!write_all(f, &hdr, sizeof(hdr))) { std::fclose(f); return false; }
            if (!write_all(f, toc.data(), sizeof(RegionTocEntryV1) * toc.size())) { std::fclose(f); return false; }
        }
    }

    std::int64_t i0, j0; int ti, tj; region_coords(key, tile, i0, j0, ti, tj);
    const size_t idx = (size_t)(tj * tile + ti);

    if (delta.empty()) {
        RegionTocEntryV1 ent{};
        toc[idx] = ent;
        std::fseek(f, (long)(hdr.toc_offset + idx * sizeof(RegionTocEntryV1)), SEEK_SET);
        bool ok = write_all(f, &ent, sizeof(ent));
        std::fclose(f);
        return ok;
    }

    ChunkDeltaHeaderV1 dh{};
    std::memset(&dh, 0, sizeof(dh));
    std::memcpy(dh.magic, "WFDEL1", 6);
    dh.version = 1;
    dh.entry_count = (delta.mode == ChunkDelta::Mode::kDense)
        ? (uint32_t)delta.dense_data.size()
        : (uint32_t)delta.entries.size();
    dh.reserved = static_cast<uint32_t>(delta.mode);

    struct PackedDeltaEntry {
        uint32_t index;
        uint16_t material;
        uint16_t pad;
    };

    std::vector<uint8_t> blob;
    if (delta.mode == ChunkDelta::Mode::kDense) {
        const size_t payload_bytes = (size_t)dh.entry_count * sizeof(uint16_t);
        blob.reserve(sizeof(dh) + payload_bytes);
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&dh), reinterpret_cast<const uint8_t*>(&dh) + sizeof(dh));
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(delta.dense_data.data());
        blob.insert(blob.end(), ptr, ptr + payload_bytes);
    } else {
        blob.reserve(sizeof(dh) + delta.entries.size() * sizeof(PackedDeltaEntry));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&dh), reinterpret_cast<const uint8_t*>(&dh) + sizeof(dh));
        for (const ChunkDeltaEntry& e : delta.entries) {
            PackedDeltaEntry rec{ e.index, e.material, 0u };
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&rec);
            blob.insert(blob.end(), ptr, ptr + sizeof(rec));
        }
    }

    uint32_t checksum = fnv1a32(blob.data(), blob.size());

    std::fseek(f, 0, SEEK_END);
    std::uint64_t off = (std::uint64_t)std::ftell(f);
    if (!write_all(f, blob.data(), blob.size())) { std::fclose(f); return false; }

    RegionTocEntryV1 ent{};
    ent.offset = off;
    ent.size = (uint32_t)blob.size();
    ent.usize = ent.size;
    ent.flags = kRegionFlag_Delta;
    ent.checksum = checksum;
    toc[idx] = ent;

    std::fseek(f, (long)(hdr.toc_offset + idx * sizeof(RegionTocEntryV1)), SEEK_SET);
    bool ok = write_all(f, &ent, sizeof(ent));
    std::fclose(f);
    return ok;
}

bool RegionIO::load_chunk_delta(const FaceChunkKey& key, ChunkDelta& out, int tile, const std::string& root) {
    std::string path = region_path(key, tile, root);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { out.clear(); return false; }

    RegionHeaderV1 hdr{};
    if (!load_region_header(f, hdr)) { std::fclose(f); out.clear(); return false; }
    std::int64_t i0, j0; int ti, tj; region_coords(key, tile, i0, j0, ti, tj);
    if (hdr.face != key.face || hdr.k != key.k || hdr.i0 != i0 || hdr.j0 != j0) { std::fclose(f); out.clear(); return false; }

    const size_t idx = (size_t)(tj * tile + ti);
    std::fseek(f, (long)(hdr.toc_offset + idx * sizeof(RegionTocEntryV1)), SEEK_SET);
    RegionTocEntryV1 ent{};
    if (!read_all(f, &ent, sizeof(ent))) { std::fclose(f); out.clear(); return false; }
    if (ent.offset == 0 || ent.size == 0 || (ent.flags & kRegionFlag_Delta) == 0) { std::fclose(f); out.clear(); return false; }

    std::vector<uint8_t> blob(ent.size);
    std::fseek(f, (long)ent.offset, SEEK_SET);
    if (!read_all(f, blob.data(), blob.size())) { std::fclose(f); out.clear(); return false; }
    std::fclose(f);

    if (fnv1a32(blob.data(), blob.size()) != ent.checksum) { out.clear(); return false; }
    if (blob.size() < sizeof(ChunkDeltaHeaderV1)) { out.clear(); return false; }

    const ChunkDeltaHeaderV1* dh = reinterpret_cast<const ChunkDeltaHeaderV1*>(blob.data());
    if (std::strncmp(dh->magic, "WFDEL1", 6) != 0 || dh->version != 1) { out.clear(); return false; }
    size_t count = dh->entry_count;
    const uint8_t* p = blob.data() + sizeof(ChunkDeltaHeaderV1);
    const uint8_t* end = blob.data() + blob.size();

    ChunkDelta::Mode mode = (dh->reserved == static_cast<uint32_t>(ChunkDelta::Mode::kDense))
        ? ChunkDelta::Mode::kDense
        : ChunkDelta::Mode::kSparse;

    out.clear(mode);

    if (mode == ChunkDelta::Mode::kDense) {
        size_t bytes = count * sizeof(uint16_t);
        if (p + bytes > end) { out.clear(); return false; }
        out.dense_data.resize(count);
        std::memcpy(out.dense_data.data(), p, bytes);
        out.override_count = 0;
        for (uint16_t mat : out.dense_data) {
            if (mat != ChunkDelta::kNoOverride) ++out.override_count;
        }
    } else {
        struct PackedDeltaEntry { uint32_t index; uint16_t material; uint16_t pad; };
        size_t bytes = count * sizeof(PackedDeltaEntry);
        if (p + bytes > end) { out.clear(); return false; }
        const PackedDeltaEntry* recs = reinterpret_cast<const PackedDeltaEntry*>(p);
        out.entries.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            ChunkDeltaEntry e{};
            e.index = recs[i].index;
            e.material = recs[i].material;
            out.entries.push_back(e);
        }
        out.override_count = static_cast<uint32_t>(out.entries.size());
    }
    return true;
}

} // namespace wf
