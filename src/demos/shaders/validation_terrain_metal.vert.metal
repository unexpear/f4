#include <metal_stdlib>
using namespace metal;

struct Scene {
    float4x4 viewProj;
    float4x4 model;
    float3 lightDir;
    float3 viewPos;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float4 color [[attribute(3)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 color;
    float3 normal;
    float3 worldPosition;
};

vertex VertexOutput validation_terrain_vertex(
    VertexInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    const float4 world = scene.model * float4(input.position, 1.0f);
    VertexOutput output;
    output.position = scene.viewProj * world;
    output.color = input.color.rgb;
    output.normal = normalize(float3x3(scene.model[0].xyz, scene.model[1].xyz,
                                      scene.model[2].xyz) * input.normal);
    output.worldPosition = world.xyz;
    return output;
}
