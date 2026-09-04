#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 position [[position]];
    float2 uv;
};

fragment float4 validation_normal_map_fragment(
    FragmentInput input [[stage_in]],
    texture2d<float> normalTexture [[texture(0)]],
    sampler normalSampler [[sampler(0)]]) {
    const bool flipped = input.uv.x >= 0.5f;
    const float2 panelUv(fract(input.uv.x * 2.0f), input.uv.y);
    const float2 tiledUv = panelUv * 4.0f;
    float3 tangentNormal = normalTexture.sample(normalSampler, tiledUv).xyz * 2.0f - 1.0f;
    if (flipped) tangentNormal.y = -tangentNormal.y;
    const float3 normal = normalize(tangentNormal);
    const float3 lightDirection = normalize(float3(-0.45f, 0.75f, 1.0f));
    const float3 viewDirection(0.0f, 0.0f, 1.0f);
    const float diffuse = max(dot(normal, lightDirection), 0.0f);
    const float specular = pow(max(dot(reflect(-lightDirection, normal), viewDirection),
                                   0.0f), 32.0f);
    const float stripe = 0.5f + 0.5f * sin(tiledUv.x * 3.14159f);
    const float3 base = mix(float3(0.07f, 0.24f, 0.42f),
                            float3(0.12f, 0.55f, 0.68f), stripe);
    float3 color = base * (0.16f + 0.84f * diffuse) + float3(0.7f) * specular;
    if (input.uv.y > 0.92f) {
        color = flipped ? float3(0.86f, 0.2f, 0.1f)
                        : float3(0.08f, 0.68f, 0.35f);
    }
    if (abs(input.uv.x - 0.5f) < 0.003f) color = float3(0.95f);
    return float4(pow(color, float3(1.0f / 2.2f)), 1.0f);
}
