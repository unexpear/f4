#include <metal_stdlib>
using namespace metal;

struct Scene {
    float4x4 viewProj;
    float4x4 model;
    float3 lightDir;
    float3 viewPos;
};

struct FragmentInput {
    float4 position [[position]];
    float3 normal;
    float3 worldPosition;
    float4 color;
};

fragment float4 validation_cornell_fragment(
    FragmentInput input [[stage_in]], constant Scene& scene [[buffer(16)]])
    [[color(0)]] {
    const float3 normal = normalize(input.normal);
    const float3 light = normalize(-scene.lightDir);
    const float3 view = normalize(scene.viewPos - input.worldPosition);
    const float diffuse = max(dot(normal, light), 0.0f);
    const float3 halfVector = normalize(light + view);
    const float specular = pow(max(dot(normal, halfVector), 0.0f), 32.0f);
    const float3 color = input.color.rgb * (0.18f + 0.82f * diffuse) +
                         float3(0.18f * specular);
    return float4(color, input.color.a);
}
