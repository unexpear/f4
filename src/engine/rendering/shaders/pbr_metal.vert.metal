#include <metal_stdlib>
using namespace metal;

struct SceneUbo {
    float4x4 viewProj;
    float4x4 model;
    float4x4 normalMatrix;
    float4x4 lightViewProj;
    float4 baseColorFactor;
    float4 emissiveMetallic;
    float4 cameraExposure;
    float4 lightDirectionIntensity;
    float4 lightColorRoughness;
    float4 materialParams;
    float4 renderParams;
    float4 ambientFlags;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 color [[attribute(3)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 worldNormal;
    float2 uv;
    float4 color;
    float4 lightClip;
};

vertex VertexOutput pbr_vertex(VertexInput input [[stage_in]],
                               constant SceneUbo& scene [[buffer(16)]]) {
    VertexOutput output;
    const float4 world = scene.model * float4(input.position, 1.0f);
    const float3x3 normalMatrix(scene.normalMatrix[0].xyz,
                                scene.normalMatrix[1].xyz,
                                scene.normalMatrix[2].xyz);
    output.position = scene.viewProj * world;
    output.worldPosition = world.xyz;
    output.worldNormal = normalMatrix * input.normal;
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = scene.lightViewProj * world;
    return output;
}
