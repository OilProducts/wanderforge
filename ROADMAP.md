That plan sounds solid. Moving the **baseline to 10 cm** voxels for all common materials and reserving **5 cm / 2.5 cm** “micro‑tiers” for rare/fine simulations keeps your compute and memory under control—while leaving a clear expansion path.

Below is a detailed **project roadmap** for a C++/Vulkan implementation, sized to your “20‑minute circumnavigation” planet, followed by **minimal data structures** and a **toy compute kernel** (GLSL) for sand/water/lava.

---

## 0) Quick scale math (to size budgets)

Let `t = 20 min = 1,200 s`. Circumference `C = v * t`, radius `R = C / (2π)`. Equator voxels = `C / 0.1 m`.

| Run speed (m/s) | Circumference (m) | Radius (m) | Equator voxels (10 cm) | Equator chunks @ 64³ (6.4 m) |
| --------------: | ----------------: | ---------: | ---------------------: | ---------------------------: |
|               5 |             6,000 |      \~955 |                 60,000 |                        \~938 |
|               6 |             7,200 |    \~1,146 |                 72,000 |                      \~1,125 |
|               8 |             9,600 |    \~1,527 |                 96,000 |                      \~1,500 |

> Recommendation: assume **6 m/s** → **C ≈ 7.2 km**, **R ≈ 1.15 km**. You will **not** store the planet densely; you’ll stream/voxelize a **local shell** near the player and simulate **dense islands** only around activity.

---

## High‑level architecture (recap at 10 cm)

* **Base world:** procedural SDF/heightfield on a **cube‑sphere** with biomes/material IDs. Read‑only.
* **Delta store:** sparse edits (hash map keyed by chunk + local index). Authoritative for changes.
* **Chunks (visual/cache):** **64³ voxels** at 10 cm (**6.4 m** physical). Paletted + bitmasks. Meshed on demand.
* **Simulation islands:** dense 3D grids **12–16 m** per side (i.e., **120–160** cells at 10 cm → **1.7–4.1 M** cells), spawned where activity happens; optional nested **5 cm / 2.5 cm** refinement bubbles inside.

---

## Roadmap (phased, deliverables, budgets)

Each “phase” is \~1–3 weeks of focused work. Parallelize if you like, but the order minimizes rework.

### Phase 1 — Project skeleton & Vulkan plumbing (1–2 weeks)

**Deliverables**

* CMake skeleton, CI build.
* Vulkan instance/device selection, queues, debug layers.
* VMA (Vulkan Memory Allocator) integration, command pool/submit helpers.
* Shader toolchain (glslangValidator or DXC → SPIR‑V).
* Windowing (GLFW/SDL), swapchain & a simple full‑screen pass.

**Success criteria**

* Draw a test triangle and dispatch a trivial compute shader; hot‑reload shaders.

---

### Phase 2 — Math, planet frame & procedural base world (2 weeks)

**Deliverables**

* Math types (float3, int3, transforms); 64‑bit world coords.
* **Cube‑sphere mapping** utilities (face <-> direction vectors).
* Deterministic noise stack (FBM/Perlin/Simplex) for: elevation, caves, biome masks.
* **Base sampler**:

  ```cpp
  struct BaseSample { uint16_t material; float density; };
  BaseSample sample_base(Int3 voxel);
  ```

  Uses world‑to‑planet mapping; returns rock/soil/water/lava/air (no deltas yet).

**Success criteria**

* CPU tool to ray‑march planet surface; visualize a single ring of chunks around start.

---

### Phase 3 — Chunk store, palette & greedy meshing (2–3 weeks)

**Deliverables**

* **Chunk format**: 64³ voxels, split into **8×8×8 tiles** internally.

  * Per‑chunk: material palette (≤256), bitsets for air/solid/fluid, packed indices.
* **Region files** (Minecraft‑style): each region = **32×32 chunks** on a face‑local grid; TOC + compressed chunk blobs (zstd/LZ4).
* **Greedy mesher** (CPU first) → index/vertex buffers.
* **Renderer**: frustum culling, per‑material draw lists, triplanar texturing.

**Budget targets**

* Typical chunk after greedy meshing: **≤ 15k tris**.
* Mesh build: **< 1 ms** per dirty chunk on CPU (later move to compute).

**Success criteria**

* Walk around a static planet; streaming loads/unloads chunks around player smoothly at 60 fps.

---

### Phase 4 — Delta store & re‑meshing (1–2 weeks)

**Deliverables**

* Sparse **delta map**:

  ```cpp
  struct VoxelDelta { uint16_t mat; uint8_t temp; uint8_t flags; };
  // key = (chunk_key, local_index)
  ```
* Read path: `sample_world(voxel) = deltas.get(voxel).or_else(sample_base(voxel))`
* Write path: set/remove delta; mark affected chunk + neighbors dirty → re‑mesh.

**Success criteria**

* Tools to dig/place voxels; chunks re‑mesh locally; edits persist on save.

---

### Phase 5 — Simulation islands v1 (10 cm only) (2–3 weeks)

**Deliverables**

* **Island manager**: creates a dense box (e.g., **128³** or **160³**) centered on activity.
* **State layout per island** (GPU buffers):

  * Occupancy bitset (1 b/cell), material index (1 B), temperature (1 B), velocity/flags (1 B).
  * **Active list** buffers (ping‑pong), **move request** buffer.
* **Island<->world I/O**:

  * **Bake‑in**: sample base + deltas into island grids.
  * **Bake‑out**: write back only *changes* (deltas); enqueue chunk re‑mesh.

**Budget targets**

* Island memory (160³ \~ 4.1 M cells): **\~32–64 MB** plus staging.
* Transfer/bake each direction **< 2 ms** amortized (async DMA).

**Success criteria**

* Spawn an island when you pour material; after it calms (no active voxels for N ticks), it bakes out and disappears.

---

### Phase 6 — CA kernel v1: sand + water (2–3 weeks)

**Deliverables**

* Vulkan compute pipeline for **two‑phase** update:

  1. **Propose** moves per active cell (tile‑local in shared memory).
  2. **Commit** with atomic CAS on destination occupancy; losers re‑queue or stay.
* Rules:

  * **Sand**: try down → down‑left/right → slide if slope > threshold; sinks in water.
  * **Water**: down → lateral spread with limited outflow per tick.
* **Radial gravity**: `g = -normalize(p - center) * g0`.

**Budget targets**

* 1–2 M active cells/tick at 60 fps on your 4090 is realistic if memory‑coherent.

**Success criteria**

* Piles form with believable angle of repose; water fills and flows; sand displaces water.

---

### Phase 7 — Thermal + lava + fire (2 weeks)

**Deliverables**

* Temperature diffusion; per‑material thresholds: ignite, melt, solidify.
* **Lava**: viscous fluid + heat source; water contact → stone; wood → fire → ash.
* **Simple reaction table** (A,B → product, heatΔ, probability).

**Success criteria**

* Pour lava onto water → cooled stone on contact; wood near lava ignites and burns out.

---

### Phase 8 — Refinement bubbles (5 cm / 2.5 cm) (2–3 weeks)

**Deliverables**

* Per‑island **sub‑grid** refinement when local complexity high:

  * Allocate **2×** or **4×** finer sub‑box (e.g., 5 cm or 2.5 cm) for a small volume.
  * Map coarse cells into fine; simulate; periodically re‑aggregate down.
* Deterministic **mapping up/down** (majority material, volume‑weighted temp).

**Success criteria**

* Narrow flows (lava rivulets, fine grains) look better without exploding global cost.

---

### Phase 9 — Visual LOD & far‑field planet (1–2 weeks)

**Deliverables**

* **Far mesh**: low‑poly cube‑sphere with displacement from base elevation (no voxels).
* **Near/far transition**: blend radius; inside → chunked voxel meshes; outside → far mesh.
* **Chunk clipmap** for mid‑range if needed (optional).

**Success criteria**

* Stable 60 fps with a \~2–4 km view distance; minimal popping.

---

### Phase 10 — Save/Load, journaling, and robustness (1 week)

**Deliverables**

* Region index + chunk dirty bit tracking.
* **Delta journaling** (append log) → periodic compaction.
* Versioned world header & seed locking.

---

### Phase 11 — Tools & profiling (1 week initial, then ongoing)

**Deliverables**

* ImGui overlay: GPU timings, active cell counts, island list, memory usage.
* Heatmaps (active cells, temperature, slope).
* Determinism toggles (fixed RNG seed from cell coords + tick).

---

### Ongoing performance budgets (per frame)

* **CA compute**: 6–9 ms (depends on active cells).
* **Meshing (dirty chunks)**: 1–3 ms amortized (move to GPU compute later).
* **Rendering**: 4–6 ms (4090 with greedy meshes + triplanar is easy).
* **Streaming/IO**: async; keep under PCIe saturation; prefetch based on velocity.

---

## Key technical choices (why)

* **64³ chunks @ 10 cm** (6.4 m) give good batching. Internally, process **8³ tiles** in shared memory.
* **Simulation islands** decouple CA from world storage; only dense where needed.
* **Two‑phase CA** avoids write hazards; **checkerboard parity** reduces oscillations.
* **Palette + bitmasks** shrink memory and speed meshing.
* **Cube‑sphere** keeps distortion bounded for planet sampling; everything local remains a straight 3D grid.

---

## Minimal data structures (C++)

> These are lean, compile‑ready sketches to set the shape. Fill in I/O and safety as you build.

### Materials & utility

```cpp
// material ids are compact (<= 255 common materials)
enum Material : uint16_t {
  MAT_AIR = 0, MAT_ROCK, MAT_DIRT, MAT_WOOD, MAT_WATER, MAT_LAVA, MAT_FIRE, MAT_STONE,
  // ...
};

struct VoxelState {
  uint16_t mat;      // Material id (paletted in chunks)
  uint8_t  temp;     // 0..255 mapped to 0..Tmax
  uint8_t  flags;    // bit 0: solid, bit1: fluid, bit2: granular, etc.
};

inline bool is_air(uint16_t m)    { return m == MAT_AIR; }
inline bool is_fluid(uint16_t m)  { return m == MAT_WATER || m == MAT_LAVA; }
inline bool is_granular(uint16_t m){ return m == MAT_DIRT; }
```

### Bit‑packed index array (for paletted chunks)

```cpp
struct BitArray {
  uint32_t bits_per;  // 1,2,4,8
  uint32_t size;      // number of items
  std::vector<uint64_t> data;

  BitArray(uint32_t n, uint32_t bpp): bits_per(bpp), size(n),
    data((n * bpp + 63) / 64, 0ull) {}

  void set(uint32_t i, uint32_t v) {
    const uint64_t bit  = uint64_t(i) * bits_per;
    const uint32_t word = bit >> 6;
    const uint32_t off  = bit & 63;
    const uint64_t mask = ((1ull << bits_per) - 1ull) << off;
    data[word] = (data[word] & ~mask) | (uint64_t(v) << off);
    const uint32_t spill = (off + bits_per) > 64;
    if (spill) {
      const uint32_t w2 = word + 1;
      const uint32_t r  = 64 - off;
      const uint64_t mask2 = (1ull << (bits_per - r)) - 1ull;
      data[w2] = (data[w2] & ~mask2) | (uint64_t(v) >> r);
    }
  }

  uint32_t get(uint32_t i) const {
    const uint64_t bit  = uint64_t(i) * bits_per;
    const uint32_t word = bit >> 6;
    const uint32_t off  = bit & 63;
    uint64_t val = data[word] >> off;
    if ((off + bits_per) > 64) {
      val |= data[word + 1] << (64 - off);
    }
    return uint32_t(val & ((1ull << bits_per) - 1ull));
  }
};
```

### Chunk (64³) with palette + masks

```cpp
struct ChunkKey { int32_t cx, cy, cz; }; // 64-voxel steps in world space

struct Chunk64 {
  static constexpr int N = 64;
  static constexpr int N3 = N*N*N;

  // small palette (<= 256) for this chunk
  std::vector<uint16_t> palette;       // palette[index] -> material id
  std::unordered_map<uint16_t,uint16_t> palette_lut; // material->index
  BitArray indices;     // N3 entries, 1/2/4/8 bits per entry
  std::array<uint64_t, (N3+63)/64> occ;     // occupancy bitset (non-air)
  std::array<uint64_t, (N3+63)/64> fluid;   // fluids bitset (optional)
  std::array<uint64_t, (N3+63)/64> granular;// granular bitset (optional)
  bool dirty_mesh = true;

  Chunk64() : indices(N3, 8) { // start with 8 bpp; can shrink after building palette
    occ.fill(0); fluid.fill(0); granular.fill(0);
    palette.reserve(64);
  }

  uint16_t ensure_palette(uint16_t mat) {
    auto it = palette_lut.find(mat);
    if (it != palette_lut.end()) return it->second;
    uint16_t id = (uint16_t)palette.size();
    palette.push_back(mat);
    palette_lut.emplace(mat, id);
    return id;
  }

  static inline uint32_t lindex(int x,int y,int z) { return (z*N + y)*N + x; }

  void set_voxel(int x,int y,int z, uint16_t mat) {
    const uint32_t i = lindex(x,y,z);
    const uint16_t pi = ensure_palette(mat);
    indices.set(i, pi);
    const uint32_t w = i >> 6, b = i & 63, bit = 1u << b;
    if (mat != MAT_AIR) occ[w] |=  (uint64_t)bit; else occ[w] &= ~((uint64_t)bit);
    // update fluid/granular bitsets similarly
    dirty_mesh = true;
  }

  uint16_t get_material(int x,int y,int z) const {
    const uint32_t i = lindex(x,y,z);
    return palette[ indices.get(i) ];
  }
};
```

### Delta store (sparse edits)

```cpp
struct LocalDelta { uint32_t idx; VoxelState v; }; // idx = local 0..(64^3-1)
struct ChunkDelta {
  std::vector<LocalDelta> cells; // small, sparse list
};
struct DeltaStore {
  struct Key { int32_t cx, cy, cz; bool operator==(const Key& o) const {
    return cx==o.cx && cy==o.cy && cz==o.cz; } };
  struct KeyHash { size_t operator()(const Key& k) const {
    uint64_t h = (uint64_t)(uint32_t)k.cx * 0x9E3779B185EBCA87ull;
    h ^= (uint64_t)(uint32_t)k.cy + 0x9E3779B185EBCA87ull + (h<<6) + (h>>2);
    h ^= (uint64_t)(uint32_t)k.cz + 0x9E3779B185EBCA87ull + (h<<6) + (h>>2);
    return (size_t)h;
  }};
  std::unordered_map<Key, ChunkDelta, KeyHash> map;
};
```

### Simulation island (dense box, GPU‑resident)

```cpp
struct Island {
  Int3 origin_vox;   // world voxel coords of (0,0,0) corner
  Int3 dim;          // e.g., (160,160,160) at 10 cm
  // GPU buffers (SSBOs)
  VkBuffer occBits;      // 1 bit per cell
  VkBuffer matIdx;       // 1 byte per cell (index into island-local palette)
  VkBuffer temp;         // 1 byte per cell
  VkBuffer velFlags;     // 1 byte per cell
  VkBuffer activeA, activeB; uint32_t activeCount;
  VkBuffer moveBuf;      // one entry per active cell
  // ...
};
```

---

## Toy CA kernel (Vulkan GLSL): sand + water + lava

> This is intentionally compact. It uses a **two‑phase** approach with a simple conflict rule: the **lowest “ticket” wins** per destination (ticket derived from cell index and tick). Real code should tile into shared memory and handle boundaries; this shows the gist.

### Common includes (SSBO layout)

```glsl
// bindings: adjust to your descriptor set layout
layout(std430, binding=0) buffer OccBits   { uint occ[]; };   // bitfield: 1 bit per cell
layout(std430, binding=1) buffer MatBuf    { uint mat[]; };   // 0=air,1=sand,2=water,3=lava,4=stone,5=wood,6=fire,...
layout(std430, binding=2) buffer TempBuf   { uint temp[]; };  // 0..255
layout(std430, binding=3) buffer ActiveBuf { uint active[]; }; // indices of active cells
layout(std430, binding=4) buffer NextActive{ uint nextActive[]; };
layout(std430, binding=5) buffer MoveBuf   { uvec2 moves[]; }; // dstIdx, ticket
layout(std430, binding=6) buffer ClaimBuf  { uint claim[]; };  // per-cell ticket (0xFFFFFFFF = unclaimed)

layout(push_constant) uniform PC {
  uvec3 dim;       // island dims (X,Y,Z)
  uint  activeCount;
  uint  tick;
  float g;         // not used in toy (CA-style), useful if doing MAC-style fluids
} pc;

uint lindex(uvec3 p) { return (p.z*pc.dim.y + p.y)*pc.dim.x + p.x; }

bool in_bounds(ivec3 p) {
  return uint(p.x) < pc.dim.x && uint(p.y) < pc.dim.y && uint(p.z) < pc.dim.z;
}

bool get_occ(uint idx) {
  uint word = idx >> 5u, bit = idx & 31u;
  return ((occ[word] >> bit) & 1u) != 0u;
}
void set_occ(uint idx, bool on) {
  uint word = idx >> 5u, bit = idx & 31u, m = 1u << bit;
  if (on) atomicOr(occ[word], m); else atomicAnd(occ[word], ~m);
}
```

### Pass 1: propose

```glsl
// local_size tuned later; 256 threads is fine to start
layout(local_size_x=256) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= pc.activeCount) return;

  uint idx = active[i];
  uint m   = mat[idx];
  if (m == 0u) return; // air
  uvec3 P;
  // reconstruct 3D (cheap integer math)
  uint x = idx % pc.dim.x;
  uint y = (idx / pc.dim.x) % pc.dim.y;
  uint z = idx / (pc.dim.x * pc.dim.y);
  P = uvec3(x,y,z);

  // gravity = -Y for island-local frame (actual radial gravity handled by choosing "down" dir per island)
  ivec3 down = ivec3(0,-1,0);

  // choose desired move
  ivec3 dst = ivec3(P); // default stay
  if (m == 1u /*sand*/) {
    ivec3 cand[4] = { down, down + ivec3(-1,0,0), down + ivec3(1,0,0), down + ivec3(0,0,-1) };
    // try down, then diagonals (two axes shown)
    for (int k=0;k<4;k++) {
      ivec3 q = ivec3(P)+cand[k];
      if (!in_bounds(q)) continue;
      uint qidx = lindex(uvec3(q));
      if (!get_occ(qidx) || mat[qidx]==2u /*water: sand sinks*/) { dst = q; break; }
    }
  } else if (m == 2u /*water*/) {
    ivec3 cand[5] = { down, down+ivec3(-1,0,0), down+ivec3(1,0,0), ivec3(-1,0,0), ivec3(1,0,0) };
    for (int k=0;k<5;k++) {
      ivec3 q = ivec3(P)+cand[k];
      if (!in_bounds(q)) continue;
      uint qidx = lindex(uvec3(q));
      if (!get_occ(qidx)) { dst = q; break; }
    }
  } else if (m == 3u /*lava*/) {
    // like water but slower: only try down, else stay
    ivec3 q = ivec3(P)+down;
    if (in_bounds(q)) {
      uint qidx = lindex(uvec3(q));
      if (!get_occ(qidx)) dst = q;
    }
  } else {
    return;
  }

  uint dstIdx = lindex(uvec3(dst));
  uint ticket = (pc.tick * 73856093u) ^ (idx * 19349663u); // quick hash
  moves[i] = uvec2(dstIdx, ticket);
}
```

### Pass 2: commit (+ basic reactions)

```glsl
layout(local_size_x=256) in;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= pc.activeCount) return;

  uint srcIdx = active[i];
  uvec2 mv = moves[i];
  uint dstIdx = mv.x;
  uint ticket = mv.y;
  uint m = mat[srcIdx];

  if (dstIdx == srcIdx || m==0u) return;

  // Claim destination (lowest ticket wins)
  uint prev = atomicMin(claim[dstIdx], ticket);
  bool win = (ticket <= prev);

  if (win) {
    // Reactions: lava+water -> stone
    uint dm = mat[dstIdx];
    if ((m==3u && dm==2u) || (m==2u && dm==3u)) {
      // place stone at dst, leave air at src
      mat[dstIdx] = 4u; set_occ(dstIdx, true);
      mat[srcIdx] = 0u; set_occ(srcIdx, false);
      return;
    }
    // Swap with water if sand moving into water
    if (m==1u && dm==2u) {
      mat[dstIdx] = 1u; // sand
      mat[srcIdx] = 2u; // water
      set_occ(dstIdx, true);
      set_occ(srcIdx, true);
      return;
    }
    // Regular move into empty
    if (!get_occ(dstIdx)) {
      mat[dstIdx] = m; set_occ(dstIdx, true);
      mat[srcIdx] = 0u; set_occ(srcIdx, false);
      return;
    }
  }
  // Loser or blocked: keep active for next tick
  // nextActive append would go here (use atomic counter)
}
```

> Notes
> • In real code, **initialize `claim[]` each tick to 0xFFFFFFFF** (e.g., via a clear pass).
> • Use **shared memory tiles (8×8×8)**: load mat/occ into `shared` arrays, operate entirely there, then commit to global to reduce bandwidth.
> • Add **checkerboard parity** (even/odd based on (x+y+z+tick)&1) to reduce livelock.
> • Add a **nextActive** queue with an atomic pointer to schedule neighbors of any cell that moved (and cells that lost a claim).

---

## World ↔ island integration (practical loop)

1. **Detect activity triggers** (player dig/place, material source, slope failure) → request/expand an island centered at that area.
2. **Bake‑in**: sample base+delta into island buffers (GPU copy via staging).
3. **Simulate K ticks** (limited per frame or until island calm).
4. **Bake‑out**: diff against previous snapshot → write small set of changed cells into the **DeltaStore**; mark chunks dirty → re‑mesh (compute or CPU).
5. **Retire** island if calm for N ticks; otherwise migrate/grow it if the frontier is active.

---

## File formats (lean, versioned)

**Region file** (`.rgn`), per cube‑sphere face:

```
Header {
  char magic[4] = "RGN1";
  uint32 version;
  int32 face;      // 0..5 cube face
  int32 faceOriginChunkX, faceOriginChunkY; // face-local chunk grid origin
}
Directory[1024] for 32×32 chunks {
  uint32 offset;   // 4KB sectors from file start
  uint32 length;   // in bytes
  uint32 mtime;    // unix time
}
ChunkBlob (zstd):
{
  ChunkHeader { int16 cx, cy, cz; uint8 bpp; uint16 paletteCount; }
  palette[paletteCount] : uint16
  indices : packed bits (bpp ∈ {1,2,4,8})
  occBits : ceil(64^3 / 8) bytes
  classBits (fluid/granular) : optional
}
```

**Delta journal** (`.dlt`):

```
Header "DLT1", version, seed
Entries: [ ChunkKey, count, {localIdx, mat, temp, flags} * count ]
// Periodically compact into the region chunk blobs.
```

---

## Performance checklist (per milestone)

* Use **indirect draws** and **persistent mapped** buffers for mesh streaming.
* Batch chunk meshes per material to reduce binds.
* Keep **CA active set** small: promote to active only changed cells and their neighborhood.
* In compute, prefer **SoA** layouts and **shared memory** tiles.
* Use **async compute** queue for CA; render on graphics queue; synchronize with timeline semaphores.

---

## What to implement next (short path)

1. **Phases 1–3** to get a navigable, chunked planet with greedy meshing (no CA).
2. **Phase 4** to persist edits.
3. **Phases 5–6** to bring up the first simulation island with sand+water.
4. **Phase 7** for lava/fire and reactions.
5. **Phase 8** for refinement bubbles (5 cm/2.5 cm).
6. **Phase 9** for far‑field visuals.

I can turn any of these phases into a **week-by-week task list** with specific classes, API calls, and unit tests. If you want, I’ll also sketch the **Vulkan descriptor set layouts** and a **tiny C++ driver** that records the two compute passes for the CA and presents timing queries so you can profile immediately.
