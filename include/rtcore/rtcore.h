// Steam Audio RT Core - backend-agnostic ray tracing interface.
// All coordinates are world space, right-handed by convention of the caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace rtcore {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Result of a closest-hit query. A miss is distance == infinity with -1 indices.
struct Hit {
    float distance = std::numeric_limits<float>::infinity();
    std::int32_t triangleIndex = -1; // Globally unique across the scene.
    std::int32_t objectIndex = -1;
    std::int32_t materialIndex = -1;
    float u = 0.0f; // Barycentric coordinate of vertex 1.
    float v = 0.0f; // Barycentric coordinate of vertex 2.
    Vec3 normal;
};

struct Triangle {
    std::uint32_t i0 = 0;
    std::uint32_t i1 = 0;
    std::uint32_t i2 = 0;
    std::int32_t materialIndex = -1;
};

// One mesh. Vertices/triangles are copied during build() and may be freed afterwards.
struct Mesh {
    const Vec3* vertices = nullptr;
    std::uint32_t vertexCount = 0;
    const Triangle* triangles = nullptr;
    std::uint32_t triangleCount = 0;
    std::int32_t objectIndex = -1;
};

// Matches Steam Audio's IPLMaterial memory layout (3 bands).
struct Material {
    float absorption[3] = {0.0f, 0.0f, 0.0f};
    float scattering = 0.0f;
    float transmission[3] = {0.0f, 0.0f, 0.0f};
};

struct SceneDesc {
    const Mesh* meshes = nullptr;
    std::size_t meshCount = 0;
    const Material* materials = nullptr;
    std::size_t materialCount = 0;
};

enum class Status : std::int32_t {
    ok = 0,
    invalidArgument,
    unsupported,
    outOfMemory,
    deviceError,
    notBuilt,
};

const char* toString(Status status);

// Batched, synchronous ray tracer. Implementations may serialize concurrent calls.
class IRayTracer {
public:
    virtual ~IRayTracer() = default;

    virtual Status build(const SceneDesc& scene) = 0;

    // maxDistances[i] < minDistances[i] means the ray must be ignored.
    virtual Status traceClosest(const Ray* rays,
                                const float* minDistances,
                                const float* maxDistances,
                                std::size_t count,
                                Hit* hits) = 0;

    virtual Status traceAny(const Ray* rays,
                            const float* minDistances,
                            const float* maxDistances,
                            std::size_t count,
                            std::uint8_t* occluded) = 0;

    virtual const char* name() const = 0;
};

// Return nullptr when the platform or GPU does not support the backend.
std::unique_ptr<IRayTracer> createMetalTracer();
std::unique_ptr<IRayTracer> createVulkanTracer();

} // namespace rtcore
