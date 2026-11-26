// Internal Vulkan backend types shared by vulkan_device.cpp and vulkan_tracer.cpp.
#pragma once

#include <vulkan/vulkan.h>

#include "rtcore/rtcore.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace rtcore {

struct VulkanGpuRay {
    float ox, oy, oz, tmin;
    float dx, dy, dz, tmax;
};

struct VulkanGpuHit {
    float distance;
    std::uint32_t primitive;
    float u, v;
};

struct VulkanPushConstants {
    std::uint32_t count;
    std::uint32_t anyHit;
};

struct VulkanTriangleRef {
    std::int32_t objectIndex;
    std::int32_t materialIndex;
    Vec3 normal;
};

struct VulkanBuffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
};

class VulkanTracer final : public IRayTracer {
public:
    VulkanTracer();
    ~VulkanTracer() override;

    bool valid() const { return valid_; }

    Status build(const SceneDesc& scene) override;
    Status traceClosest(const Ray* rays, const float* minDistances, const float* maxDistances,
                        std::size_t count, Hit* hits) override;
    Status traceAny(const Ray* rays, const float* minDistances, const float* maxDistances,
                    std::size_t count, std::uint8_t* occluded) override;
    const char* name() const override { return "vulkan-ray-query"; }

private:
    bool initialize();
    void shutdown();

    bool createInstance();
    bool pickDevice();
    bool createDevice();
    bool loadFunctions();
    bool createPipeline();
    bool createBuffers(std::size_t maxRays);

    Status createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, VulkanBuffer& out);
    void destroyBuffer(VulkanBuffer& buffer);
    Status ensureCapacity(std::size_t count);
    bool execute(const std::function<void(VkCommandBuffer)>& record);
    Status trace(const Ray* rays, const float* minDistances, const float* maxDistances,
                 std::size_t count, VulkanGpuHit* outHits, std::uint8_t* occluded);
    void updateDescriptors();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    PFN_vkCreateAccelerationStructureKHR createAS_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAS_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAS_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getASAddress_ = nullptr;

    VulkanBuffer vertices_;
    VulkanBuffer indices_;
    VulkanBuffer rays_;
    VulkanBuffer hits_;
    VulkanBuffer scratch_;
    VulkanBuffer accelStorage_;
    VulkanBuffer instances_;
    VulkanBuffer tlasScratch_;
    VulkanBuffer tlasStorage_;
    VkAccelerationStructureKHR accel_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;

    std::vector<VulkanTriangleRef> triangles_;
    std::size_t capacity_ = 0;
    std::mutex mutex_;
    bool built_ = false;
    bool valid_ = false;
};

} // namespace rtcore
