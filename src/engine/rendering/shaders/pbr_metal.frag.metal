#include <metal_stdlib>
using namespace metal;

struct SceneUbo {
    float4x4 viewProj;
    float4x4 model;
    float4x4 normalMatrix;
    float4x4 lightViewProj;
    float4 baseColorFactor;
    float4 emissiveMetallic;
    float4 cameraExposure;
    float4 lightDirectionIntensity;
    float4 lightColorRoughness;
    float4 materialParams;
    float4 renderParams;
    float4 ambientFlags;
};

struct VertexOutput {
    float4 position [[position]];
    float3 worldPosition;
    float3 worldNormal;
    float2 uv;
    float4 color;
    float4 lightClip;
};

constant float kPi = 3.14159265358979323846f;

float srgbChannelToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f
                             : pow((value + 0.055f) / 1.055f, 2.4f);
}

float3 srgbToLinear(float3 value) {
    return float3(srgbChannelToLinear(value.x), srgbChannelToLinear(value.y),
                  srgbChannelToLinear(value.z));
}

float linearChannelToSrgb(float value) {
    value = max(value, 0.0f);
    return value <= 0.0031308f ? value * 12.92f
                               : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float3 linearToSrgb(float3 value) {
    return float3(linearChannelToSrgb(value.x), linearChannelToSrgb(value.y),
                  linearChannelToSrgb(value.z));
}

float3 fresnelSchlick(float cosine, float3 f0) {
    return f0 + (1.0f - f0) * pow(1.0f - clamp(cosine, 0.0f, 1.0f), 5.0f);
}

float3 fresnelSchlickRoughness(float cosine, float3 f0, float roughness) {
    return f0 + (max(float3(1.0f - roughness), f0) - f0) *
                    pow(1.0f - clamp(cosine, 0.0f, 1.0f), 5.0f);
}

float distributionGgx(float nDotH, float roughness) {
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float denominator = nDotH * nDotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(kPi * denominator * denominator, 1.0e-7f);
}

float geometrySchlick(float cosine, float roughness) {
    const float k = (roughness + 1.0f) * (roughness + 1.0f) * 0.125f;
    return cosine / max(cosine * (1.0f - k) + k, 1.0e-7f);
}

float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness);
}

float2 environmentUv(float3 direction) {
    const float3 normalized = normalize(direction);
    return float2(atan2(normalized.x, normalized.z) / (2.0f * kPi) + 0.5f,
                  acos(clamp(normalized.y, -1.0f, 1.0f)) / kPi);
}

float3 mappedNormal(VertexOutput input, float3 geometricNormal,
                    constant SceneUbo& scene,
                    texture2d<float> normalTexture, sampler normalSampler) {
    const float3 normal = normalize(geometricNormal);
    const float3 dpdx = dfdx(input.worldPosition);
    const float3 dpdy = dfdy(input.worldPosition);
    const float2 duvdx = dfdx(input.uv);
    const float2 duvdy = dfdy(input.uv);
    const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) < 1.0e-8f) return normal;
    float3 tangent = (dpdx * duvdy.y - dpdy * duvdx.y) / determinant;
    float3 bitangent = (-dpdx * duvdy.x + dpdy * duvdx.x) / determinant;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    bitangent = normalize(bitangent - normal * dot(normal, bitangent) -
                          tangent * dot(tangent, bitangent));
    float3 sampled = normalTexture.sample(normalSampler, input.uv).xyz * 2.0f - 1.0f;
    sampled.xy *= scene.materialParams.x;
    sampled.y *= scene.materialParams.y;
    return normalize(sampled.x * tangent + sampled.y * bitangent + sampled.z * normal);
}

float shadowVisibility(VertexOutput input, float nDotL,
                       constant SceneUbo& scene,
                       texture2d<float> shadowTexture, sampler shadowSampler) {
    if (scene.renderParams.z < 0.5f || input.lightClip.w <= 0.0f) return 1.0f;
    const float3 projected = input.lightClip.xyz / input.lightClip.w;
    const float2 uv(projected.x * 0.5f + 0.5f, 0.5f - projected.y * 0.5f);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f) {
        return 1.0f;
    }
    const float bias = max(scene.renderParams.y * (1.0f - nDotL),
                           scene.renderParams.y * 0.2f);
    float visibility = 0.0f;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float mapDepth = shadowTexture.sample(
                shadowSampler, uv + float2(x, y) * scene.renderParams.x).r;
            visibility += projected.z - bias <= mapDepth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

float3 acesToneMap(float3 color) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e),
                 0.0f, 1.0f);
}

fragment float4 pbr_fragment(
    VertexOutput input [[stage_in]],
    constant SceneUbo& scene [[buffer(16)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    sampler baseColorSampler [[sampler(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    sampler normalSampler [[sampler(1)]],
    texture2d<float> metallicRoughnessTexture [[texture(2)]],
    sampler metallicRoughnessSampler [[sampler(2)]],
    texture2d<float> occlusionTexture [[texture(3)]],
    sampler occlusionSampler [[sampler(3)]],
    texture2d<float> emissiveTexture [[texture(4)]],
    sampler emissiveSampler [[sampler(4)]],
    texture2d<float> environmentTexture [[texture(5)]],
    sampler environmentSampler [[sampler(5)]],
    texture2d<float> shadowTexture [[texture(6)]],
    sampler shadowSampler [[sampler(6)]],
    bool frontFacing [[front_facing]]) {
    const int flags = int(scene.ambientFlags.y + 0.5f);
    const float4 baseSample = baseColorTexture.sample(baseColorSampler, input.uv);
    const float4 vertexColor = (flags & 4) != 0 ? input.color : float4(1.0f);
    const float4 base = float4(srgbToLinear(baseSample.rgb), baseSample.a) *
                        scene.baseColorFactor * vertexColor;
    if ((flags & 1) != 0 && base.a < scene.materialParams.w) discard_fragment();

    const float3 emissive = srgbToLinear(
        emissiveTexture.sample(emissiveSampler, input.uv).rgb) * scene.emissiveMetallic.rgb;
    float3 color;
    if ((flags & 2) != 0) {
        color = base.rgb + emissive;
    } else {
        const float4 metallicRoughness = metallicRoughnessTexture.sample(
            metallicRoughnessSampler, input.uv);
        const float metallic = clamp(metallicRoughness.b * scene.emissiveMetallic.w,
                                     0.0f, 1.0f);
        const float roughness = clamp(metallicRoughness.g * scene.lightColorRoughness.w,
                                      0.045f, 1.0f);
        const float3 view = normalize(scene.cameraExposure.xyz - input.worldPosition);
        float3 geometricNormal = (flags & 8) != 0
                                     ? normalize(input.worldNormal)
                                     : normalize(cross(dfdx(input.worldPosition),
                                                       dfdy(input.worldPosition)));
        if ((flags & 8) != 0) {
            if (!frontFacing) geometricNormal = -geometricNormal;
        } else if (dot(geometricNormal, view) < 0.0f) {
            geometricNormal = -geometricNormal;
        }
        const float3 normal = mappedNormal(input, geometricNormal, scene,
                                           normalTexture, normalSampler);
        const float3 light = normalize(-scene.lightDirectionIntensity.xyz);
        const float3 halfVector = normalize(view + light);
        const float nDotV = max(dot(normal, view), 0.0f);
        const float nDotL = max(dot(normal, light), 0.0f);
        const float nDotH = max(dot(normal, halfVector), 0.0f);
        const float hDotV = max(dot(halfVector, view), 0.0f);
        const float3 f0 = mix(float3(0.04f), base.rgb, metallic);
        const float3 fresnel = fresnelSchlick(hDotV, f0);
        const float distribution = distributionGgx(nDotH, roughness);
        const float geometry = geometrySmith(nDotV, nDotL, roughness);
        const float3 specular = distribution * geometry * fresnel /
                                max(4.0f * nDotV * nDotL, 1.0e-5f);
        const float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * base.rgb / kPi;
        const float3 radiance = scene.lightColorRoughness.rgb *
                                scene.lightDirectionIntensity.w;
        const float3 direct = (diffuse + specular) * radiance * nDotL *
                              shadowVisibility(input, nDotL, scene,
                                               shadowTexture, shadowSampler);

        const float3 reflection = reflect(-view, normal);
        const float3 irradiance = environmentTexture.sample(
            environmentSampler, environmentUv(normal),
            level(scene.renderParams.w)).rgb;
        const float3 prefiltered = environmentTexture.sample(
            environmentSampler, environmentUv(reflection),
            level(roughness * scene.renderParams.w)).rgb;
        const float3 environmentFresnel = fresnelSchlickRoughness(
            nDotV, f0, roughness);
        const float4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
        const float4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
        const float4 r = roughness * c0 + c1;
        const float a004 = min(r.x * r.x, exp2(-9.28f * nDotV)) * r.x + r.y;
        const float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
        const float3 indirectSpecular = prefiltered * (f0 * ab.x + ab.y);
        const float3 indirectDiffuse = (1.0f - environmentFresnel) *
                                       (1.0f - metallic) * base.rgb * irradiance;
        const float ao = mix(1.0f,
                             occlusionTexture.sample(occlusionSampler, input.uv).r,
                             scene.materialParams.z);
        color = direct + (indirectDiffuse + indirectSpecular) * ao *
                         scene.ambientFlags.x + emissive;
    }
    return float4(linearToSrgb(acesToneMap(color * scene.cameraExposure.w)), base.a);
}
