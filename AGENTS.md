# Project Rules

## Language
- C++23 for the core and host code.
- C17 only for C-compatible interfaces.
- Objective-C++ only inside `src/backend/metal/`.
- MSL for Metal shaders; GLSL for Vulkan shaders.
- Keep every source file under 500 lines. Split modules instead of growing files.

## Dependencies
- Steam Audio is a pinned Git submodule at `external/steam-audio` (v4.8.1).
- Never modify tracked files under `external/`. Build inputs may be placed in
  `external/steam-audio/core/deps/` (untracked); integration code lives in `src/`.
- Steam Audio is built as an isolated ExternalProject. It must not be
  `add_subdirectory`-ed into this project.
- The Steam Audio C API is the only coupling point. Do not include private
  `ipl::` headers from our code.

## GPU Architecture
- Ray traversal must run on RT hardware: Metal `intersection_query` or Vulkan
  `rayQueryEXT`.
- Do not write manual BVH traversal, software triangle intersection, or a CPU
  fallback.
- Do not read individual rays back to the CPU. Work in batches.
- A backend must return `nullptr` from its factory when the device or OS does
  not support hardware ray tracing.
- Keep public interfaces simple: plain structs and `rtcore::Status`.
- Backends own their GPU resources with RAII; no raw `new`/`delete`.
- Serialize concurrent access with one mutex per backend. Steam Audio calls
  the custom tracer synchronously from several worker threads.

## Backends
- Implemented: Metal 4 (MTL4 host API + MSL ray query), Vulkan ray query.
- No plan: OptiX, CUDA, Radeon Rays 2.0/4.0, legacy Metal 3 host path.
- Keep the two implemented backends strictly isolated behind `IRayTracer`.

## Testing
- Every backend change must pass `ctest`.
- Tests skip with exit code 77 when no backend is available.
- `rtcore_bench` measures end-to-end throughput including submission and
  readback, because Steam Audio's custom tracer interface is synchronous.

## Performance Reporting
- Report the GPU model, OS, scene triangle count, rays per batch and total rays.
- Never publish numbers from a different machine without labelling them.
