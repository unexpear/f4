#include <metal_stdlib>
using namespace metal;

struct ObjectData {
    float4x4 viewProj;
    float4x4 model;
    float4 albedo;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 normal;
    float4 albedo;
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

vertex VertexOutput validation_gbuffer_vertex(
    VertexInput input [[stage_in]], constant ObjectData& object [[buffer(16)]]) {
    const float4 world = object.model * float4(input.position, 1.0f);
    VertexOutput output;
    output.position = object.viewProj * world;
    output.normal = normalMatrix(object.model) * input.normal;
    output.albedo = object.albedo;
    return output;
}
