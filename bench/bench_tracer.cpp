// Throughput benchmark for the rtcore backends.
// Usage: rtcore_bench [--rays N] [--grid N] [--batch N]
#include "rtcore/rtcore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

rtcore::Status buildGrid(int grid, std::vector<rtcore::Vec3>& vertices,
                         std::vector<rtcore::Triangle>& triangles,
                         std::vector<rtcore::Material>& materials,
                         rtcore::Mesh& mesh, rtcore::SceneDesc& scene) {
    vertices.clear();
    triangles.clear();
    const float extent = 10.0f;
    for (int y = 0; y <= grid; ++y) {
        for (int x = 0; x <= grid; ++x) {
            const float fx = -extent + 2.0f * extent * static_cast<float>(x) / static_cast<float>(grid);
            const float fy = -extent + 2.0f * extent * static_cast<float>(y) / static_cast<float>(grid);
            vertices.push_back({fx, fy, 0.0f});
        }
    }
    const auto stride = static_cast<std::uint32_t>(grid + 1);
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(y) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + stride;
            const std::uint32_t i3 = i2 + 1;
            triangles.push_back({i0, i1, i3, 0});
            triangles.push_back({i0, i3, i2, 0});
        }
    }

    materials.assign(1, rtcore::Material{});
    mesh.vertices = vertices.data();
    mesh.vertexCount = static_cast<std::uint32_t>(vertices.size());
    mesh.triangles = triangles.data();
    mesh.triangleCount = static_cast<std::uint32_t>(triangles.size());
    mesh.objectIndex = 0;

    scene.meshes = &mesh;
    scene.meshCount = 1;
    scene.materials = materials.data();
    scene.materialCount = materials.size();
    return rtcore::Status::ok;
}

double seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    std::size_t totalRays = 1u << 20;
    std::size_t batchSize = 4096;
    int grid = 256;
    int iterations = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--rays") == 0) {
            totalRays = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--batch") == 0) {
            batchSize = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--grid") == 0) {
            grid = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--iters") == 0) {
            iterations = std::max(1, std::atoi(argv[i + 1]));
        }
    }
    batchSize = std::max<std::size_t>(batchSize, 1);

    std::unique_ptr<rtcore::IRayTracer> tracer = rtcore::createMetalTracer();
    if (!tracer) {
        tracer = rtcore::createVulkanTracer();
    }
    if (!tracer) {
        std::fprintf(stderr, "no ray tracing backend available\n");
        return 77;
    }

    std::vector<rtcore::Vec3> vertices;
    std::vector<rtcore::Triangle> triangles;
    std::vector<rtcore::Material> materials;
    rtcore::Mesh mesh;
    rtcore::SceneDesc scene;
    buildGrid(grid, vertices, triangles, materials, mesh, scene);
    if (tracer->build(scene) != rtcore::Status::ok) {
        std::fprintf(stderr, "scene build failed\n");
        return 1;
    }

    std::printf("backend: %s\n", tracer->name());
    std::printf("triangles: %zu\n", triangles.size());
    std::printf("rays: %zu, batch: %zu\n", totalRays, batchSize);

    std::vector<rtcore::Ray> rays(batchSize);
    std::vector<float> minDistances(batchSize, 0.0f);
    std::vector<float> maxDistances(batchSize, 100.0f);
    std::vector<rtcore::Hit> hits(batchSize);
    std::vector<std::uint8_t> occluded(batchSize);

    std::uint32_t seed = 0x12345678u;
    auto nextRandom = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (auto& ray : rays) {
        ray.origin = {-9.5f + 19.0f * nextRandom(), -9.5f + 19.0f * nextRandom(), 5.0f};
        ray.direction = {0.0f, 0.0f, -1.0f};
    }

    auto runOnce = [&](bool anyHit) {
        std::size_t processed = 0;
        double checksum = 0.0;
        const auto start = std::chrono::steady_clock::now();
        while (processed < totalRays) {
            const std::size_t count = std::min(batchSize, totalRays - processed);
            if (anyHit) {
                tracer->traceAny(rays.data(), minDistances.data(), maxDistances.data(), count, occluded.data());
                for (std::size_t i = 0; i < count; ++i) {
                    checksum += occluded[i];
                }
            } else {
                tracer->traceClosest(rays.data(), minDistances.data(), maxDistances.data(), count, hits.data());
                for (std::size_t i = 0; i < count; ++i) {
                    checksum += hits[i].distance < 1000.0f ? hits[i].distance : 0.0f;
                }
            }
            processed += count;
        }
        const double elapsed = seconds(start);
        volatile double keep = checksum;
        (void)keep;
        return elapsed;
    };

    auto measure = [&](bool anyHit) {
        runOnce(anyHit); // warm-up, not counted
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
    return 0;
}
