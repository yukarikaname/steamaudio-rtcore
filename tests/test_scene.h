// Shared test scene: a 4x4 wall in the z=0 plane with one material.
#pragma once

#include "rtcore/rtcore.h"

#include <vector>

namespace testscene {

struct WallScene {
    std::vector<rtcore::Vec3> vertices;
    std::vector<rtcore::Triangle> triangles;
    std::vector<rtcore::Material> materials;
    rtcore::Mesh mesh;

    WallScene() {
        vertices = {
            {-2.0f, -2.0f, 0.0f},
            {2.0f, -2.0f, 0.0f},
            {2.0f, 2.0f, 0.0f},
            {-2.0f, 2.0f, 0.0f},
        };
        triangles = {
            {0, 1, 2, 0},
            {0, 2, 3, 0},
        };
        rtcore::Material material;
        material.absorption[0] = 0.2f;
        material.absorption[1] = 0.2f;
        material.absorption[2] = 0.2f;
        material.scattering = 0.1f;
        material.transmission[0] = 0.5f;
        material.transmission[1] = 0.5f;
        material.transmission[2] = 0.5f;
        materials = {material};

        mesh.vertices = vertices.data();
        mesh.vertexCount = static_cast<std::uint32_t>(vertices.size());
        mesh.triangles = triangles.data();
        mesh.triangleCount = static_cast<std::uint32_t>(triangles.size());
        mesh.objectIndex = 3;
    }

    rtcore::SceneDesc desc() const {
        rtcore::SceneDesc scene;
        scene.meshes = &mesh;
        scene.meshCount = 1;
        scene.materials = materials.data();
        scene.materialCount = materials.size();
        return scene;
    }
};

inline bool nearlyEqual(float a, float b, float tolerance = 1.0e-4f) {
    return (a > b ? a - b : b - a) <= tolerance;
}

} // namespace testscene
