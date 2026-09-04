#include <metal_stdlib>
using namespace metal;

struct Scene {
    float4x4 viewProj;
    float4 cameraPos;
    float4 lightPos;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float3 offset [[attribute(2)]];
    float3 material [[attribute(3)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 normal;
    float3 material;
};

vertex VertexOutput validation_pbr_vertex(
    VertexInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    VertexOutput output;
    output.worldPosition = input.position + input.offset;
    output.normal = input.normal;
    output.material = input.material;
    output.position = scene.viewProj * float4(output.worldPosition, 1.0f);
    return output;
}
