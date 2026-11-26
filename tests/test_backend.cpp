// Backend conformance test. Runs every backend that the platform provides.
#include "rtcore/rtcore.h"
#include "test_scene.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

void testBackend(rtcore::IRayTracer& tracer) {
    std::printf("backend: %s\n", tracer.name());
    testscene::WallScene scene;
    CHECK(tracer.build(scene.desc()) == rtcore::Status::ok);

    const float inf = std::numeric_limits<float>::infinity();
    rtcore::Ray rays[5] = {
        {{0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}},  // hits triangle 0
        {{0.5f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}},  // hits the back face
        {{10.0f, 10.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, // misses
        {{0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}},  // clipped by tmax
        {{0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}},  // clipped by tmin
    };
    const float minDistances[5] = {0.0f, 0.0f, 0.0f, 0.0f, 6.0f};
    const float maxDistances[5] = {100.0f, 100.0f, 100.0f, 4.0f, 100.0f};

    rtcore::Hit hits[5];
    CHECK(tracer.traceClosest(rays, minDistances, maxDistances, 5, hits) == rtcore::Status::ok);

    CHECK(testscene::nearlyEqual(hits[0].distance, 5.0f));
    CHECK(hits[0].triangleIndex == 0);
    CHECK(hits[0].objectIndex == 3);
    CHECK(hits[0].materialIndex == 0);
    CHECK(testscene::nearlyEqual(hits[0].normal.z, 1.0f));

    CHECK(testscene::nearlyEqual(hits[1].distance, 5.0f));
    CHECK(hits[1].triangleIndex == 0);

    CHECK(hits[2].distance == inf);
    CHECK(hits[2].triangleIndex == -1);
    CHECK(hits[3].distance == inf);
    CHECK(hits[4].distance == inf);

    std::uint8_t occluded[5] = {};
    CHECK(tracer.traceAny(rays, minDistances, maxDistances, 5, occluded) == rtcore::Status::ok);
    CHECK(occluded[0] == 1);
    CHECK(occluded[1] == 1);
    CHECK(occluded[2] == 0);
    CHECK(occluded[3] == 0);
    CHECK(occluded[4] == 0);

    // Large batch: exercises buffer growth and both hit/miss conversion paths.
    std::vector<rtcore::Ray> batch(4096);
    std::vector<float> batchMin(batch.size(), 0.0f);
    std::vector<float> batchMax(batch.size(), 100.0f);
    std::vector<rtcore::Hit> batchHits(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const float x = -1.9f + 3.8f * static_cast<float>(i % 64) / 63.0f;
        const float y = -1.9f + 3.8f * static_cast<float>(i / 64) / 63.0f;
        batch[i] = {{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
    }
    CHECK(tracer.traceClosest(batch.data(), batchMin.data(), batchMax.data(),
                              batch.size(), batchHits.data()) == rtcore::Status::ok);
    std::size_t hitCount = 0;
    for (const auto& hit : batchHits) {
        if (hit.distance < inf) {
            ++hitCount;
            CHECK(hit.objectIndex == 3);
        }
    }
    CHECK(hitCount == batch.size());
}

} // namespace

int main() {
    std::vector<std::unique_ptr<rtcore::IRayTracer>> tracers;
    if (auto metal = rtcore::createMetalTracer()) {
        tracers.push_back(std::move(metal));
    }
    if (auto vulkan = rtcore::createVulkanTracer()) {
        tracers.push_back(std::move(vulkan));
    }

    if (tracers.empty()) {
        std::printf("no ray tracing backend available; skipping\n");
        return 77;
    }

    for (auto& tracer : tracers) {
        testBackend(*tracer);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all backend checks passed\n");
    return 0;
}
