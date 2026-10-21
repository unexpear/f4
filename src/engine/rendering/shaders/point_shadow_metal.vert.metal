#include <metal_stdlib>
using namespace metal;

// Point-light shadow face: see engine/lighting/point_shadows.h.
struct PointShadowUbo {
    float4x4 faceViewProj;
    float4x4 reserved; // unused: casters are instanced
    float4 lightPositionInvRange;
    float4 tileTransform;
    float4 alphaParams;
};

struct VertexInput {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(2)]];
    // Per-instance caster transform (same instance stream as the PBR pass).
    float4 model0 [[attribute(9)]];
    float4 model1 [[attribute(10)]];
    float4 model2 [[attribute(11)]];
    float4 model3 [[attribute(12)]];
};

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 faceClip;
    float2 uv;
    float4 light [[flat]];
    float3 alpha [[flat]];
};

vertex VertexOutput point_shadow_vertex(VertexInput input [[stage_in]],
                                        constant PointShadowUbo& scene [[buffer(16)]]) {
    VertexOutput output;
    const float4 world = input.model0 * input.position.x + input.model1 * input.position.y +
                         input.model2 * input.position.z + input.model3;
    const float4 clip = scene.faceViewProj * world;
    output.worldPosition = world.xyz;
    output.faceClip = clip.xyw;
    output.uv = input.uv;
    output.light = scene.lightPositionInvRange;
    output.alpha = scene.alphaParams.xyz;
    output.position = float4(clip.xy * scene.tileTransform.xy + scene.tileTransform.zw * clip.w,
                             clip.z, clip.w);
    return output;
}
