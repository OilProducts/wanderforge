# Repository Guidelines

## Project Structure & Module Organization
- `src/`: C++20 sources (e.g., `main.cpp`, `vk_app.{h,cpp}`, meshing, planet).
- `include/`: Public headers shared across targets.
- `shaders/`: GLSL 450 sources; compiled to SPIR‑V into `build/shaders/` when shader tools are available.
- `tools/`: Small CPU‑only utilities (e.g., `wf_ringmap`, `wf_chunk_demo`).
- `cmake/`: Build helpers and templates (e.g., `wf_config.h.in`).
- `build/`: CMake build output (generated; ignored).
- `PLAN.md` / `ROADMAP.md`: Technical plan and original planning notes.

## Build, Test, and Development Commands
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- Build all: `cmake --build build --config Debug`
- Run app: `./build/wanderforge`
- CPU tools: `cmake --build build --target wf_ringmap` and `cmake --build build --target wf_chunk_demo`
- Dependencies: Vulkan SDK or system Vulkan, GLFW 3.3+, CMake 3.16+, C++20 compiler.
  - Debian/Ubuntu: `sudo apt install libvulkan-dev vulkan-tools libglfw3-dev`
  - Fedora: `sudo dnf install vulkan-loader-devel vulkan-tools glfw-devel`
- Shader tools (optional): `glslc` or `glslangValidator`. If absent, graphics path falls back to clear‑only; compute is disabled.
- Debug builds attempt to enable Vulkan validation layers.

## Coding Style & Naming Conventions
- Language: C++20. Warnings: `-Wall -Wextra -Wpedantic`; fix new warnings.
- Indentation: 4 spaces, no tabs. UTF‑8 source files.
- Naming: `PascalCase` for types/classes (`VulkanApp`); `snake_case` for functions (`create_instance`); members end with `_` (`instance_`).
- Files: `lower_snake_case.*` for engine files; keep top‑level docs in uppercase (`README.md`, `PLAN.md`).
- Keep headers minimal; prefer forward declarations in headers and includes in `.cpp`.

## Shaders & Assets
- Source location: put GLSL in `shaders/`; use version `#version 450`.
- Build: CMake compiles to `build/shaders/*.spv` when tools are present; otherwise the app looks for prebuilt SPIR‑V and continues if missing.
- Missing shaders: keep messages at info level and do not treat as errors (render path falls back to clear‑only).
- Config header: shader directory is provided via generated `wf_config.h` macro `WF_SHADER_DIR`; avoid hardcoding absolute paths.

## Testing Guidelines
- Current: no formal test suite. Add tests under `tests/` when introduced.
- Recommend Catch2 or GoogleTest; name files `*_test.cpp` and wire `ctest` via CMake.
- Include clear repro steps in PRs until tests land.

## Planet Math & Chunking
- Cube‑sphere mapping APIs must remain exact inverses (`direction_from_face_uv` ↔ `face_uv_from_direction`).
- Keep conversions stable: `voxel_from_lat_lon_h` and `lat_lon_h_from_voxel` should round‑trip within voxel precision.
- Tools: `wf_ringmap` provides quick visual checks; keep it CPU‑only and dependency‑light.
- Chunk generation now runs column-by-column (cache face direction/surface height once per 64×64 column, fill vertical strata, and only evaluate cave FBM below the surface). Keep the fast path intact when tweaking terrain so generation stays sub-millisecond per chunk.
- `ChunkDelta` is tiered: start sparse, promote to dense when ~18 % of voxels change, and demote once activity drops. Always mark deltas dirty when mutating entries so `flush_dirty_chunk_deltas()` can persist them when `save_chunks_enabled=true`.
- Runtime editing: loader threads cache the latest `Chunk64` contents per `FaceChunkKey`. Left click (while in mouse-look) digs the targeted voxel to air; `G` places dirt into the empty cell in front of the surface. Any edit must go through `ChunkDelta::apply_edit` so overrides stay consistent and deltas remain flushable.

## Meshing
- Maintain both naive and greedy meshers; greedy must merge coplanar quads per axis and preserve face normals/material ids.
- Mesh demo (`wf_chunk_demo`) should remain CPU‑only and fast to run as a smoke test (prints counts only).

## PR & Review Guidelines
- Style: Conventional Commits.
  - Examples: `feat(phase1): GLFW + Vulkan clear loop`, `docs(readme): link PLAN.md`, `build: require GLFW dev package`.
- PRs: describe intent, link to the relevant phase in `PLAN.md`, list build/run steps, and note platform (Linux/Windows/macOS). Include short logs/screenshots where relevant.
- Keep diffs focused; avoid unrelated reformatting. Update docs when behavior or commands change.

## Cross‑Platform & Vulkan Notes
- Linux first; keep Windows/macOS builds healthy (MoltenVK on macOS; portability enumeration/subset guarded in code).
- Prefer mailbox present mode when available; fall back to FIFO.
- Depth state: triangle path has no depth; chunk path enables depth testing/writes.
- Validation layers: enable in Debug; ensure the app still runs if layers aren’t installed.
- Render conventions (Phase 3.5): build matrices column-major (`MVP = P * V`) using Vulkan’s 0..1 clip depth, treat +Y as up, and assume CCW front faces with `VK_CULL_MODE_BACK_BIT`. Overlay/HUD keeps culling disabled but uses the same winding. Use the HUD `Axes`/`Tri` toggles to validate orientation after math changes.

## Security & Configuration Tips
- Prefer Debug builds while developing (validation layers on). If validation is missing, install the Vulkan SDK.
- Do not commit build artifacts or local config; keep secrets out of source control.

## For Automation/Agents
- Keep changes minimal and surgical; do not introduce new dependencies without discussion.
- Update CMake when adding sources/tools/shaders and keep CPU tools free of Vulkan/GLFW.
- Preserve info‑level behavior for missing shaders/compute; never fail the app for absent tooling.
- Follow the established naming/layout; place headers in `include/`, sources in `src/`, tools in `tools/`, shaders in `shaders/`.
- Align work with `PLAN.md` phases; document which phase a change advances.
