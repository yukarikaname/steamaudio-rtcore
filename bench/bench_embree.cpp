// CPU reference benchmark: Embree 4 on the same grid scene and ray set.
// This is a benchmark-only baseline, not an rtcore backend.
#include <embree4/rtcore.h>


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

struct SceneData {
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    int grid = 0;
};

SceneData makeGrid(int grid) {
    SceneData data;
    data.grid = grid;
    const float extent = 10.0f;
    for (int y = 0; y <= grid; ++y) {
        for (int x = 0; x <= grid; ++x) {
            const float fx = -extent + 2.0f * extent * static_cast<float>(x) / static_cast<float>(grid);
            const float fy = -extent + 2.0f * extent * static_cast<float>(y) / static_cast<float>(grid);
            data.vertices.push_back(fx);
            data.vertices.push_back(fy);
            data.vertices.push_back(0.0f);
        }
    }
    const auto stride = static_cast<std::uint32_t>(grid + 1);
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(y) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + stride;
            const std::uint32_t i3 = i2 + 1;
            data.indices.push_back(i0);
            data.indices.push_back(i1);
            data.indices.push_back(i3);
            data.indices.push_back(i0);
            data.indices.push_back(i3);
            data.indices.push_back(i2);
        }
    }
    return data;
}

RTCScene buildScene(RTCDevice device, const SceneData& data) {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);

    auto* vertices = static_cast<float*>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float),
        data.vertices.size() / 3));
    auto* indices = static_cast<std::uint32_t*>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(std::uint32_t),
        data.indices.size() / 3));
    std::memcpy(vertices, data.vertices.data(), data.vertices.size() * sizeof(float));
    std::memcpy(indices, data.indices.data(), data.indices.size() * sizeof(std::uint32_t));

    rtcCommitGeometry(geometry);
    rtcAttachGeometry(scene, geometry);
    rtcReleaseGeometry(geometry);
    rtcCommitScene(scene);
    return scene;
}

template <typename Function>
void parallelFor(std::size_t count, Function&& function) {
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t chunk = (count + threads - 1) / threads;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        const std::size_t begin = std::min(count, static_cast<std::size_t>(t) * chunk);
        const std::size_t end = std::min(count, begin + chunk);
        if (begin == end) {
            break;
        }
        workers.emplace_back([&, begin, end] { function(begin, end); });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

double seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    std::size_t totalRays = 1u << 20;
    int grid = 256;
    int iterations = 7;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--rays") == 0) {
            totalRays = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--grid") == 0) {
            grid = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--iters") == 0) {
            iterations = std::max(1, std::atoi(argv[i + 1]));
        }
    }

    RTCDevice device = rtcNewDevice(nullptr);
    if (!device) {
        std::fprintf(stderr, "Embree device creation failed\n");
        return 1;
    }
    const SceneData data = makeGrid(grid);
    RTCScene scene = buildScene(device, data);
    if (rtcGetDeviceError(device) != RTC_ERROR_NONE) {
        std::fprintf(stderr, "Embree scene build failed\n");
        return 1;
    }

    std::printf("backend: embree %s (CPU, %u threads)\n", RTC_VERSION_STRING,
                std::max(1u, std::thread::hardware_concurrency()));
    std::printf("triangles: %zu\n", data.indices.size() / 3);
    std::printf("rays: %zu\n", totalRays);

    std::vector<RTCRayHit> rayHits(totalRays);
    std::vector<RTCRay> rays(totalRays);
    std::uint32_t seed = 0x12345678u;
    auto nextRandom = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };

    for (std::size_t i = 0; i < totalRays; ++i) {
        const float x = -9.5f + 19.0f * nextRandom();
        const float y = -9.5f + 19.0f * nextRandom();

        RTCRayHit& hit = rayHits[i];
        hit.ray.org_x = x;
        hit.ray.org_y = y;
        hit.ray.org_z = 5.0f;
        hit.ray.dir_x = 0.0f;
        hit.ray.dir_y = 0.0f;
        hit.ray.dir_z = -1.0f;
        hit.ray.tnear = 0.0f;
        hit.ray.tfar = 100.0f;
        hit.ray.mask = 0xFFFFFFFFu;
        hit.ray.flags = 0;
        hit.ray.time = 0.0f;
        hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

        RTCRay& ray = rays[i];
        ray.org_x = x;
        ray.org_y = y;
        ray.org_z = 5.0f;
        ray.dir_x = 0.0f;
        ray.dir_y = 0.0f;
        ray.dir_z = -1.0f;
        ray.tnear = 0.0f;
        ray.tfar = 100.0f;
        ray.mask = 0xFFFFFFFFu;
        ray.flags = 0;
        ray.time = 0.0f;
    }

    auto runOnce = [&](bool anyHit) {
        const auto start = std::chrono::steady_clock::now();
        if (anyHit) {
            parallelFor(totalRays, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    rays[i].tnear = 0.0f;
                    rays[i].tfar = 100.0f;
                    rtcOccluded1(scene, &rays[i], nullptr);
                }
            });
        } else {
            parallelFor(totalRays, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    rayHits[i].ray.tnear = 0.0f;
                    rayHits[i].ray.tfar = 100.0f;
                    rayHits[i].hit.geomID = RTC_INVALID_GEOMETRY_ID;
                    rtcIntersect1(scene, &rayHits[i], nullptr);
                }
            });
        }
        return seconds(start);
    };

    auto measure = [&](bool anyHit) {
        runOnce(anyHit);
        double best = 1.0e30;
        double total = 0.0;
        for (int i = 0; i < iterations; ++i) {
            const double elapsed = runOnce(anyHit);
            best = std::min(best, elapsed);
            total += elapsed;
        }
        const double mean = total / static_cast<double>(iterations);
        std::printf("%s: best %.2f Mrays/s, mean %.2f Mrays/s (%d iterations)\n",
                    anyHit ? "any-hit" : "closest-hit",
                    static_cast<double>(totalRays) / best / 1.0e6,
                    static_cast<double>(totalRays) / mean / 1.0e6, iterations);
    };

    measure(false);
    measure(true);

    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
}
