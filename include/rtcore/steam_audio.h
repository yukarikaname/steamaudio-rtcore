// Bridges an rtcore::IRayTracer into a Steam Audio custom scene.
#pragma once

#include "phonon.h"
#include "rtcore/rtcore.h"

#include <memory>
#include <vector>

namespace rtcore {

class SteamAudioScene {
public:
    // A tracer owns one acceleration structure, so create one tracer per scene.
    // The tracer must outlive this object. Returns nullptr on failure and writes
    // the reason to status (when provided).
    static std::unique_ptr<SteamAudioScene> create(IPLContext context,
                                                   IRayTracer& tracer,
                                                   const SceneDesc& scene,
                                                   Status* status = nullptr);

    ~SteamAudioScene();

    SteamAudioScene(const SteamAudioScene&) = delete;
    SteamAudioScene& operator=(const SteamAudioScene&) = delete;

    IPLScene handle() const { return scene_; }

private:
    SteamAudioScene(IPLContext context, IRayTracer& tracer, const SceneDesc& scene);

    static void IPLCALL closestCallback(const IPLRay* ray,
                                        IPLfloat32 minDistance,
                                        IPLfloat32 maxDistance,
                                        IPLHit* hit,
                                        void* userData);
    static void IPLCALL anyCallback(const IPLRay* ray,
                                    IPLfloat32 minDistance,
                                    IPLfloat32 maxDistance,
                                    IPLuint8* occluded,
                                    void* userData);
    static void IPLCALL batchedClosestCallback(IPLint32 numRays,
                                               const IPLRay* rays,
                                               const IPLfloat32* minDistances,
                                               const IPLfloat32* maxDistances,
                                               IPLHit* hits,
                                               void* userData);
    static void IPLCALL batchedAnyCallback(IPLint32 numRays,
                                           const IPLRay* rays,
                                           const IPLfloat32* minDistances,
                                           const IPLfloat32* maxDistances,
                                           IPLuint8* occluded,
                                           void* userData);

    void writeHit(const Hit& hit, IPLHit* out) const;

    IRayTracer& tracer_;
    std::vector<Material> materials_;
    Material fallbackMaterial_{};
    IPLScene scene_ = nullptr;
};

} // namespace rtcore
