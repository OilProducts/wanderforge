# Wanderforge Technical Plan

Author: Chris • Language: C++20 • Graphics: Vulkan • OS: Linux first, Windows/macOS supported

This document is a self-contained plan to build an experimental, planetary-scale voxel sandbox with falling-sand style voxel simulation. It consolidates decisions, constraints, phases, and detailed steps to reach a playable, extensible prototype.

## Vision & Scope

- Planet you can circumnavigate in ~20 minutes of running (≈7.2 km circumference at 6 m/s).
- Default 10 cm voxels for common materials; optional 5 cm / 2.5 cm micro-tiers for rare/fine simulations.
- “Blocky” visuals first (fast meshing, easy to debug); optional smooth view-only mesher later.
- Falling-sand style per-voxel physics (granular flow, water, lava, heat, basic reactions).
- Desktop-class hardware target (Ryzen 9800X3D + RTX 4090); keep scalable to mid-range.

## Design Pillars

- Sparse everywhere, dense where it matters: store the world procedurally + sparse edits; keep dense 3D grids only in “simulation islands”.
- Chunked world with paletted storage and fast greedy meshing; recompute only dirty regions.
- GPU-first CA (cellular automata) for active voxels using compute shaders with propose+commit.
- Deterministic, versioned formats for repeatability; seed-locked base world.
- Cross-platform Vulkan with minimal, well-chosen dependencies.

## Scale & Budgets

- Circumference ≈ 7.2 km; radius ≈ 1.15 km; 10 cm voxels → 72,000 cells around equator.
- Chunks: 64³ voxels (6.4 m cube). Typical chunk after greedy meshing ≤ 15k triangles.
- Simulation islands: 12–16 m boxes at 10 cm (≈1.7–4.1 M cells); refine sub-boxes to 5 cm/2.5 cm when needed.
- Frame budgets (target): CA compute 6–9 ms; meshing 1–3 ms amortized; rendering 4–6 ms; IO async.

## World Representation

- Base world (read-only): procedural SDF/heightfield on a cube-sphere with biome/material IDs.
- Delta store (authoritative edits): sparse map keyed by `(chunk_key, local_index)`.
- Runtime chunks (visual/cache): 64×64×64 paletted voxels with bitmasks (air/solid/fluid/granular); meshed on demand.
- Simulation islands: dense GPU-resident volumes spawned where activity happens and retired when calm.

## Data Model & Storage

- Chunk64 (64³):
  - Palette (≤ 256 materials), packed indices (1/2/4/8 bpp) via a bit-array.
  - Bitmasks: occupancy, fluid, granular.
  - Internal tiling: 8×8×8 tiles to match GPU shared-memory patterns.
- Region files: 32×32 chunk tiles per file (face-local for a cube-sphere face); table-of-contents + compressed chunk blobs (zstd/LZ4).
- Delta journal: append-only edit log periodically compacted into region chunk blobs.

## Simulation Model (Falling-Sand CA)

- Active set only: build/maintain a queue of cells to update (plus neighbors) to minimize work.
- Two-phase tick:
  - Propose: each active cell picks a destination (down/diagonals/lateral spill); writes a move ticket.
  - Commit: atomically claim destinations; resolve conflicts; apply reactions (lava+water→stone), swaps (sand↔water).
- Island lifecycle:
  - Bake-in (sample base+delta into dense buffers) → simulate K ticks → bake-out (diff → deltas) → retire or migrate.
- Radial gravity: per-island “down” aligned to gravity direction at island center.
- Refinement bubbles: temporary 2×/4× sub-grids (5 cm / 2.5 cm) in high-complexity zones; deterministic up/down mapping.

## Vulkan Architecture (Minimum Viable)

- Instance with validation + `VK_EXT_debug_utils` (debug messenger). On macOS use MoltenVK and portability enumeration.
- Physical/Logical device selection: prefer discrete GPU; create graphics + compute queues.
- Windowing: GLFW surface, swapchain, image views, render pass (clear-only initially), frame sync (semaphores/fences).
- Memory: Vulkan Memory Allocator (VMA) for buffers/images.
- Pipelines: graphics pipeline (clear/fullscreen), compute pipelines (for CA, later); shader compilation via glslang or shaderc.
- Command infrastructure: per-frame command buffers, transient pools, utils for staging copies and async compute.

## Dependencies

- Required: `glfw` (window/surface), `Vulkan SDK` (or system Vulkan), `VMA` (amalgamated header), `glslang/shaderc`.
- Helpful soon: `glm` (math), `volk` (loader convenience), `fmt` or `spdlog` (logging), `zstd`/`lz4` (region compression).

## Phases, Deliverables, and Acceptance

### Phase 1 — Vulkan Plumbing & Window (Completed)
- Deliverables:
  - Instance with validation and debug messenger; device/queues; command pool; swapchain; per-frame sync.
  - GLFW window; render loop that clears and presents every frame.
  - VMA wired for buffer/image allocations; shader compilation toolchain.
- Acceptance:
  - Runs on Linux; builds on Windows/macOS (MoltenVK). Validation toggle via build type. Prints chosen GPU/queues.
 - Notes:
  - Add minimal `.vk` utilities: error macros, scoped command submitters, pipeline barrier helpers.
 - Progress:
   - 2025-08-09: Implemented GLFW window, Vulkan instance with debug messenger, device/queues, swapchain, image views, render pass, framebuffers, command buffers, sync, and clear-present loop. GPU/queue info printed at startup. Optional VMA detection added (uses `vk_mem_alloc.h` if present).
   - 2025-08-09: Added shader tool detection and a minimal graphics pipeline. If `glslc`/`glslangValidator` are present, the app compiles shaders and draws a colored triangle; otherwise it runs clear-only.
   - 2025-08-09: Added a minimal compute pipeline and no-op dispatch each frame (when `noop.comp.spv` is present). Phase 1 acceptance criteria met and verified.

### Phase 2 — Math, Planet Frame, Base Sampler (Completed)
- Deliverables:
  - `int3/float3` types, transforms; 64-bit world coordinates.
  - Cube-sphere mapping utilities; deterministic noise stack.
  - `BaseSample sample_base(Int3 p) -> {material, density}` across rock/soil/water/lava/air.
- Acceptance:
  - CPU tool to ray-march the planet surface; visualize a ring of voxels/chunks for sanity.
- Progress:
  - 2025-08-09: Added math/noise, cube-sphere mapping, base sampler, and `wf_ringmap` PPM tool to visualize an equatorial strip.
  - 2025-08-09: Added exact inverse mapping (`face_uv_from_direction`). Phase 2 acceptance criteria met.

### Phase 3 — Chunk Store, Region IO, Greedy Meshing (2–3 weeks)
- Deliverables:
  - `Chunk64` with palette/bitmasks/packed indices; internal 8×8×8 tiles.
  - Region file format and async IO; dirty-chunk tracking.
  - CPU greedy mesher producing indexed meshes; triplanar texturing.
  - Renderer with frustum culling and per-material draw lists.
- Acceptance:
  - Walk around a static planet; smooth chunk streaming at 60 fps; typical chunk ≤ 15k tris.
 - Progress:
  - 2025-08-09: Added `Chunk64` (palette + occupancy) and a naive face mesher.
 - 2025-08-09: Implemented CPU greedy meshing; demo tool shows large triangle reduction on a test scene.
  - 2025-08-09: Added Region IO scaffolding (header + TOC + raw blobs) with 32×32 face‑local tiles per region file (per‑k shell). New `wf_region_demo` saves/loads a sample chunk. Compression hooks reserved for zstd/lz4 later.
  - 2025-08-10: Extracted `ChunkRenderer` (pipeline, layout, push constants) and switched to indirect multi‑draw for chunk batches. Added pooled vertex/index buffer allocator.
  - 2025-08-10: Added persistent loader thread + request coalescing, prioritized tile ordering (near‑first, ahead‑of‑camera within ring), and prune hysteresis (load R, prune R+margin) for smooth streaming.
  - 2025-08-10: Fixed device‑lost during streaming by deferring GPU resource destruction by frame; added robust pooled allocator with strict alignment; fixed stride mismatch (pipeline now uses `sizeof(Vertex)`).
  - 2025-08-10: HUD improvements: multi‑line stats; added optional pool usage and loader status; added stream/pool logging toggles for targeted diagnostics.

 - Next tasks (Phase 3 scope):
   - Extract `ChunkRenderer` module (pipeline, vertex layout, push constants, draw paths) — [Done].
   - Centralize Vulkan helpers (shader module loader, `find_memory_type`, buffer utils) for reuse — [Done] (`vk_utils`).
   - Factor a `Camera` utility for view/projection and input config (FOV/near/far, smoothing) — [Done (math helpers)]; input config toggles pending.
   - Expand to a larger streaming ring; add simple CPU frustum culling — [Partial].
   - Move toward draw batching/indirect draws to reduce command overhead — [Done].
   - Persistent loader + request coalescing — [Done].
   - Prioritized tile ordering (near‑first, ahead‑of‑camera within ring) — [Done].
   - Prune hysteresis (load R, prune R+margin) — [Done].
   - Deferred GPU resource destruction by frame (avoid device‑lost) — [Done].
   - Pooled allocator free‑list with strict alignment; logging toggles for stream/pool — [Done].
   - Pool caps configurable + HUD usage line — [Done].
   
   - HUD: DPI/scale control and optional text shadow — [Pending]; skip rebuilds unless content changes — [Done].

 - Upcoming (to complete Phase 3 acceptance):
   - Device‑local pools + staging uploads for higher throughput.

 - Completed since last update:
   - Multi‑Face Streaming: dynamically select face from camera direction, recenter the ring across faces, and preserve continuity with a short dual‑face hold (configurable) during transitions. HUD now shows face and ring center.

 - Extras (non‑blocking additions):
   - Threaded meshing in loader: parallelize the mesh build per tile after voxel generation to further reduce wall‑clock time of a request.
   - Near‑seam neighbor‑face prefetch: when approaching a cube‑face seam (|u| or |v| large), preemptively request the adjacent face’s ring to hide seam transitions completely.

 - Extras (non‑blocking for Phase 3 acceptance):
   - Tail reclamation in pools (shrink tail when freeing the last allocated block).
   - Region IO compression & compaction: compression flags (LZ4/Zstd), async IO worker, compaction, and robust error handling.

### Phase 3.5 — Convention Migration (Vulkan‑native clip space)

- Goal: Migrate rendering to consistent Vulkan‑native conventions to reduce ambiguity and simplify future features (post, shadows), while keeping visuals stable.
- Target conventions:
  - Depth: Vulkan [0,1] (optionally reversed‑Z later).
  - Matrices: column‑major on CPU; upload column‑major; `MVP = P * V`.
  - Front faces: CCW; culling: BACK (unchanged from Phase 3 end state).
  - Coordinate frame: right‑handed world, +Y up (unchanged).
  - Overlay: explicit NDC mapping consistent with Vulkan Y.
- Step‑by‑step plan:
  1) Add math helpers: `perspective_vulkan(fov,aspect,zn,zf)`, `look_at(eye,fwd,up)`, `mul(a,b)` producing column‑major float[16].
  2) Add a common header enabling GLM flags (if GLM used): `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_RIGHT_HANDED`; otherwise document our own math.
  3) Add debug primitives and checks:
     - Axis gizmo at origin (RGB XYZ), and a screen‑space test triangle to validate Y/handedness.
     - A runtime toggle to flip `frontFace` and to disable culling (for quick A/B during migration).
  4) Switch camera path to use column‑major `P * V`; push constants/upload unchanged elsewhere.
  5) Verify culling from default pose; adjust chunk pipeline `frontFace` only if mismatch (expect CCW BACK).
  6) Verify overlay orientation and HUD (NDC Y up). Fix only if discrepancy is observed.
  7) Remove legacy row‑major helpers and any ad‑hoc transposes; centralize all math through the helpers.
  8) Optional: implement reversed‑Z (set `depthCompareOp = GREATER`, swap zn/zf mapping); validate sky/ground cases and precision improvement.
  9) Document the final conventions in README and AGENTS; add a short “troubleshooting visuals” section.
- Acceptance:
  - Default scene renders identically (or better precision) vs Phase 3.
  - No culling inversions; seams remain closed; overlay orientation correct.
  - Debug gizmos confirm axes and screen‑space orientation.

### UI Module Plan — Immediate-Mode Overlay & HUD

To keep the HUD lightweight yet extensible (and ultimately reusable outside Wanderforge), the UI layer will live under a `ui/` directory with no project-specific dependencies. The end state is an immediate-mode GUI toolkit that emits batched geometry for a single Vulkan draw while supporting interactive widgets.

- **Goals**
  - Resolution-independent layout with configurable DPI scale and optional shadows/outlines.
  - Explicit separation of per-frame command building (`UIContext`) and persistent interaction/input state (`UIBackend`).
  - Modular widgets (text, rectangles, meters, later sliders/toggles) composed via lightweight layout helpers.
  - Keep renderer agnostic: the overlay pipeline simply consumes vertex/index buffers supplied by the UI module.

- **Architecture**
  - `ui/ui_context.h|cpp`: builds vertices for primitives, manages style stack, converts screen-space rects to NDC.
  - `ui/ui_backend.h|cpp`: ingests input events, tracks widget state (hovered/pressed/value), and resolves interaction results each frame.
  - `ui/layout.h`: helper functions for anchored boxes, vertical stacks, gutters, etc.
  - `ui/primitives.h`: immediate-mode helpers (`button`, `toggle`, `text`, `rect`, …) built atop context/backend.
  - Overlay renderer becomes a thin wrapper (pipeline + upload) that accepts `UIContext::DrawData`.

- **Phased Rollout**
  1. **Phase A:** Introduce UIContext/UIBackend, move existing HUD text into the new system, add config-driven `hud_scale` and optional drop shadow.
  2. **Phase B:** Add basic interactive widgets (buttons/toggles) and simple layout helpers; wire mouse input from `VulkanApp` through UIBackend.
  3. **Phase C:** Extend primitives (sliders, progress bars, tooltips), add persistent widget state/animations, and expose focus/navigation hooks.
  4. **Phase D:** Optional future work—text input, icons, SDF fonts—without changing the renderer contract.

- **Reusability Checklist**
  - Keep public headers free of Wanderforge-specific types (only standard + Vulkan types needed for uploads).
  - Provide a small adapter struct (`UIDrawData`) so other projects can plug in their own GPU backend.
  - Document the input event format and expected call order (backend `begin_frame`, build widgets, renderer `submit`).

Completion of Phase A satisfies the current Phase 3 HUD requirements; subsequent phases can iterate alongside gameplay features without entangling the render core.

### Phase 4 — Delta Store & Local Remeshing (1–2 weeks)
- Deliverables:
  - Sparse delta map keyed by `(chunk, localIndex)`; read-path overlays base; write-path persists edits and marks dirty neighbors.
- Acceptance:
  - Dig/place tools; changed chunks remesh locally; edits persist across runs.

### Phase 5 — Simulation Islands v1 (10 cm) (2–3 weeks)
- Deliverables:
  - Island manager; dense buffers (occ, material, temp, vel/flags); active queues; move requests.
  - Bake-in/out between islands and world; DMA staging paths.
- Acceptance:
  - Spawn/migrate/retire islands around activity; bake-out writes deltas and triggers remesh.

### Phase 6 — CA Kernel v1: Sand + Water (2–3 weeks)
- Deliverables:
  - Two compute passes (propose/commit) over active cells with conflict resolution and basic swaps.
  - Radial gravity via per-island basis.
- Acceptance:
  - Piles form with believable slope; water fills and flows; sand displaces water; stable 60 fps for 1–2 M active cells on 4090.

### Phase 7 — Thermal + Lava + Fire (2 weeks)
- Deliverables:
  - Temperature diffusion; thresholds for ignite/melt/solidify; lava as viscous fluid and heat source; simple reaction table.
- Acceptance:
  - Lava+water produces stone; wood near lava ignites and burns out with plausible timing.

### Phase 8 — Refinement Bubbles (5 cm / 2.5 cm) (2–3 weeks)
- Deliverables:
  - Sub-grid allocation and simulation for narrow flows; deterministic up/down mapping between tiers.
- Acceptance:
  - Fine features improve without blowing global cost; visual continuity maintained.

### Phase 9 — Visual LOD & Far-Field Planet (1–2 weeks)
- Deliverables:
  - Far mesh (low-poly cube-sphere with displaced elevation); near/far transition and optional clipmaps.
- Acceptance:
  - 2–4 km view distance at 60 fps; minimal popping.

### Phase 10 — Save/Load, Journaling, Robustness (1 week)
- Deliverables:
  - Region index; dirty-bit tracking; delta journaling and periodic compaction; versioned headers; seed locking.
- Acceptance:
  - Large sessions survive restarts; compaction keeps disk usage stable.

### Phase 11 — Tools & Profiling (1 week initial, ongoing)
- Deliverables:
  - ImGui overlay: GPU timings, active counts, island list, memory; heatmaps for activity/temperature/slope; determinism toggles.
- Acceptance:
  - Clear diagnosis of perf/memory; reproducible simulation under fixed RNG seeds.

## Engineering Practices

- Build: `CMake` 3.16+, `C++20`, warnings-as-errors in CI later; Linux primary; Windows/macOS toolchains checked.
- Logging: concise startup dump of GPU/queues/features; validation errors promoted.
- Determinism: version fields for data; fixed RNG derived from `(cell, tick)` when needed.
- Async compute: CA on compute queue; graphics on graphics queue; timeline semaphores for sync.
- Mesh streaming: indirect draws; persistently mapped buffers; material-batched submissions.

## Risks & Mitigations

- Bandwidth-bound CA: use shared-memory tiles (8³), SoA layouts, active-set throttling.
- LOD cracks: start blocky; add Transvoxel or octree-aware dual contouring later for smooth.
- Edit storms: prioritize dirty-region queues; amortize meshing; move to compute meshing if CPU stalls.
- Cross-platform Vulkan quirks: gate features via `VK_KHR_portability_enumeration`; keep optional layers/extensions guarded.

## Immediate Next Action

- Finish Phase 1: add GLFW window + debug messenger + swapchain and clear every frame, with validation in Debug builds. Then proceed to Phase 2’s math/planet frame.

---

Appendix A — Minimal Structures (Sketches)

- `enum Material : uint16_t { MAT_AIR=0, MAT_ROCK, MAT_DIRT, MAT_WOOD, MAT_WATER, MAT_LAVA, MAT_FIRE, MAT_STONE };`
- `struct VoxelState { uint16_t mat; uint8_t temp; uint8_t flags; };`
- `struct BitArray { /* packed indices 1/2/4/8 bpp for 64³ */ };`
- `struct Chunk64 { palette, indices, occ/fluid/granular bitsets, dirty_mesh; }`
- `struct Island { origin_vox, dim; SSBOs: occBits, matIdx, temp, velFlags; active lists; move buffer; }`

Appendix B — CA Passes (Toy)

- Pass 1 Propose: for each active cell, select destination (down/diag/lateral), write `(dst, ticket)`.
- Pass 2 Commit: atomically claim `dst` (min ticket wins); apply reactions and swaps; update occupancy and material buffers.
