#include "vulkan_tracer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace rtcore {
namespace {

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(const Vec3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length <= 1.0e-20f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / length;
    return {v.x * inv, v.y * inv, v.z * inv};
}

} // namespace

Status VulkanTracer::build(const SceneDesc& scene) {
    if (!valid_) {
        return Status::unsupported;
    }
    if (!scene.meshes || scene.meshCount == 0) {
        return Status::invalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<VulkanTriangleRef> triangles;
    for (std::size_t m = 0; m < scene.meshCount; ++m) {
        const Mesh& mesh = scene.meshes[m];
        if (!mesh.vertices || !mesh.triangles || mesh.vertexCount == 0 || mesh.triangleCount == 0) {
            return Status::invalidArgument;
        }
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), mesh.vertices, mesh.vertices + mesh.vertexCount);
        for (std::uint32_t t = 0; t < mesh.triangleCount; ++t) {
            const Triangle& tri = mesh.triangles[t];
            indices.push_back(base + tri.i0);
            indices.push_back(base + tri.i1);
            indices.push_back(base + tri.i2);
            const Vec3 normal = normalize(cross(subtract(vertices[base + tri.i1], vertices[base + tri.i0]),
                                                subtract(vertices[base + tri.i2], vertices[base + tri.i0])));
            triangles.push_back({mesh.objectIndex, tri.materialIndex, normal});
        }
    }

    destroyBuffer(vertices_);
    destroyBuffer(indices_);
    if (accel_ != VK_NULL_HANDLE) {
        destroyAS_(device_, accel_, nullptr);
        accel_ = VK_NULL_HANDLE;
    }
    destroyBuffer(accelStorage_);
    destroyBuffer(scratch_);

    const VkBufferUsageFlags inputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (createBuffer(vertices.size() * sizeof(Vec3), inputUsage, true, vertices_) != Status::ok ||
        createBuffer(indices.size() * sizeof(std::uint32_t), inputUsage, true, indices_) != Status::ok) {
        return Status::outOfMemory;
    }
    std::memcpy(vertices_.mapped, vertices.data(), vertices_.size);
    std::memcpy(indices_.mapped, indices.data(), indices_.size);

    VkAccelerationStructureGeometryTrianglesDataKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geometry.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.vertexData.deviceAddress = vertices_.address;
    geometry.vertexStride = sizeof(Vec3);
    geometry.maxVertex = static_cast<std::uint32_t>(vertices.size()) - 1;
    geometry.indexType = VK_INDEX_TYPE_UINT32;
    geometry.indexData.deviceAddress = indices_.address;
    geometry.transformData.deviceAddress = 0;

    VkAccelerationStructureGeometryKHR geometryInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometryInfo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometryInfo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometryInfo.geometry.triangles = geometry;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometryInfo;

    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(triangles.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    buildSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo, &primitiveCount, &sizes);

    const VkBufferUsageFlags storageUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (createBuffer(sizes.accelerationStructureSize, storageUsage, false, accelStorage_) != Status::ok ||
        createBuffer(sizes.buildScratchSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     false, scratch_) != Status::ok) {
        return Status::outOfMemory;
    }

    VkAccelerationStructureCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    createInfo.buffer = accelStorage_.handle;
    createInfo.offset = 0;
    createInfo.size = sizes.accelerationStructureSize;
    if (createAS_(device_, &createInfo, nullptr, &accel_) != VK_SUCCESS) {
        return Status::deviceError;
    }

    buildInfo.dstAccelerationStructure = accel_;
    buildInfo.scratchData.deviceAddress = scratch_.address;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

    const bool submitted = execute([&](VkCommandBuffer cmd) {
        VkMemoryBarrier toBuild{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        toBuild.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        toBuild.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             0, 1, &toBuild, 0, nullptr, 0, nullptr);

        buildAS_(cmd, 1, &buildInfo, ranges);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    });
    if (!submitted) {
        return Status::deviceError;
    }

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &accel_;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.pNext = &asWrite;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    triangles_ = std::move(triangles);
    built_ = true;
    return Status::ok;
}

Status VulkanTracer::ensureCapacity(std::size_t count) {
    if (count <= capacity_) {
        return Status::ok;
    }
    destroyBuffer(rays_);
    destroyBuffer(hits_);
    capacity_ = std::max<std::size_t>(count, capacity_ * 2);
    if (createBuffers(capacity_)) {
        return Status::ok;
    }
    capacity_ = 0;
    return Status::outOfMemory;
}

Status VulkanTracer::trace(const Ray* rays, const float* minDistances, const float* maxDistances,
                           std::size_t count, VulkanGpuHit* outHits, std::uint8_t* occluded) {
    Status status = ensureCapacity(count);
    if (status != Status::ok) {
        return status;
    }

    if (!rays_.mapped || !hits_.mapped) {
        return Status::deviceError;
    }

    auto* gpuRays = static_cast<VulkanGpuRay*>(rays_.mapped);
    auto* gpuHits = static_cast<VulkanGpuHit*>(hits_.mapped);
    for (std::size_t i = 0; i < count; ++i) {
        gpuRays[i] = {rays[i].origin.x, rays[i].origin.y, rays[i].origin.z, minDistances[i],
                      rays[i].direction.x, rays[i].direction.y, rays[i].direction.z, maxDistances[i]};
        gpuHits[i] = {std::numeric_limits<float>::infinity(), 0xFFFFFFFFu, 0.0f, 0.0f};
    }

    VulkanPushConstants push{static_cast<std::uint32_t>(count), occluded ? 1u : 0u};
    const bool submitted = execute([&](VkCommandBuffer cmd) {
        VkMemoryBarrier toShader{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        toShader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &toShader, 0, nullptr, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0, 1, &descriptorSet_, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, static_cast<std::uint32_t>((count + 63) / 64), 1, 1);

        VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &toHost, 0, nullptr, 0, nullptr);
    });
    if (!submitted) {
        return Status::deviceError;
    }

    if (occluded) {
        for (std::size_t i = 0; i < count; ++i) {
            occluded[i] = (maxDistances[i] >= minDistances[i] &&
                           gpuHits[i].distance < std::numeric_limits<float>::infinity()) ? 1 : 0;
        }
    } else {
        std::memcpy(outHits, gpuHits, count * sizeof(VulkanGpuHit));
    }
    return Status::ok;
}

Status VulkanTracer::traceClosest(const Ray* rays, const float* minDistances, const float* maxDistances,
                                  std::size_t count, Hit* hits) {
    if (!rays || !minDistances || !maxDistances || !hits) {
        return Status::invalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!built_) {
        return Status::notBuilt;
    }
    if (count == 0) {
        return Status::ok;
    }

    std::vector<VulkanGpuHit> gpuHits(count);
    Status status = trace(rays, minDistances, maxDistances, count, gpuHits.data(), nullptr);
    if (status != Status::ok) {
        return status;
    }
    for (std::size_t i = 0; i < count; ++i) {
        hits[i] = Hit{};
        if (gpuHits[i].distance >= std::numeric_limits<float>::infinity() ||
            gpuHits[i].primitive >= triangles_.size()) {
            continue;
        }
        hits[i].distance = gpuHits[i].distance;
        hits[i].triangleIndex = static_cast<std::int32_t>(gpuHits[i].primitive);
        hits[i].objectIndex = triangles_[gpuHits[i].primitive].objectIndex;
        hits[i].materialIndex = triangles_[gpuHits[i].primitive].materialIndex;
        hits[i].u = gpuHits[i].u;
        hits[i].v = gpuHits[i].v;
        hits[i].normal = triangles_[gpuHits[i].primitive].normal;
    }
    return Status::ok;
}

Status VulkanTracer::traceAny(const Ray* rays, const float* minDistances, const float* maxDistances,
                              std::size_t count, std::uint8_t* occluded) {
    if (!rays || !minDistances || !maxDistances || !occluded) {
        return Status::invalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!built_) {
        return Status::notBuilt;
    }
    if (count == 0) {
        return Status::ok;
    }
    std::vector<VulkanGpuHit> gpuHits(count);
    return trace(rays, minDistances, maxDistances, count, gpuHits.data(), occluded);
}

std::unique_ptr<IRayTracer> createVulkanTracer() {
    auto tracer = std::make_unique<VulkanTracer>();
    if (!tracer->valid()) {
        return nullptr;
    }
    return tracer;
}

} // namespace rtcore
