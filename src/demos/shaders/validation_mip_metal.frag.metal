#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 position [[position]];
    float2 uv;
};

fragment float4 validation_mip_fragment(
    FragmentInput input [[stage_in]],
    texture2d<float> levelZeroTexture [[texture(0)]],
    sampler levelZeroSampler [[sampler(0)]],
    texture2d<float> mipTexture [[texture(1)]],
    sampler mipSampler [[sampler(1)]]) [[color(0)]] {
    const bool mipmapped = input.uv.x >= 0.5f;
    const float localX = fract(input.uv.x * 2.0f) * 2.0f - 1.0f;
    const float horizonDistance = max(0.875f - input.uv.y, 0.012f);
    const float perspective = 1.0f / horizonDistance;
    const float2 checkerUv(localX * perspective * 2.0f, perspective * 3.0f);
    const float3 levelZero = levelZeroTexture.sample(levelZeroSampler, checkerUv).rgb;
    const float3 filtered = mipTexture.sample(mipSampler, checkerUv).rgb;
    if (input.uv.y > 0.86f) {
        return float4(mipmapped ? float3(0.08f, 0.65f, 0.32f)
                               : float3(0.85f, 0.18f, 0.08f), 1.0f);
    }
    const float fog = smoothstep(8.0f, 65.0f, perspective);
    float3 color = mix(mipmapped ? filtered : levelZero,
                       float3(0.16f, 0.19f, 0.22f), fog * 0.55f);
    if (abs(input.uv.x - 0.5f) < 0.003f) color = float3(0.95f);
    return float4(color, 1.0f);
}
