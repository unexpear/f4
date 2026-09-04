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
    float3 color;
    float3 normal;
    float3 worldPosition;
};

fragment float4 validation_terrain_fragment(
    FragmentInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    const float3 normal = normalize(input.normal);
    const float3 light = normalize(-scene.lightDir);
    const float3 view = normalize(scene.viewPos - input.worldPosition);
    const float diffuse = max(dot(normal, light), 0.0f);
    const float rim = pow(1.0f - max(dot(normal, view), 0.0f), 3.0f);
    const float3 lit = input.color * (0.28f + 0.72f * diffuse) +
                       float3(0.08f, 0.11f, 0.14f) * rim;
    return float4(lit, 1.0f);
}
