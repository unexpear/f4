#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 faceClip;
    float2 uv;
    float4 light [[flat]];
    float3 alpha [[flat]];
};

fragment float point_shadow_fragment(VertexOutput input [[stage_in]],
                                     texture2d<float> baseColorTexture [[texture(0)]],
                                     sampler baseColorSampler [[sampler(0)]]) {
    const float2 local = input.faceClip.xy / input.faceClip.z;
    if (abs(local.x) > 1.0f || abs(local.y) > 1.0f) discard_fragment();
    if (input.alpha.z > 0.5f &&
        baseColorTexture.sample(baseColorSampler, input.uv).a * input.alpha.x < input.alpha.y) {
        discard_fragment();
    }
    return clamp(length(input.worldPosition - input.light.xyz) * input.light.w, 0.0f, 1.0f);
}
