#include <metal_stdlib>
using namespace metal;

struct ShadowUbo {
    float4x4 lightViewProj;
    float4x4 model;
    float4 alphaParams;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(2)]];
};

struct VertexOutput {
    float4 position [[position]];
    float depth;
    float2 uv;
    float3 alpha [[flat]];
};

vertex VertexOutput shadow_vertex(VertexInput input [[stage_in]],
                                  constant ShadowUbo& scene [[buffer(16)]]) {
    VertexOutput output;
    output.position = scene.lightViewProj * scene.model * float4(input.position, 1.0f);
    output.depth = output.position.z / output.position.w;
    output.uv = input.uv;
    output.alpha = scene.alphaParams.xyz;
    return output;
}
