#include <metal_stdlib>
using namespace metal;

struct Scene {
    float4x4 viewProj;
    float4 cameraPos;
    float4 lightPos;
};

struct FragmentInput {
    float4 position [[position]];
    float3 worldPosition;
    float3 normal;
    float3 material;
};

constant float kPi = 3.14159265359f;

float distributionGgx(float3 normal, float3 halfway, float roughness) {
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float nDotH = max(dot(normal, halfway), 0.0f);
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(kPi * denominator * denominator, 0.0001f);
}

float geometrySchlickGgx(float nDotV, float roughness) {
    const float radius = roughness + 1.0f;
    const float k = radius * radius / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 0.0001f);
}

float3 fresnelSchlick(float cosine, float3 f0) {
    return f0 + (1.0f - f0) * pow(1.0f - cosine, 5.0f);
}

fragment float4 validation_pbr_fragment(
    FragmentInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    const float metallic = clamp(input.material.x, 0.0f, 1.0f);
    const float roughness = clamp(input.material.y, 0.06f, 1.0f);
    const float3 albedo = mix(float3(0.72f, 0.08f, 0.035f),
                              float3(0.95f, 0.58f, 0.12f), input.material.z);
    const float3 normal = normalize(input.normal);
    const float3 viewDirection = normalize(scene.cameraPos.xyz - input.worldPosition);
    const float3 toLight = scene.lightPos.xyz - input.worldPosition;
    const float distanceSquared = max(dot(toLight, toLight), 0.01f);
    const float3 lightDirection = normalize(toLight);
    const float3 halfway = normalize(viewDirection + lightDirection);
    const float3 radiance = float3(135.0f) / distanceSquared;
    const float3 f0 = mix(float3(0.04f), albedo, metallic);
    const float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);
    const float distribution = distributionGgx(normal, halfway, roughness);
    const float geometry = geometrySchlickGgx(max(dot(normal, viewDirection), 0.0f), roughness) *
                           geometrySchlickGgx(max(dot(normal, lightDirection), 0.0f), roughness);
    const float3 specular = distribution * geometry * fresnel /
                            max(4.0f * max(dot(normal, viewDirection), 0.0f) *
                                max(dot(normal, lightDirection), 0.0f), 0.001f);
    const float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * albedo / kPi;
    float3 color = float3(0.025f) * albedo +
                   (diffuse + specular) * radiance *
                       max(dot(normal, lightDirection), 0.0f);
    color = color / (color + 1.0f);
    color = pow(color, float3(1.0f / 2.2f));
    return float4(color, 1.0f);
}
