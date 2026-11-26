# Steam Audio RT Core

Hardware ray tracing backends for Steam Audio's custom ray tracer.

Steam Audio keeps doing all the acoustics: reflection paths, occlusion, pathing.
This project only answers the two geometric questions Steam Audio asks, and it
answers them on the GPU's ray tracing hardware:

- closest hit along a ray interval
- any hit along a ray interval

Both are exposed through `IPL_SCENETYPE_CUSTOM`, so no Steam Audio source files
are modified.

## Backends

| Backend | Host API | Traversal | Status |
|---|---|---|---|
| Metal 4 | `MTL4CommandQueue` / `MTL4ComputeCommandEncoder` | MSL `intersection_query<triangle_data>` | Implemented, macOS 26+ |
| Vulkan | `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` | GLSL `rayQueryEXT` | Implemented (build-tested; run on a Vulkan RT GPU) |

Out of scope (no plan): legacy Metal 3 host path, OptiX, CUDA, Radeon Rays 4.0.
The project deliberately stays at two backends; see `AGENTS.md`.

Ray traversal always runs on RT cores. The "compute shader" is only the launch
vehicle: it issues inline ray queries. There is no hand-written BVH traversal
and no CPU fallback. If the device does not support ray queries, the backend
factory returns `nullptr`.

Vulkan RT is not available on macOS: MoltenVK exposes no
`VK_KHR_ray_query` / `VK_KHR_acceleration_structure`, so the Vulkan backend is
built but returns `nullptr` there. Use macOS + Metal 4, or Windows/Linux with a
Vulkan RT capable driver.

## Layout

```
include/rtcore/rtcore.h        public backend interface
include/rtcore/steam_audio.h   Steam Audio custom-scene adapter
src/steam_audio_adapter.cpp    callback conversion + material mapping
src/backend/metal/             MTL4 host code + MSL shader
src/backend/vulkan/            Vulkan host code
src/shaders/vulkan/            GLSL ray-query shader
tests/                         backend conformance + Steam Audio E2E test
bench/                         throughput benchmark
external/steam-audio/          pinned upstream submodule (v4.8.1)
```

## Build

Requirements:

- CMake 3.25+ and Ninja
- A C++23 compiler (Apple Clang 21 / MSVC 19.4x / GCC 14+)
- Xcode 26+ for Metal 4
- Vulkan SDK 1.2+ with `glslangValidator` for the Vulkan backend

Steam Audio is a pinned submodule and is built as an isolated ExternalProject:

```bash
git submodule update --init --recursive
```

Steam Audio needs a few prebuilt dependencies. The canonical way is the
upstream script (it clones flatbuffers, mysofa, pffft, zlib):

```bash
cd external/steam-audio/core/build
python3 get_dependencies.py -p osx -a arm64     # macOS
# python3 get_dependencies.py -p linux -a x64   # Linux
```

On macOS, `scripts/bootstrap_deps_macos.sh` is a much faster alternative: it
uses Homebrew for flatbuffers and builds only pffft + libmysofa from sources
fetched through a fast mirror. Override `MYSOFA_URL` if that mirror is not
reachable from your network.

```bash
./scripts/bootstrap_deps_macos.sh
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
```

`-DRTCORE_ENABLE_STEAMAUDIO=OFF` builds only the backends, without the adapter.

## CI

`.github/workflows/ci.yml` runs two jobs:

- `macos-26`: bootstrap deps, build with Xcode 26 toolchain, run both tests
  against Metal 4, then a benchmark smoke test.
- `ubuntu-24.04`: build the Vulkan backend with `libvulkan-dev` and
  `glslang-tools`, run the tests (they skip with code 77 when the runner has no
  RT-capable Vulkan device).

## Usage

```cpp
#include "rtcore/rtcore.h"
#include "rtcore/steam_audio.h"

auto tracer = rtcore::createMetalTracer();          // or createVulkanTracer()
if (!tracer) { /* no RT hardware */ }

// Fill SceneDesc with meshes, triangles and materials (3-band IPLMaterial layout).
rtcore::Status status;
auto scene = rtcore::SteamAudioScene::create(context, *tracer, sceneDesc, &status);

iplSimulatorSetScene(simulator, scene->handle());
```

The tracer owns one acceleration structure, so use one tracer per scene. It
must outlive `SteamAudioScene`. Steam Audio may call the callbacks
from several worker threads; each backend serializes submissions internally.

## Steam Audio mapping

The adapter installs all four callbacks (`closest`, `any`, batched variants)
and maps results as follows:

| `IPLHit` field | Source |
|---|---|
| `distance` | hardware ray query `t` |
| `triangleIndex` | global BLAS primitive index |
| `objectIndex` | per-mesh `Mesh::objectIndex` |
| `materialIndex` | per-triangle `Triangle::materialIndex` |
| `normal` | geometric normal, `normalize(cross(e1, e2))` |
| `material` | pointer into the adapter's copied material table |

Batched callbacks are forwarded as one GPU dispatch. Steam Audio's single-ray
callbacks (`DirectSimulator` occlusion/transmission, probe generation) are
executed as one-ray dispatches, because the custom-tracer contract is
synchronous.

## Tests

- `rtcore_test_backend`: hit/miss/backface/tmin/tmax, object/material mapping,
  normal, any-hit, and a 4096-ray batch.
- `rtcore_test_steamaudio`: real Steam Audio simulator over a custom scene.
  A wall must produce `occlusion == 0` and `transmission[0] == 0.5`; a clear
  path must produce `1.0` for both.

## Performance

Measured with `rtcore_bench`, which includes command submission and readback
(the real Steam Audio callback path), not just kernel time.

**Machine: Apple M6, 12-core GPU, macOS 27.3.**

Scene: 131,072 triangles (flat grid), 1,048,576 rays, all rays aimed at the
grid. Best of 7 iterations after a warm-up; the mean is roughly 2-5% lower.

| Backend | GPU | Ray batch | Closest hit | Any hit |
|---|---|---|---|---|
| Metal 4 ray query | Apple M6 | 4,096 | 20.5 Mrays/s | 29.5 Mrays/s |
| Metal 4 ray query | Apple M6 | 65,536 | 137.8 Mrays/s | 190.0 Mrays/s |
| Radeon Rays 2.0 | not measured | — | — | — |

The small batch is Steam Audio's typical `rayBatchSize`; the large batch shows
what the same GPU path reaches when the per-submission sync cost is amortized.
If you control `IPLSimulationSettings::rayBatchSize`, larger batches pay off on
Apple Silicon.

Radeon Rays 2.0 is Steam Audio's built-in Windows/AMD OpenCL backend. It was
not measured here (this machine has no AMD GPU and no Windows/OpenCL runtime),
and this project does not integrate it. Treat that row as unfilled, not as a
comparison result.

Reproduce:

```bash
./build/macos/rtcore_bench --rays 1048576 --batch 4096 --grid 256 --iters 7
./build/macos/rtcore_bench --rays 1048576 --batch 65536 --grid 256 --iters 7
```
