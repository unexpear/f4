#include <metal_stdlib>
using namespace metal;

struct CubeUniforms {
    float4x4 viewProj;
    float4x4 model;
    float4 lightDir;
    float4 viewPos;
};

struct CubeVertexInput {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 faceColor [[attribute(2)]];
};

struct CubeVertexOutput {
    float4 position [[position]];
    float3 normal;
    float3 worldPosition;
    float3 albedo;
};

vertex CubeVertexOutput cube_vertex(
    CubeVertexInput input [[stage_in]],
    constant CubeUniforms& uniforms [[buffer(16)]]) {
    CubeVertexOutput output;
    const float4 worldPosition = uniforms.model * float4(input.position, 1.0f);
    output.position = uniforms.viewProj * worldPosition;
    output.normal = float3x3(uniforms.model[0].xyz, uniforms.model[1].xyz,
                             uniforms.model[2].xyz) * input.normal;
    output.worldPosition = worldPosition.xyz;
    output.albedo = float3(input.faceColor,
                           0.5f + 0.4f * sin(input.faceColor.x * 6.28f +
                                             input.faceColor.y));
    return output;
}
