// Metal Shading Language source for the Metal 4 ray-query backend.
// Compiled at runtime by MTL4Compiler (no offline metallib step).
#pragma once

inline constexpr const char kMetalRayQuerySource[] = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

struct RTRay {
    packed_float3 origin;
    float tmin;
    packed_float3 direction;
    float tmax;
};

struct RTHit {
    float distance;
    uint primitive;
    float2 barycentrics;
};

struct RTParams {
    uint count;
    uint anyHit;
};

kernel void rtcore_trace(primitive_acceleration_structure accel [[buffer(0)]],
                         device const RTRay* rays [[buffer(1)]],
                         device RTHit* hits [[buffer(2)]],
                         constant RTParams& params [[buffer(3)]],
                         uint tid [[thread_position_in_grid]])
{
    if (tid >= params.count) {
        return;
    }

    RTRay input = rays[tid];
    RTHit output;
    output.distance = INFINITY;
    output.primitive = 0xFFFFFFFFu;
    output.barycentrics = float2(0.0f, 0.0f);

    if (input.tmax >= input.tmin) {
        intersection_query<triangle_data> query;
        intersection_params options;
        options.accept_any_intersection(params.anyHit != 0u);

        ray r(float3(input.origin), float3(input.direction), input.tmin, input.tmax);
        query.reset(r, accel, options);

        while (query.next()) {
        }

        if (query.get_committed_intersection_type() != intersection_type::none) {
            output.distance = query.get_committed_distance();
            output.primitive = query.get_committed_primitive_id();
            output.barycentrics = query.get_committed_triangle_barycentric_coord();
        }
    }

    hits[tid] = output;
}
)MSL";
