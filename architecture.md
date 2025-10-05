# Architecture Notes

## Context (October 4, 2025)
- Current `wf::VulkanApp` owns window/input, renderer lifetime, HUD, streaming, and editing logic directly.
- Chunk streaming spawns ad-hoc worker threads per ring load even though `StreamingService` already exists.
- Rendering and overlay components each discover shader artifacts independently.

## Proposed Subsystem Split

### 1. Platform Layer
- **WindowInput** (existing) + new `PlatformEvents` interface to expose input/state changes without binding to gameplay logic.
- Responsibility: window lifecycle, input events, OS integration.
- Output: dispatches events to the App Facade.

### 2. Render Orchestrator
- New `RenderSystem` owning `Renderer`, swapchain, debug visuals, and pipeline lifetimes.
- Feeds drawables supplied by higher layers (`SceneRenderList`, HUD batches).
- Manages shader module lookup via shared `ShaderCache` service.

### 3. World Runtime
- New `WorldRuntime` wrapping `WorldStreamingSubsystem`, player camera state, and edit tools.
- Provides: `update(dt)`, `snapshot_camera()`, `stream_status()`.
- Encapsulates config application and voxel edit API.

### 4. UI & Telemetry
- `HudController` coordinating `OverlayRenderer`, `ui::UIContext`, metrics sampling, profile CSV sink.
- Consumes data from `WorldRuntime` and `RenderSystem` without direct device access.

### 5. Application Facade
- Slim `AppController` replacing monolithic `VulkanApp`.
- Coordinates lifecycle: init subsystems, main loop, config reload/save.
- Handles high-level mode switching (walk vs fly) by delegating to `WorldRuntime`.

### 6. Shared Services
- `ShaderCache`: resolves SPIR-V paths, watches for changes, caches modules.
- `ConfigService`: existing `AppConfigManager`; move config deltas here.
- `TaskScheduler`: existing `StreamingService`, expanded to handle generation and save queues.

## Interaction Outline
1. `AppController` pulls OS events from `Platform Layer`, forwards normalized commands to `WorldRuntime` / `HudController`.
2. `WorldRuntime` updates streaming via `TaskScheduler`, emits `ChunkDrawItem`s to `RenderSystem`.
3. `HudController` builds overlay draw data, submits to `RenderSystem`.
4. `RenderSystem` composes frame using shader assets via `ShaderCache`.

## Target Ownership Graph (WIP)
- **AppController** (construction order listed)
  - **Platform Layer** (WindowInput + PlatformEvents queue)
  - **WorldRuntime** (configured with TaskScheduler + StreamingService)
  - **RenderSystem** (wraps Renderer, OverlayRenderer, ChunkRenderer, ShaderCache)
  - **HudController** (consumes snapshots from WorldRuntime + RenderSystem)
  - **Shared Services**
    - `ShaderCache`
    - `ConfigService` (AppConfigManager)
    - `TaskScheduler` (extends StreamingService job dispatch)
- **Lifecycle**
  1. AppController bootstraps shared services, then initializes Platform Layer → RenderSystem → WorldRuntime → HudController.
  2. Per-frame: Platform Layer pumps events → AppController translates to input snapshot → WorldRuntime update → HudController update → RenderSystem draw.
  3. Shutdown unwinds in reverse order, ensuring RenderSystem releases GPU resources before Platform teardown.

_Status (October 5, 2025): AppController currently proxies to VulkanApp. Ownership hand-off is underway; Platform Layer, RenderSystem, HudController still live inside VulkanApp._

## Migration Steps (High-Level)
1. Extract config/loading/edit helpers from `VulkanApp` into `WorldRuntime` skeleton.
2. Introduce `AppController` that owns `WorldRuntime`, `RenderSystem`, `HudController`; adapt `main.cpp` to launch it.
3. Move rendering setup/teardown from `VulkanApp` into `RenderSystem`; provide minimal facade to the controller.
4. Introduce `ShaderCache` utility and update chunk/overlay renderers to consume it.
5. Refactor streaming jobs to use `TaskScheduler` queues exclusively.

## Open Questions
- Do we want `WorldRuntime` to own camera movement, or keep a separate `CameraController`?
- Should config hot-reload push updates directly into each subsystem or require explicit apply calls from `AppController`?
- Can HUD data be decoupled enough to allow headless builds (e.g., telemetry server)?

---
Document owner: assistant session (Codex CLI).
