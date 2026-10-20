cbuffer SceneUbo : register(b0, space0) {
    column_major float4x4 viewProj;
    column_major float4x4 model;
    column_major float4x4 normalMatrix;
    column_major float4x4 lightViewProj;
    float4 baseColorFactor;
    float4 emissiveMetallic;
    float4 cameraExposure;
    float4 lightDirectionIntensity;
    float4 lightColorRoughness;
    float4 materialParams;
    float4 renderParams;
    float4 ambientFlags;
};

Texture2D<float4> baseColorTexture : register(t0, space0);
SamplerState baseColorSampler : register(s0, space0);
Texture2D<float4> normalTexture : register(t1, space0);
SamplerState normalSampler : register(s1, space0);
Texture2D<float4> metallicRoughnessTexture : register(t2, space0);
SamplerState metallicRoughnessSampler : register(s2, space0);
Texture2D<float4> occlusionTexture : register(t3, space0);
SamplerState occlusionSampler : register(s3, space0);
Texture2D<float4> emissiveTexture : register(t4, space0);
SamplerState emissiveSampler : register(s4, space0);
Texture2D<float4> environmentTexture : register(t5, space0);
SamplerState environmentSampler : register(s5, space0);
Texture2D<float> shadowTexture : register(t6, space0);
SamplerState shadowSampler : register(s6, space0);
// Storage slot 7 is a raw SRV after the seven PBR textures (t0..t6).
ByteAddressBuffer pointLightWords : register(t7, space0);
// Point-shadow atlas at slot 8, past the clustered light SRV.
Texture2D<float> pointShadowTexture : register(t8, space0);
SamplerState pointShadowSampler : register(s8, space0);

static const float PI = 3.14159265358979323846;

struct VertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct InstancedVertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
    float4 model0 : TEXCOORD9;
    float4 model1 : TEXCOORD10;
    float4 model2 : TEXCOORD11;
    float4 model3 : TEXCOORD12;
    float4 normal0 : TEXCOORD13;
    float4 normal1 : TEXCOORD14;
    float4 normal2 : TEXCOORD15;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
    float4 lightClip : TEXCOORD4;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProj, world);
    output.worldPosition = world.xyz;
    output.worldNormal = mul((float3x3)normalMatrix, input.normal);
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = mul(lightViewProj, world);
    return output;
}

VertexOutput VSMainInstanced(InstancedVertexInput input) {
    VertexOutput output;
    const float4 world = input.model0 * input.position.x +
                         input.model1 * input.position.y +
                         input.model2 * input.position.z + input.model3;
    output.position = mul(viewProj, world);
    output.worldPosition = world.xyz;
    output.worldNormal = input.normal0.xyz * input.normal.x +
                         input.normal1.xyz * input.normal.y +
                         input.normal2.xyz * input.normal.z;
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = mul(lightViewProj, world);
    return output;
}

float srgbChannelToLinear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 srgbToLinear(float3 c) {
    return float3(srgbChannelToLinear(c.x), srgbChannelToLinear(c.y),
                  srgbChannelToLinear(c.z));
}

float linearChannelToSrgb(float c) {
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 linearToSrgb(float3 c) {
    return float3(linearChannelToSrgb(c.x), linearChannelToSrgb(c.y),
                  linearChannelToSrgb(c.z));
}

float3 fresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 f0, float roughness) {
    return f0 + (max(1.0 - roughness, f0) - f0) *
                    pow(1.0 - saturate(cosTheta), 5.0);
}

float distributionGgx(float nDotH, float roughness) {
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float d = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * d * d, 1.0e-7);
}

float geometrySchlick(float cosine, float roughness) {
    const float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    return cosine / max(cosine * (1.0 - k) + k, 1.0e-7);
}

float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness);
}

float2 environmentUv(float3 direction) {
    const float3 d = normalize(direction);
    return float2(atan2(d.x, d.z) / (2.0 * PI) + 0.5,
                  acos(clamp(d.y, -1.0, 1.0)) / PI);
}

float3 mappedNormal(VertexOutput input, float3 geometricNormal) {
    const float3 n = normalize(geometricNormal);
    const float3 dpdx = ddx(input.worldPosition);
    const float3 dpdy = ddy(input.worldPosition);
    const float2 duvdx = ddx(input.uv);
    const float2 duvdy = ddy(input.uv);
    const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) < 1.0e-8) return n;
    float3 tangent = (dpdx * duvdy.y - dpdy * duvdx.y) / determinant;
    float3 bitangent = (-dpdx * duvdy.x + dpdy * duvdx.x) / determinant;
    tangent = normalize(tangent - n * dot(n, tangent));
    bitangent = normalize(bitangent - n * dot(n, bitangent) -
                          tangent * dot(tangent, bitangent));
    float3 sampled = normalTexture.Sample(normalSampler, input.uv).xyz * 2.0 - 1.0;
    sampled.xy *= materialParams.x;
    sampled.y *= materialParams.y;
    return normalize(sampled.x * tangent + sampled.y * bitangent + sampled.z * n);
}

float shadowVisibility(VertexOutput input, float nDotL) {
    if (renderParams.z < 0.5 || input.lightClip.w <= 0.0) return 1.0;
    const float3 projected = input.lightClip.xyz / input.lightClip.w;
    const float2 uv = float2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    const float bias = max(renderParams.y * (1.0 - nDotL), renderParams.y * 0.2);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float mapDepth = shadowTexture.Sample(
                shadowSampler, uv + float2(x, y) * renderParams.x);
            visibility += projected.z - bias <= mapDepth ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

uint pointLightWord(uint word) {
    return pointLightWords.Load(word * 4u);
}

float pointLightFloat(uint word) {
    return asfloat(pointLightWords.Load(word * 4u));
}

// Point-light shadows: see engine/lighting/point_shadows.h for the atlas and
// the clustered-buffer words this reads.
float4 pointLightVec4(uint word) {
    return float4(pointLightFloat(word), pointLightFloat(word + 1u),
                  pointLightFloat(word + 2u), pointLightFloat(word + 3u));
}

float pointShadowVisibility(uint shadowSlot, float3 worldPosition, float3 toFragment,
                            float normalizedDistance, float nDotL) {
    const uint matrixBase = pointLightWord(10u);
    if (matrixBase == 0u) return 1.0;
    const float3 a = abs(toFragment);
    uint face;
    if (a.x >= a.y && a.x >= a.z) {
        face = toFragment.x >= 0.0 ? 0u : 1u;
    } else if (a.y >= a.z) {
        face = toFragment.y >= 0.0 ? 2u : 3u;
    } else {
        face = toFragment.z >= 0.0 ? 4u : 5u;
    }
    const uint base = matrixBase + ((shadowSlot - 1u) * 6u + face) * 16u;
    const float4 clip = pointLightVec4(base) * worldPosition.x +
                        pointLightVec4(base + 4u) * worldPosition.y +
                        pointLightVec4(base + 8u) * worldPosition.z +
                        pointLightVec4(base + 12u);
    if (clip.w <= 0.0) return 1.0;
    const float2 local = clip.xy / clip.w;
    const float rows = (float)pointLightWord(12u);
    const float texel = pointLightFloat(14u);
    const float limit = 1.0 - 1.5 * texel;
    const float bias = pointLightFloat(13u) * (1.0 + 2.0 * (1.0 - nDotL));
    const float column = (float)face;
    const float row = (float)(shadowSlot - 1u);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 tap = clamp(local + float2(x, y) * texel, -limit, limit);
            const float2 atlas = float2((tap.x + 1.0 + 2.0 * column) / 6.0 - 1.0,
                                        (tap.y + 1.0 + 2.0 * row) / rows - 1.0);
            const float2 uv = float2(atlas.x * 0.5 + 0.5, 0.5 - atlas.y * 0.5);
            const float stored = pointShadowTexture.SampleLevel(pointShadowSampler, uv, 0.0);
            visibility += normalizedDistance - bias <= stored ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

// Clustered point lights: see engine/lighting/clustered_lights.h for the word
// layout and the shared cluster lookup.
float3 pointLightsDirect(float3 worldPosition, float3 n, float3 v, float3 baseColor,
                         float metallic, float roughness, float3 f0) {
    const uint lightCount = pointLightWord(0u);
    if (lightCount == 0u) return 0.0;
    const float4 clip = mul(viewProj, float4(worldPosition, 1.0));
    if (clip.w <= 0.0) return 0.0;
    const uint tilesX = pointLightWord(1u);
    const uint tilesY = pointLightWord(2u);
    const uint slices = pointLightWord(3u);
    const float2 ndc = clip.xy / clip.w;
    const uint tileX = (uint)clamp(floor((ndc.x * 0.5 + 0.5) * (float)tilesX), 0.0,
                                   (float)(tilesX - 1u));
    const uint tileY = (uint)clamp(floor((ndc.y * 0.5 + 0.5) * (float)tilesY), 0.0,
                                   (float)(tilesY - 1u));
    const float sliceValue = floor(log2(max(clip.w, 1.0e-4)) * pointLightFloat(8u) +
                                   pointLightFloat(9u));
    const uint slice = (uint)clamp(sliceValue, 0.0, (float)(slices - 1u));
    const uint cluster = (slice * tilesY + tileY) * tilesX + tileX;
    const uint clusterWord = pointLightWord(5u) + cluster * 2u;
    const uint first = pointLightWord(clusterWord);
    const uint count = pointLightWord(clusterWord + 1u);
    const uint lightBase = pointLightWord(4u);
    float3 sum = 0.0;
    [loop]
    for (uint i = 0u; i < count; ++i) {
        const uint record = lightBase + pointLightWord(first + i) * 8u;
        const float3 position = float3(pointLightFloat(record), pointLightFloat(record + 1u),
                                       pointLightFloat(record + 2u));
        const float range = pointLightFloat(record + 3u);
        const float3 toLight = position - worldPosition;
        const float distanceSq = dot(toLight, toLight);
        const float rangeSq = range * range;
        if (distanceSq >= rangeSq) continue;
        const float lightDistance = sqrt(distanceSq);
        const float3 l = lightDistance > 1.0e-5 ? toLight / lightDistance : n;
        const float nDotL = max(dot(n, l), 0.0);
        if (nDotL <= 0.0) continue;
        const uint shadowSlot = pointLightWord(record + 7u);
        const float shadow = shadowSlot != 0u
                                 ? pointShadowVisibility(shadowSlot, worldPosition, -toLight,
                                                         lightDistance / range, nDotL)
                                 : 1.0;
        if (shadow <= 0.0) continue;
        const float3 radiance = float3(pointLightFloat(record + 4u),
                                       pointLightFloat(record + 5u),
                                       pointLightFloat(record + 6u));
        const float3 h = normalize(v + l);
        const float nDotV = max(dot(n, v), 0.0);
        const float nDotH = max(dot(n, h), 0.0);
        const float hDotV = max(dot(h, v), 0.0);
        const float3 f = fresnelSchlick(hDotV, f0);
        const float d = distributionGgx(nDotH, roughness);
        const float g = geometrySmith(nDotV, nDotL, roughness);
        const float3 specular = d * g * f / max(4.0 * nDotV * nDotL, 1.0e-5);
        const float3 diffuse = (1.0 - f) * (1.0 - metallic) * baseColor / PI;
        const float ratio = distanceSq / rangeSq;
        const float window = saturate(1.0 - ratio * ratio);
        const float attenuation = window * window / (distanceSq + 1.0);
        sum += (diffuse + specular) * radiance * (attenuation * nDotL * shadow);
    }
    return sum;
}

float3 acesToneMap(float3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float4 PSMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target0 {
    const int flags = (int)(ambientFlags.y + 0.5);
    const float4 baseSample = baseColorTexture.Sample(baseColorSampler, input.uv);
    const float4 vertexColor = (flags & 4) != 0 ? input.color : 1.0;
    const float4 base = float4(srgbToLinear(baseSample.rgb), baseSample.a) *
                        baseColorFactor * vertexColor;
    if ((flags & 1) != 0 && base.a < materialParams.w) discard;

    const float3 emissive = srgbToLinear(
        emissiveTexture.Sample(emissiveSampler, input.uv).rgb) * emissiveMetallic.rgb;
    float3 color;
    if ((flags & 2) != 0) {
        color = base.rgb + emissive;
    } else {
        const float4 mr = metallicRoughnessTexture.Sample(metallicRoughnessSampler,
                                                          input.uv);
        const float metallic = saturate(mr.b * emissiveMetallic.w);
        const float roughness = clamp(mr.g * lightColorRoughness.w, 0.045, 1.0);
        const float3 v = normalize(cameraExposure.xyz - input.worldPosition);
        float3 geometricNormal = (flags & 8) != 0
                                     ? normalize(input.worldNormal)
                                     : normalize(cross(ddx(input.worldPosition),
                                                       ddy(input.worldPosition)));
        if ((flags & 8) != 0) {
            if (!frontFacing) geometricNormal = -geometricNormal;
        } else if (dot(geometricNormal, v) < 0.0) {
            geometricNormal = -geometricNormal;
        }
        const float3 n = mappedNormal(input, geometricNormal);
        const float3 l = normalize(-lightDirectionIntensity.xyz);
        const float3 h = normalize(v + l);
        const float nDotV = max(dot(n, v), 0.0);
        const float nDotL = max(dot(n, l), 0.0);
        const float nDotH = max(dot(n, h), 0.0);
        const float hDotV = max(dot(h, v), 0.0);
        const float3 f0 = lerp(0.04, base.rgb, metallic);
        const float3 f = fresnelSchlick(hDotV, f0);
        const float d = distributionGgx(nDotH, roughness);
        const float g = geometrySmith(nDotV, nDotL, roughness);
        const float3 specular = d * g * f / max(4.0 * nDotV * nDotL, 1.0e-5);
        const float3 diffuse = (1.0 - f) * (1.0 - metallic) * base.rgb / PI;
        const float3 radiance = lightColorRoughness.rgb * lightDirectionIntensity.w;
        const float3 direct = (diffuse + specular) * radiance * nDotL *
                              shadowVisibility(input, nDotL);

        const float3 pointDirect = pointLightsDirect(input.worldPosition, n, v, base.rgb,
                                                     metallic, roughness, f0);

        const float3 reflection = reflect(-v, n);
        const float3 irradiance = environmentTexture.SampleLevel(
            environmentSampler, environmentUv(n), renderParams.w).rgb;
        const float3 prefiltered = environmentTexture.SampleLevel(
            environmentSampler, environmentUv(reflection), roughness * renderParams.w).rgb;
        const float3 envF = fresnelSchlickRoughness(nDotV, f0, roughness);
        const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
        const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
        const float4 r = roughness * c0 + c1;
        const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
        const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
        const float3 indirectSpecular = prefiltered * (f0 * ab.x + ab.y);
        const float3 indirectDiffuse = (1.0 - envF) * (1.0 - metallic) *
                                       base.rgb * irradiance;
        const float ao = lerp(1.0, occlusionTexture.Sample(occlusionSampler, input.uv).r,
                              materialParams.z);
        color = direct + pointDirect + (indirectDiffuse + indirectSpecular) * ao * ambientFlags.x +
                emissive;
    }
    const float3 exposed = color * cameraExposure.w;
    // ambientFlags.z: write linear HDR for the bloom chain, which tone maps at
    // composite time; otherwise tone map + encode here exactly as before.
    if (ambientFlags.z > 0.5) return float4(exposed, base.a);
    return float4(linearToSrgb(acesToneMap(exposed)), base.a);
}
