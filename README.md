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

### Run

```
./build/wanderforge
```

This opens a window; if shader tools (`glslc` or `glslangValidator`) are available, it draws a simple triangle and issues a no-op compute dispatch each frame. Otherwise it clears the background. Build with `-DCMAKE_BUILD_TYPE=Debug` to try enabling validation layers (if installed).

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

## Contributing

Early days—no external contributions yet. Feedback and ideas are welcome; issues can be used to capture discussion once the repository structure is in place.

## License

TBD.
