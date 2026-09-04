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
    float4 color [[attribute(2)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 normal;
    float3 worldPosition;
    float4 color;
};

float3x3 normalMatrix(float4x4 model) {
    const float3 a = model[0].xyz;
    const float3 b = model[1].xyz;
    const float3 c = model[2].xyz;
    const float inverseDeterminant = 1.0f / dot(a, cross(b, c));
    return float3x3(cross(b, c) * inverseDeterminant,
                    cross(c, a) * inverseDeterminant,
                    cross(a, b) * inverseDeterminant);
}

vertex VertexOutput validation_cornell_vertex(
    VertexInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    const float4 world = scene.model * float4(input.position, 1.0f);
    VertexOutput output;
    output.position = scene.viewProj * world;
    output.normal = normalMatrix(scene.model) * input.normal;
    output.worldPosition = world.xyz;
    output.color = input.color;
    return output;
}
