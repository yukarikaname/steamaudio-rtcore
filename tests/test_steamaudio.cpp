// End-to-end test: Steam Audio custom scene -> rtcore backend.
#include "phonon.h"
#include "rtcore/rtcore.h"
#include "rtcore/steam_audio.h"
#include "test_scene.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

constexpr IPLSimulationFlags kSimFlags = static_cast<IPLSimulationFlags>(
    IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
constexpr IPLDirectSimulationFlags kDirectFlags = static_cast<IPLDirectSimulationFlags>(
    IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);

int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

struct Context {
    IPLContext handle = nullptr;
    IPLerror error = IPL_STATUS_SUCCESS;
    Context() {
        IPLContextSettings settings{};
        settings.version = STEAMAUDIO_VERSION;
        error = iplContextCreate(&settings, &handle);
    }
    ~Context() {
        if (handle) {
            iplContextRelease(&handle);
        }
    }
};

IPLCoordinateSpace3 space(float x, float y, float z, float aheadZ) {
    IPLCoordinateSpace3 out{};
    out.right = {1.0f, 0.0f, 0.0f};
    out.up = {0.0f, 1.0f, 0.0f};
    out.ahead = {0.0f, 0.0f, aheadZ};
    out.origin = {x, y, z};
    return out;
}

} // namespace

int main() {
    std::unique_ptr<rtcore::IRayTracer> tracer = rtcore::createMetalTracer();
    if (!tracer) {
        tracer = rtcore::createVulkanTracer();
    }
    if (!tracer) {
        std::printf("no ray tracing backend available; skipping\n");
        return 77;
    }

    testscene::WallScene geometry;
    Context audio;

    if (audio.error != IPL_STATUS_SUCCESS || !audio.handle) {
        std::fprintf(stderr, "iplContextCreate failed: %d\n", static_cast<int>(audio.error));
        return 1;
    }
    rtcore::Status status = rtcore::Status::ok;
    auto scene = rtcore::SteamAudioScene::create(audio.handle, *tracer, geometry.desc(), &status);
    if (!scene) {
        std::fprintf(stderr, "SteamAudioScene creation failed: %s\n", rtcore::toString(status));
        return 1;
    }
    std::printf("backend: %s\n", tracer->name());

    IPLSimulationSettings settings{};
    settings.flags = kSimFlags;
    settings.sceneType = IPL_SCENETYPE_CUSTOM;
    settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    settings.maxNumOcclusionSamples = 1;
    settings.maxNumRays = 32;
    settings.numDiffuseSamples = 1;
    settings.maxDuration = 0.05f;
    settings.maxOrder = 0;
    settings.maxNumSources = 1;
    settings.numThreads = 1;
    settings.rayBatchSize = 64;
    settings.numVisSamples = 1;
    settings.samplingRate = 48000;
    settings.frameSize = 512;

    IPLSimulator simulator = nullptr;
    IPLerror error = iplSimulatorCreate(audio.handle, &settings, &simulator);
    CHECK(error == IPL_STATUS_SUCCESS);
    if (!simulator) {
        return 1;
    }

    iplSimulatorSetScene(simulator, scene->handle());

    IPLSourceSettings sourceSettings{};
    sourceSettings.flags = kSimFlags;
    IPLSource source = nullptr;
    error = iplSourceCreate(simulator, &sourceSettings, &source);
    CHECK(error == IPL_STATUS_SUCCESS);
    if (!source) {
        iplSimulatorRelease(&simulator);
        return 1;
    }

    auto configure = [&](IPLCoordinateSpace3 sourceSpace, IPLCoordinateSpace3 listenerSpace) {
        IPLSimulationInputs inputs{};
        inputs.flags = kSimFlags;
        inputs.directFlags = kDirectFlags;
        inputs.source = sourceSpace;
        inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
        inputs.numOcclusionSamples = 1;
        inputs.numTransmissionRays = 4;
        iplSourceSetInputs(source, inputs.flags, &inputs);

        IPLSimulationSharedInputs shared{};
        shared.listener = listenerSpace;
        shared.numRays = 32;
        shared.numBounces = 1;
        shared.duration = 0.05f;
        shared.order = 0;
        shared.irradianceMinDistance = 0.1f;
        iplSimulatorSetSharedInputs(simulator, inputs.flags, &shared);
    };

    configure(space(0.5f, 0.0f, 5.0f, -1.0f), space(0.5f, 0.0f, -5.0f, 1.0f));
    iplSourceAdd(source, simulator);
    iplSimulatorCommit(simulator);
    iplSimulatorRunDirect(simulator);
    iplSimulatorRunReflections(simulator);

    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(source, kSimFlags, &outputs);

    CHECK(testscene::nearlyEqual(outputs.direct.occlusion, 0.0f));
    CHECK(testscene::nearlyEqual(outputs.direct.transmission[0], 0.5f, 1.0e-3f));

    // Clear line of sight: both the wall occlusion and its transmission disappear.
    configure(space(10.0f, 10.0f, 5.0f, -1.0f), space(10.0f, 10.0f, -5.0f, 1.0f));
    iplSimulatorRunDirect(simulator);
    iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
    CHECK(testscene::nearlyEqual(outputs.direct.occlusion, 1.0f));
    CHECK(testscene::nearlyEqual(outputs.direct.transmission[0], 1.0f, 1.0e-3f));

    iplSourceRemove(source, simulator);
    iplSourceRelease(&source);
    iplSimulatorRelease(&simulator);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("Steam Audio integration checks passed\n");
    return 0;
}
