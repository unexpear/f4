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
    float4 model0 [[attribute(9)]];
    float4 model1 [[attribute(10)]];
    float4 model2 [[attribute(11)]];
    float4 model3 [[attribute(12)]];
    float4 normal0 [[attribute(13)]];
    float4 normal1 [[attribute(14)]];
    float4 normal2 [[attribute(15)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 worldNormal;
    float2 uv;
    float4 color;
    float4 lightClip;
};

vertex VertexOutput pbr_instanced_vertex(
    VertexInput input [[stage_in]], constant SceneUbo& scene [[buffer(16)]]) {
    VertexOutput output;
    const float4 world = input.model0 * input.position.x +
                         input.model1 * input.position.y +
                         input.model2 * input.position.z + input.model3;
    output.position = scene.viewProj * world;
    output.worldPosition = world.xyz;
    output.worldNormal = input.normal0.xyz * input.normal.x +
                         input.normal1.xyz * input.normal.y +
                         input.normal2.xyz * input.normal.z;
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = scene.lightViewProj * world;
    return output;
}
