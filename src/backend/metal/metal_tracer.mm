#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ray_query_msl.h"
#include "rtcore/rtcore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace rtcore {
namespace {

struct GpuRay {
    float ox, oy, oz, tmin;
    float dx, dy, dz, tmax;
};

struct GpuHit {
    float distance;
    std::uint32_t primitive;
    float u, v;
};

struct GpuParams {
    std::uint32_t count;
    std::uint32_t anyHit;
};

struct TriangleRef {
    std::int32_t objectIndex;
    std::int32_t materialIndex;
    std::uint32_t i0, i1, i2;
    Vec3 normal;
};

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

class MetalTracer final : public IRayTracer {
public:
    explicit MetalTracer(id<MTLDevice> device)
        : device_(device) {
        valid_ = initialize();
    }

    ~MetalTracer() override = default;

    bool valid() const { return valid_; }

    Status build(const SceneDesc& scene) override {
        if (!valid_) {
            return Status::unsupported;
        }
        if (!scene.meshes || scene.meshCount == 0) {
            return Status::invalidArgument;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<Vec3> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<TriangleRef> triangles;

        for (std::size_t m = 0; m < scene.meshCount; ++m) {
            const Mesh& mesh = scene.meshes[m];
            if (!mesh.vertices || !mesh.triangles || mesh.vertexCount == 0 || mesh.triangleCount == 0) {
                return Status::invalidArgument;
            }

            const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
            vertices.insert(vertices.end(), mesh.vertices, mesh.vertices + mesh.vertexCount);

            for (std::uint32_t t = 0; t < mesh.triangleCount; ++t) {
                const Triangle& tri = mesh.triangles[t];
                if (tri.i0 >= mesh.vertexCount || tri.i1 >= mesh.vertexCount || tri.i2 >= mesh.vertexCount) {
                    return Status::invalidArgument;
                }

                TriangleRef ref;
                ref.objectIndex = mesh.objectIndex;
                ref.materialIndex = tri.materialIndex;
                ref.i0 = baseVertex + tri.i0;
                ref.i1 = baseVertex + tri.i1;
                ref.i2 = baseVertex + tri.i2;
                const Vec3& a = vertices[ref.i0];
                const Vec3& b = vertices[ref.i1];
                const Vec3& c = vertices[ref.i2];
                ref.normal = normalize(cross(subtract(b, a), subtract(c, a)));
                triangles.push_back(ref);

                indices.push_back(ref.i0);
                indices.push_back(ref.i1);
                indices.push_back(ref.i2);
            }
        }

        if (indices.empty()) {
            return Status::invalidArgument;
        }

        id<MTLBuffer> vertexBuffer = [device_ newBufferWithBytes:vertices.data()
                                                          length:vertices.size() * sizeof(Vec3)
                                                         options:MTLResourceStorageModeShared];
        id<MTLBuffer> indexBuffer = [device_ newBufferWithBytes:indices.data()
                                                         length:indices.size() * sizeof(std::uint32_t)
                                                        options:MTLResourceStorageModeShared];
        if (!vertexBuffer || !indexBuffer) {
            return Status::outOfMemory;
        }

        MTL4PrimitiveAccelerationStructureDescriptor* descriptor =
            [MTL4PrimitiveAccelerationStructureDescriptor new];
        MTL4AccelerationStructureTriangleGeometryDescriptor* geometry =
            [MTL4AccelerationStructureTriangleGeometryDescriptor new];
        geometry.vertexBuffer = MTL4BufferRange(vertexBuffer.gpuAddress, vertexBuffer.length);
        geometry.vertexFormat = MTLAttributeFormatFloat3;
        geometry.vertexStride = sizeof(Vec3);
        geometry.indexBuffer = MTL4BufferRange(indexBuffer.gpuAddress, indexBuffer.length);
        geometry.indexType = MTLIndexTypeUInt32;
        geometry.triangleCount = triangles.size();
        geometry.opaque = YES;
        descriptor.geometryDescriptors = @[ geometry ];

        MTLAccelerationStructureSizes sizes = [device_ accelerationStructureSizesWithDescriptor:descriptor];
        if (sizes.accelerationStructureSize == 0) {
            return Status::unsupported;
        }

        id<MTLAccelerationStructure> accel = [device_ newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        id<MTLBuffer> scratch = [device_ newBufferWithLength:std::max<NSUInteger>(sizes.buildScratchBufferSize, 16)
                                                     options:MTLResourceStorageModePrivate];
        if (!accel || !scratch) {
            return Status::outOfMemory;
        }

        if (vertexBuffer_) {
            [residency_ removeAllocation:vertexBuffer_];
        }
        if (indexBuffer_) {
            [residency_ removeAllocation:indexBuffer_];
        }
        if (accel_) {
            [residency_ removeAllocation:accel_];
        }
        if (scratch_) {
            [residency_ removeAllocation:scratch_];
        }
        [residency_ addAllocation:vertexBuffer];
        [residency_ addAllocation:indexBuffer];
        [residency_ addAllocation:accel];
        [residency_ addAllocation:scratch];
        [residency_ commit];
        [residency_ requestResidency];

        if (!encodeAndWait(^(id<MTL4ComputeCommandEncoder> encoder) {
                [encoder buildAccelerationStructure:accel
                                        descriptor:descriptor
                                     scratchBuffer:MTL4BufferRange(scratch.gpuAddress, scratch.length)];
            })) {
            return Status::deviceError;
        }

        vertexBuffer_ = vertexBuffer;
        indexBuffer_ = indexBuffer;
        scratch_ = scratch;
        accel_ = accel;
        triangles_ = std::move(triangles);
        built_ = true;

        [argumentTable_ setResource:accel_.gpuResourceID atBufferIndex:0];
        updateArgumentTable();
        return Status::ok;
    }

    Status traceClosest(const Ray* rays, const float* minDistances, const float* maxDistances,
                        std::size_t count, Hit* hits) override {
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

        std::vector<GpuHit> gpuHits(count);
        Status status = trace(rays, minDistances, maxDistances, count, gpuHits.data(), nullptr);
        if (status != Status::ok) {
            return status;
        }

        for (std::size_t i = 0; i < count; ++i) {
            const GpuHit& gpu = gpuHits[i];
            Hit& out = hits[i];
            out = Hit{};
            if (gpu.distance >= std::numeric_limits<float>::infinity() ||
                gpu.primitive >= triangles_.size()) {
                continue;
            }
            const TriangleRef& tri = triangles_[gpu.primitive];
            out.distance = gpu.distance;
            out.triangleIndex = static_cast<std::int32_t>(gpu.primitive);
            out.objectIndex = tri.objectIndex;
            out.materialIndex = tri.materialIndex;
            out.u = gpu.u;
            out.v = gpu.v;
            out.normal = tri.normal;
        }
        return Status::ok;
    }

    Status traceAny(const Ray* rays, const float* minDistances, const float* maxDistances,
                    std::size_t count, std::uint8_t* occluded) override {
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

        std::vector<GpuHit> gpuHits(count);
        Status status = trace(rays, minDistances, maxDistances, count, gpuHits.data(), occluded);
        return status;
    }

    const char* name() const override { return "metal4-ray-query"; }

private:
    bool initialize() {
        NSError* error = nil;

        MTL4CompilerDescriptor* compilerDescriptor = [MTL4CompilerDescriptor new];
        compiler_ = [device_ newCompilerWithDescriptor:compilerDescriptor error:&error];
        if (!compiler_) {
            return false;
        }

        MTL4LibraryDescriptor* libraryDescriptor = [MTL4LibraryDescriptor new];
        libraryDescriptor.source = [NSString stringWithUTF8String:kMetalRayQuerySource];
        id<MTLLibrary> library = [compiler_ newLibraryWithDescriptor:libraryDescriptor error:&error];
        if (!library) {
            NSLog(@"rtcore: Metal library compilation failed: %@", error);
            return false;
        }

        MTL4LibraryFunctionDescriptor* function = [MTL4LibraryFunctionDescriptor new];
        function.library = library;
        function.name = @"rtcore_trace";

        MTL4ComputePipelineDescriptor* pipelineDescriptor = [MTL4ComputePipelineDescriptor new];
        pipelineDescriptor.computeFunctionDescriptor = function;
        pipeline_ = [compiler_ newComputePipelineStateWithDescriptor:pipelineDescriptor
                                                 compilerTaskOptions:nil
                                                               error:&error];
        if (!pipeline_) {
            NSLog(@"rtcore: Metal pipeline creation failed: %@", error);
            return false;
        }

        queue_ = [device_ newMTL4CommandQueue];
        allocator_ = [device_ newCommandAllocator];
        commandBuffer_ = [device_ newCommandBuffer];
        event_ = [device_ newSharedEvent];
        if (!queue_ || !allocator_ || !commandBuffer_ || !event_) {
            return false;
        }

        MTL4ArgumentTableDescriptor* tableDescriptor = [MTL4ArgumentTableDescriptor new];
        tableDescriptor.maxBufferBindCount = 8;
        tableDescriptor.initializeBindings = YES;
        argumentTable_ = [device_ newArgumentTableWithDescriptor:tableDescriptor error:&error];
        if (!argumentTable_) {
            return false;
        }

        MTLResidencySetDescriptor* residencyDescriptor = [MTLResidencySetDescriptor new];
        residencyDescriptor.initialCapacity = 32;
        residency_ = [device_ newResidencySetWithDescriptor:residencyDescriptor error:&error];
        if (!residency_) {
            return false;
        }
        [queue_ addResidencySet:residency_];
        [residency_ requestResidency];

        params_ = [device_ newBufferWithLength:sizeof(GpuParams) options:MTLResourceStorageModeShared];
        if (!params_) {
            return false;
        }
        ensureRayCapacity(64);
        return true;
    }

    bool ensureRayCapacity(std::size_t count) {
        const NSUInteger rayBytes = static_cast<NSUInteger>(count * sizeof(GpuRay));
        const NSUInteger hitBytes = static_cast<NSUInteger>(count * sizeof(GpuHit));

        bool changed = false;
        if (!rays_ || rays_.length < rayBytes) {
            if (rays_) {
                [residency_ removeAllocation:rays_];
            }
            rays_ = [device_ newBufferWithLength:std::max<NSUInteger>(rayBytes, 16)
                                         options:MTLResourceStorageModeShared];
            changed = true;
        }
        if (!hits_ || hits_.length < hitBytes) {
            if (hits_) {
                [residency_ removeAllocation:hits_];
            }
            hits_ = [device_ newBufferWithLength:std::max<NSUInteger>(hitBytes, 16)
                                         options:MTLResourceStorageModeShared];
            changed = true;
        }
        if (!rays_ || !hits_) {
            return false;
        }

        if (changed) {
            [residency_ addAllocation:rays_];
            [residency_ addAllocation:hits_];
            [residency_ addAllocation:params_];
            [residency_ commit];
            [residency_ requestResidency];
            updateArgumentTable();
        }
        return true;
    }

    void updateArgumentTable() {
        if (!argumentTable_ || !rays_ || !hits_ || !params_) {
            return;
        }
        [argumentTable_ setAddress:rays_.gpuAddress atIndex:1];
        [argumentTable_ setAddress:hits_.gpuAddress atIndex:2];
        [argumentTable_ setAddress:params_.gpuAddress atIndex:3];
    }

    bool encodeAndWait(void (^encode)(id<MTL4ComputeCommandEncoder>)) {
        [allocator_ reset];
        [commandBuffer_ beginCommandBufferWithAllocator:allocator_];
        [commandBuffer_ useResidencySet:residency_];
        id<MTL4ComputeCommandEncoder> encoder = [commandBuffer_ computeCommandEncoder];
        if (!encoder) {
            [commandBuffer_ endCommandBuffer];
            return false;
        }
        encode(encoder);
        [encoder endEncoding];
        [commandBuffer_ endCommandBuffer];

        id<MTL4CommandBuffer> buffers[] = { commandBuffer_ };
        [queue_ commit:buffers count:1];
        const std::uint64_t value = ++eventValue_;
        [queue_ signalEvent:event_ value:value];
        return [event_ waitUntilSignaledValue:value timeoutMS:10000];
    }

    Status trace(const Ray* rays, const float* minDistances, const float* maxDistances,
                 std::size_t count, GpuHit* outHits, std::uint8_t* occluded) {
        if (!ensureRayCapacity(count)) {
            return Status::outOfMemory;
        }

        auto* gpuRays = static_cast<GpuRay*>(rays_.contents);
        auto* gpuHits = static_cast<GpuHit*>(hits_.contents);
        for (std::size_t i = 0; i < count; ++i) {
            gpuRays[i] = {rays[i].origin.x, rays[i].origin.y, rays[i].origin.z, minDistances[i],
                          rays[i].direction.x, rays[i].direction.y, rays[i].direction.z, maxDistances[i]};
            gpuHits[i] = {std::numeric_limits<float>::infinity(), 0xFFFFFFFFu, 0.0f, 0.0f};
        }

        auto* params = static_cast<GpuParams*>(params_.contents);
        params->count = static_cast<std::uint32_t>(count);
        params->anyHit = occluded ? 1u : 0u;

        const bool submitted = encodeAndWait(^(id<MTL4ComputeCommandEncoder> encoder) {
            [encoder setComputePipelineState:pipeline_];
            [encoder setArgumentTable:argumentTable_];
            const NSUInteger threadgroup = std::min<NSUInteger>(64, pipeline_.maxTotalThreadsPerThreadgroup);
            [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(threadgroup, 1, 1)];
        });
        if (!submitted) {
            return Status::deviceError;
        }

        if (occluded) {
            for (std::size_t i = 0; i < count; ++i) {
                const float maxDistance = maxDistances[i];
                const float minDistance = minDistances[i];
                occluded[i] = (maxDistance >= minDistance &&
                               gpuHits[i].distance < std::numeric_limits<float>::infinity())
                                  ? 1
                                  : 0;
            }
        } else {
            std::memcpy(outHits, gpuHits, count * sizeof(GpuHit));
        }
        return Status::ok;
    }

    id<MTLDevice> device_ = nil;
    id<MTL4Compiler> compiler_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    id<MTL4CommandQueue> queue_ = nil;
    id<MTL4CommandAllocator> allocator_ = nil;
    id<MTL4CommandBuffer> commandBuffer_ = nil;
    id<MTLSharedEvent> event_ = nil;
    id<MTL4ArgumentTable> argumentTable_ = nil;
    id<MTLResidencySet> residency_ = nil;

    id<MTLBuffer> params_ = nil;
    id<MTLBuffer> rays_ = nil;
    id<MTLBuffer> hits_ = nil;
    id<MTLBuffer> vertexBuffer_ = nil;
    id<MTLBuffer> indexBuffer_ = nil;
    id<MTLBuffer> scratch_ = nil;
    id<MTLAccelerationStructure> accel_ = nil;

    std::vector<TriangleRef> triangles_;
    std::mutex mutex_;
    std::uint64_t eventValue_ = 0;
    bool built_ = false;
    bool valid_ = false;
};

} // namespace

std::unique_ptr<IRayTracer> createMetalTracer() {
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            return nullptr;
        }
        auto tracer = std::make_unique<MetalTracer>(device);
        if (!tracer->valid()) {
            return nullptr;
        }
        return tracer;
    }
    return nullptr;
}

} // namespace rtcore
