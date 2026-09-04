#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 position [[position]];
    float2 uv;
};

fragment float4 validation_deferred_fragment(
    FragmentInput input [[stage_in]],
    texture2d<float> albedoTexture [[texture(0)]],
    sampler albedoSampler [[sampler(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    sampler normalSampler [[sampler(1)]]) [[color(0)]] {
    const float4 albedoSample = albedoTexture.sample(albedoSampler, input.uv);
    const float3 encodedNormal = normalTexture.sample(normalSampler, input.uv).xyz;
    if (albedoSample.a < 0.5f) {
        const float3 low(0.018f, 0.032f, 0.065f);
        const float3 high(0.11f, 0.19f, 0.28f);
        return float4(mix(low, high, input.uv.y), 1.0f);
    }
    const float3 normal = normalize(encodedNormal * 2.0f - 1.0f);
    const float3 lightDirection = normalize(float3(-0.45f, 0.8f, 0.55f));
    const float diffuse = max(dot(normal, lightDirection), 0.0f);
    const float rim = pow(1.0f - max(normal.z, 0.0f), 2.0f);
    const float3 color = albedoSample.rgb * (0.16f + 0.84f * diffuse) +
                         float3(0.12f) * rim;
    return float4(pow(color, float3(1.0f / 2.2f)), 1.0f);
}
