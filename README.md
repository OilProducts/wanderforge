# Wanderforge

An experimental game project in early planning. The original LLM-aided planning document has been preserved as `ROADMAP.md`. This `README` will be the entry point as the codebase takes shape.

## Status

- Bootstrapped: CMake + C++20 + minimal Vulkan device enumeration.

## What’s Here

- `ROADMAP.md`: The detailed planning/roadmap document that guided the initial direction (kept intact).
- `README.md`: This file. A concise overview that will grow with the project.

## Project Goals (early sketch)

- Explore a walkable, streamable planetary sandbox with simulation “islands”.
- Start small, ship vertical slices, and iterate pragmatically.
- Keep performance and memory budgets front-and-center.

See `ROADMAP.md` for the deeper technical direction and phased plan. All details are subject to change as implementation begins.

## Getting Started

### Prerequisites

- A C++20 compiler (`g++` or `clang++`).
- CMake 3.16+.
- Vulkan SDK or system Vulkan headers/runtime.
  - Linux (Debian/Ubuntu): `sudo apt install libvulkan-dev vulkan-tools`
  - Linux (Fedora): `sudo dnf install vulkan-loader-devel vulkan-tools`
  - Windows/macOS: install the Vulkan SDK from LunarG (MoltenVK on macOS).

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

## Contributing

Early days—no external contributions yet. Feedback and ideas are welcome; issues can be used to capture discussion once the repository structure is in place.

## License

TBD.
