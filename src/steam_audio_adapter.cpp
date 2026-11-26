#include "rtcore/steam_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rtcore {
namespace {

static_assert(sizeof(Ray) == sizeof(IPLRay), "IPLRay must match rtcore::Ray");
static_assert(sizeof(Material) == sizeof(IPLMaterial), "IPLMaterial layout mismatch");
static_assert(IPL_NUM_BANDS == 3, "This adapter assumes three bands");

inline Ray toRay(const IPLRay& ray) {
    Ray out;
    out.origin = {ray.origin.x, ray.origin.y, ray.origin.z};
    out.direction = {ray.direction.x, ray.direction.y, ray.direction.z};
    return out;
}

inline bool validInterval(float minDistance, float maxDistance) {
    return minDistance <= maxDistance;
}

} // namespace

SteamAudioScene::SteamAudioScene(IPLContext context, IRayTracer& tracer, const SceneDesc& scene)
    : tracer_(tracer) {
    if (scene.materials && scene.materialCount > 0) {
        materials_.assign(scene.materials, scene.materials + scene.materialCount);
    }
    IPLSceneSettings settings{};
    settings.type = IPL_SCENETYPE_CUSTOM;
    settings.closestHitCallback = &SteamAudioScene::closestCallback;
    settings.anyHitCallback = &SteamAudioScene::anyCallback;
    settings.batchedClosestHitCallback = &SteamAudioScene::batchedClosestCallback;
    settings.batchedAnyHitCallback = &SteamAudioScene::batchedAnyCallback;
    settings.userData = this;

    IPLerror error = iplSceneCreate(context, &settings, &scene_);
    if (error != IPL_STATUS_SUCCESS) {
        scene_ = nullptr;
    }
}

SteamAudioScene::~SteamAudioScene() {
    if (scene_) {
        iplSceneRelease(&scene_);
    }
}

std::unique_ptr<SteamAudioScene> SteamAudioScene::create(IPLContext context,
                                                         IRayTracer& tracer,
                                                         const SceneDesc& scene,
                                                         Status* status) {
    if (!context || !scene.meshes || scene.meshCount == 0) {
        if (status) {
            *status = Status::invalidArgument;
        }
        return nullptr;
    }

    Status buildStatus = tracer.build(scene);
    if (buildStatus != Status::ok) {
        if (status) {
            *status = buildStatus;
        }
        return nullptr;
    }

    auto result = std::unique_ptr<SteamAudioScene>(new SteamAudioScene(context, tracer, scene));
    if (!result->scene_) {
        if (status) {
            *status = Status::deviceError;
        }
        return nullptr;
    }

    if (status) {
        *status = Status::ok;
    }
    return result;
}

void SteamAudioScene::writeHit(const Hit& hit, IPLHit* out) const {
    *out = IPLHit{};
    out->distance = hit.distance;
    out->triangleIndex = hit.triangleIndex;
    out->objectIndex = hit.objectIndex;
    out->materialIndex = hit.materialIndex;
    out->normal = {hit.normal.x, hit.normal.y, hit.normal.z};

    if (hit.distance < std::numeric_limits<float>::infinity()) {
        const Material* material = &fallbackMaterial_;
        if (hit.materialIndex >= 0 && static_cast<std::size_t>(hit.materialIndex) < materials_.size()) {
            material = &materials_[static_cast<std::size_t>(hit.materialIndex)];
        }
        out->material = reinterpret_cast<IPLMaterial*>(const_cast<Material*>(material));
    }
}

void IPLCALL SteamAudioScene::closestCallback(const IPLRay* ray,
                                              IPLfloat32 minDistance,
                                              IPLfloat32 maxDistance,
                                              IPLHit* hit,
                                              void* userData) {
    SteamAudioScene* self = static_cast<SteamAudioScene*>(userData);
    *hit = IPLHit{};
    if (!self || !ray || !validInterval(minDistance, maxDistance)) {
        return;
    }

    Ray converted = toRay(*ray);
    Hit result;
    if (self->tracer_.traceClosest(&converted, &minDistance, &maxDistance, 1, &result) != Status::ok) {
        return;
    }
    self->writeHit(result, hit);
}

void IPLCALL SteamAudioScene::anyCallback(const IPLRay* ray,
                                          IPLfloat32 minDistance,
                                          IPLfloat32 maxDistance,
                                          IPLuint8* occluded,
                                          void* userData) {
    SteamAudioScene* self = static_cast<SteamAudioScene*>(userData);
    *occluded = 0;
    if (!self || !ray) {
        return;
    }
    if (!validInterval(minDistance, maxDistance)) {
        *occluded = 1;
        return;
    }

    Ray converted = toRay(*ray);
    self->tracer_.traceAny(&converted, &minDistance, &maxDistance, 1, occluded);
}

void IPLCALL SteamAudioScene::batchedClosestCallback(IPLint32 numRays,
                                                     const IPLRay* rays,
                                                     const IPLfloat32* minDistances,
                                                     const IPLfloat32* maxDistances,
                                                     IPLHit* hits,
                                                     void* userData) {
    SteamAudioScene* self = static_cast<SteamAudioScene*>(userData);
    if (!self || numRays <= 0) {
        return;
    }

    thread_local std::vector<Ray> converted;
    thread_local std::vector<Hit> results;
    converted.resize(static_cast<std::size_t>(numRays));
    results.assign(static_cast<std::size_t>(numRays), Hit{});

    for (IPLint32 i = 0; i < numRays; ++i) {
        converted[static_cast<std::size_t>(i)] = toRay(rays[i]);
        hits[i] = IPLHit{};
    }

    Status status = self->tracer_.traceClosest(converted.data(), minDistances, maxDistances,
                                               static_cast<std::size_t>(numRays), results.data());
    if (status != Status::ok) {
        return;
    }

    for (IPLint32 i = 0; i < numRays; ++i) {
        self->writeHit(results[static_cast<std::size_t>(i)], &hits[i]);
    }
}

void IPLCALL SteamAudioScene::batchedAnyCallback(IPLint32 numRays,
                                                 const IPLRay* rays,
                                                 const IPLfloat32* minDistances,
                                                 const IPLfloat32* maxDistances,
                                                 IPLuint8* occluded,
                                                 void* userData) {
    SteamAudioScene* self = static_cast<SteamAudioScene*>(userData);
    if (!self || numRays <= 0) {
        return;
    }

    thread_local std::vector<Ray> converted;
    converted.resize(static_cast<std::size_t>(numRays));
    for (IPLint32 i = 0; i < numRays; ++i) {
        converted[static_cast<std::size_t>(i)] = toRay(rays[i]);
    }

    self->tracer_.traceAny(converted.data(), minDistances, maxDistances,
                           static_cast<std::size_t>(numRays), occluded);

    // Steam Audio's custom hit contract requires invalid intervals to report occlusion.
    for (IPLint32 i = 0; i < numRays; ++i) {
        if (!validInterval(minDistances[i], maxDistances[i])) {
            occluded[i] = 1;
        }
    }
}

} // namespace rtcore
