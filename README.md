# Wanderforge

An experimental game project in early planning. The original LLM-aided planning document has been preserved as `ROADMAP.md`. This `README` will be the entry point as the codebase takes shape.

## Status

- Bootstrapped: CMake + C++20 + minimal Vulkan device enumeration.

## What’s Here

- `ROADMAP.md`: The detailed planning/roadmap document that guided the initial direction (kept intact).
- `README.md`: This file. A concise overview that will grow with the project.
- `PLAN.md`: A self-contained technical plan with phases, data model, and Vulkan approach.

## Project Goals (early sketch)

- Explore a walkable, streamable planetary sandbox with simulation “islands”.
- Start small, ship vertical slices, and iterate pragmatically.
- Keep performance and memory budgets front-and-center.

See `ROADMAP.md` for the deeper technical direction and phased plan. All details are subject to change as implementation begins.
For a consolidated, actionable plan, see `PLAN.md`.

## Getting Started

### Prerequisites

- A C++20 compiler (`g++` or `clang++`).
- CMake 3.16+.
- Vulkan SDK or system Vulkan headers/runtime.
  - Debian/Ubuntu: `sudo apt install libvulkan-dev vulkan-tools`
  - Fedora: `sudo dnf install vulkan-loader-devel vulkan-tools`
  - Windows/macOS: install the Vulkan SDK from LunarG (MoltenVK on macOS).
- GLFW 3.3+ development headers (window + Vulkan surface creation).
  - Debian/Ubuntu: `sudo apt install libglfw3-dev`
  - Fedora: `sudo dnf install glfw-devel`
  - Arch: `sudo pacman -S glfw-x11` (or `glfw-wayland`)
  - macOS (Homebrew): `brew install glfw`
  - Windows (vcpkg): `vcpkg install glfw3`
 - Optional: Vulkan Memory Allocator (header-only). If available, place `vk_mem_alloc.h` in your include path (e.g., `include/third_party/`) or install a package providing it; the build auto-detects and enables it.

### Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Shaders and Tools

The app compiles GLSL 450 shaders to SPIR‑V at build time if a shader tool is found:

- Tools: either `glslc` (Shaderc) or `glslangValidator` must be in `PATH`.
- Generated SPIR‑V goes to `build/shaders/*.spv` and the app uses that directory automatically.

Install tips:
- Debian/Ubuntu: `sudo apt install glslang-tools` (for `glslangValidator`)
- Fedora: `sudo dnf install glslang` (or install the Vulkan SDK)
- macOS (Homebrew): `brew install glslang`
- Windows: install the Vulkan SDK (includes both tools)

Manually build the shaders target:

```
cmake --build build --target wf_shaders --config Release
ls build/shaders  # expect triangle.*, chunk.*, overlay.* .spv files
```

If the shader tools are missing, the app runs in a “no-shaders” mode: triangle, chunk rendering, and HUD overlay may be disabled. You’ll see info messages like “Shaders not found” or “HUD overlay disabled”. When the overlay is available, startup logs include “HUD overlay enabled”.

### Run

```
./build/wanderforge
```

This opens a window; if shader tools (`glslc` or `glslangValidator`) are available, it draws a few demo chunks (loaded/generated via Region IO on face 0) with a simple free‑fly camera and a small on‑screen HUD. Otherwise it falls back to a basic triangle, or clear‑only if no shaders.

Controls (chunk view):
- W/A/S/D: move forward/left/back/right
- Q/E: move down/up
- Right mouse drag: look around (yaw/pitch)
- Shift: hold to move faster
 - Default: horizontal mouse is inverted (A.K.A. swap left/right). Press `X` to toggle.
 - Title bar HUD shows FPS, position, yaw/pitch, invert flags, and speed. If shaders are available, an in‑window overlay mirrors the same info.

Config options:
- File `wanderforge.cfg` (same directory) or env vars:
  - `invert_mouse_x=true|false` (default true; or `WF_INVERT_MOUSE_X=1`)
  - `invert_mouse_y=true|false` (or `WF_INVERT_MOUSE_Y=1`)
  - `mouse_sensitivity=0.0025` (or `WF_MOUSE_SENSITIVITY`)
  - `move_speed=12.0` (or `WF_MOVE_SPEED`)
  - Camera:
    - `fov_deg=60.0` (or `WF_FOV_DEG`)
    - `near_m=0.1` (or `WF_NEAR_M`)
    - `far_m=1000.0` (or `WF_FAR_M`)
  - Terrain shaping:
    - `terrain_amp_m=12.0` (or `WF_TERRAIN_AMP_M`)
    - `terrain_freq=64.0` (or `WF_TERRAIN_FREQ`)
    - `terrain_octaves=4` (or `WF_TERRAIN_OCTAVES`)
    - `terrain_lacunarity=2.0` (or `WF_TERRAIN_LACUNARITY`)
    - `terrain_gain=0.5` (or `WF_TERRAIN_GAIN`)
  - Profiling/metrics:
    - `profile_csv=true|false` (or `WF_PROFILE_CSV=1`)
    - `profile_csv_path=profile.csv` (or `WF_PROFILE_CSV_PATH`)
  - HUD:
    - `hud_scale=2.0` (or `WF_HUD_SCALE`) to scale text/layout (applied uniformly)
    - `hud_shadow=true|false` (or `WF_HUD_SHADOW`) to toggle drop shadow
    - `hud_shadow_offset=1.5` (or `WF_HUD_SHADOW_OFFSET`) for pixel offset of the shadow

HUD shows loader and upload stats: queue depth, generation and meshing times (total and per-chunk), and uploads per frame with timing. Enable CSV to log per-job and per-frame upload events for offline analysis.
  - Toggle at runtime: press `X` (invert X) or `Y` (invert Y)

## Current Render Conventions (Phase 3)

The renderer is now aligned with Vulkan’s column-major + depth‑0..1 expectations. These notes capture the current state so we can finish Phase 3’s validation/documentation tasks.

- Matrices: CPU builds column-major `MVP = P * V` using `wf::perspective_vk` (Vulkan 0..1 clip depth). Shaders multiply `pc.mvp * vec4(inPos, 1)` directly.
- Coordinate frame: right-handed world with +Y up. Free-fly and walk cameras derive yaw from the local +Y axis and pitch about the camera’s right axis.
- Projection: default field of view comes from `wanderforge.cfg` (`fov_deg`, default 60°); near/far planes are `near_m`/`far_m` (defaults 0.1/1000.0).
- Front faces & culling: the primary pipeline culls `VK_CULL_MODE_BACK_BIT` with `VK_FRONT_FACE_COUNTER_CLOCKWISE`. The chunk and overlay pipelines currently disable culling but their meshes still follow the same CCW orientation.
- Overlay/HUD: quads are generated in screen pixels, converted to Vulkan NDC (Y up), and rendered with alpha blending.

Notes
- The above choices worked reliably across our targets. Mixing GL/Vulkan depth or matrix layouts caused the visual anomalies we debugged; Phase 3.5 will migrate to Vulkan‑native depth and column‑major matrices with validation helpers.

## Voxels On A Sphere (Cube‑Sphere)

- Plain‑English picture: Imagine a cube gently wrapped around a ball. To locate a point on the planet, we shoot a line from the center to that point. Whichever cube side the line passes through is the “face,” and the spot on that face is a simple 2D coordinate. This avoids the nasty stretching near the poles you’d get with a latitude/longitude grid.

- Mapping (how code does it): a 3D direction picks a face (`face_from_direction`). We convert between face‑local coordinates and directions with
  - `direction_from_face_uv(face, u, v)` and
  - `face_uv_from_direction(dir)` (exact inverse).

- World grid: Voxels are 10 cm cubes in a 3D grid. We group them into `64³` chunks (~6.4 m boxes). For streaming/indexing we use `FaceChunkKey {face,i,j,k}` where `i,j` are along the face and `k` steps outward from the center (radial shells).

- Conversions (for tools and gameplay):
  - `voxel_from_lat_lon_h(cfg, lat, lon, height_m)` → voxel index from latitude, longitude, and height above “sea level”.
  - `lat_lon_h_from_voxel(cfg, voxel, out_lat, out_lon, out_h)` → back to spherical coordinates.

- Base world (what’s in a voxel): `sample_base(cfg, voxel)` uses deterministic noise (seeded FBM) to assign materials like air, water, dirt, and rock, with a configurable sea level and sparse caves. This is the read‑only baseline; later, edits are stored as sparse deltas and meshed locally.

- See it yourself: generate a thin strip image around the planet’s surface:
  - `./build/wf_ringmap 1024 256 0 ring.ppm` (equator)
  - `./build/wf_ringmap 1024 256 45 ring_45.ppm` (45° latitude)

### Notes

- If using the Vulkan SDK, ensure the `VULKAN_SDK` environment variable is set (Windows/macOS) and your runtime is properly installed.
- On macOS you need MoltenVK; the SDK includes it.
 - If CMake errors about GLFW not found, install the GLFW dev package listed above and re-run `cmake -S . -B build`.
 - If you provide `vk_mem_alloc.h`, the app will use VMA for memory allocation; otherwise it runs without it.

### Optional: CPU Ring Visualizer

Generate a simple PPM image of a thin ring around the equator using the procedural base sampler (no Vulkan required):

```
cmake --build build --target wf_ringmap --config Release
./build/wf_ringmap 1024 256 0 ring.ppm   # equator
./build/wf_ringmap 1024 256 45 ring_45.ppm # 45° latitude
```

Open `ring.ppm` with any image viewer to inspect surface, water, and cave distribution near the equator.

### Optional: Chunk Meshing Demo (CPU)

Build a tiny test chunk and run the naive mesher (no Vulkan required):

```
cmake --build build --target wf_chunk_demo --config Release
./build/wf_chunk_demo
```

This prints vertex/triangle counts for a simple synthetic scene (flat ground with a small pillar), comparing naive vs. greedy meshing:

```
Naive  -> Vertices: 66688, Tris: 33344
Greedy -> Vertices:     72, Tris:    36
```

Greedy meshing drastically reduces geometry by merging coplanar faces. GPU meshing will follow in Phase 3.

### Optional: Region IO Demo (CPU)

Save/load a chunk into a face-local region file (32×32 tiles per file):

```
cmake --build build --target wf_region_demo --config Release
./build/wf_region_demo 0 0 0 0   # face=0, i=0, j=0, k=0
```

This writes `regions/face0/k0/r_0_0.wfr` and then reloads it, printing a quick round‑trip check. The format is versioned (`WFREGN1`) with a header + TOC; chunks are stored as raw, uncompressed blobs for now.

## Contributing

Early days—no external contributions yet. Feedback and ideas are welcome; issues can be used to capture discussion once the repository structure is in place.

## License

TBD.
