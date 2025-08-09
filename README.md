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

### Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Run

```
./build/wanderforge
```

This prints detected Vulkan devices. Build with `-DCMAKE_BUILD_TYPE=Debug` to try enabling validation layers (if installed).

### Notes

- If using the Vulkan SDK, ensure the `VULKAN_SDK` environment variable is set (Windows/macOS) and your runtime is properly installed.
- On macOS you need MoltenVK; the SDK includes it.
 - If CMake errors about GLFW not found, install the GLFW dev package listed above and re-run `cmake -S . -B build`.

## Contributing

Early days—no external contributions yet. Feedback and ideas are welcome; issues can be used to capture discussion once the repository structure is in place.

## License

TBD.
