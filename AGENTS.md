# Repository Guidelines

## Project Structure & Module Organization
- `src/`: C++20 sources (e.g., `main.cpp`, `vk_app.{h,cpp}` for GLFW + Vulkan app loop).
- `include/`: Public headers (reserved for future libraries).
- `build/`: CMake build output (generated; ignored).
- `PLAN.md` / `ROADMAP.md`: Technical plan and original planning notes.

## Build, Test, and Development Commands
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build --config Debug`
- Run: `./build/wanderforge`
- Dependencies: Vulkan SDK or system Vulkan, GLFW 3.3+, CMake 3.16+, C++20 compiler.
  - Debian/Ubuntu: `sudo apt install libvulkan-dev vulkan-tools libglfw3-dev`
  - Fedora: `sudo dnf install vulkan-loader-devel vulkan-tools glfw-devel`
Debug builds attempt to enable Vulkan validation layers.

## Coding Style & Naming Conventions
- Language: C++20. Warnings: `-Wall -Wextra -Wpedantic`; fix new warnings.
- Indentation: 4 spaces, no tabs. UTF‑8 source files.
- Naming: `PascalCase` for types/classes (`VulkanApp`); `snake_case` for functions (`create_instance`); members end with `_` (`instance_`).
- Files: `lower_snake_case.*` for engine files; keep top-level docs in uppercase (`README.md`, `PLAN.md`).
- Keep headers minimal; prefer forward declarations in headers and includes in `.cpp`.

## Testing Guidelines
- Current: no formal test suite. Add tests under `tests/` when introduced.
- Recommend Catch2 or GoogleTest; name files `*_test.cpp` and wire `ctest` via CMake.
- Include clear repro steps in PRs until tests land.

## Commit & Pull Request Guidelines
- Style: Conventional Commits.
  - Examples: `feat(phase1): GLFW + Vulkan clear loop`, `docs(readme): link PLAN.md`, `build: require GLFW dev package`.
- PRs: describe intent, link to `PLAN.md` phase/issue, list build/run steps, and note platform (Linux/Windows/macOS). Include logs/screenshots if relevant.
- Keep diffs focused; avoid unrelated reformatting. Update docs when behavior or commands change.

## Security & Configuration Tips
- Prefer Debug builds while developing (validation layers on). If validation is missing, install the Vulkan SDK.
- Do not commit build artifacts or local config; keep secrets out of source control.
