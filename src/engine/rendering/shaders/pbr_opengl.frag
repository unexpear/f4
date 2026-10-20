#version 430 core

layout(std140, binding = 0) uniform SceneUbo {
    mat4 viewProj;
    mat4 model;
    mat4 normalMatrix;
    mat4 lightViewProj;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 cameraExposure;
    vec4 lightDirectionIntensity;
    vec4 lightColorRoughness;
    vec4 materialParams;
    vec4 renderParams;
    vec4 ambientFlags;
};

uniform sampler2D uTex;
uniform sampler2D uTex1;
uniform sampler2D uTex2;
uniform sampler2D uTex3;
uniform sampler2D uTex4;
uniform sampler2D uTex5;
uniform sampler2D uTex6;
// RHI storage slot 7 maps directly to SSBO binding 7 on OpenGL.
layout(std430, binding = 7) readonly buffer PointLightData {
    uint pointLightWords[];
};
// Point-shadow atlas on texture unit 8.
uniform sampler2D uTex8;

in vec3 vWorldPosition;
in vec3 vWorldNormal;
in vec2 vUv;
in vec4 vColor;
in vec4 vLightClip;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979323846;

vec3 srgbToLinear(vec3 c) {
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(c, vec3(0.0031308)));
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) *
                    pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

float distributionGgx(float nDotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float d = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * d * d, 1.0e-7);
}

float geometrySchlick(float cosine, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    return cosine / max(cosine * (1.0 - k) + k, 1.0e-7);
}

float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness);
}

vec2 environmentUv(vec3 direction) {
    vec3 d = normalize(direction);
    return vec2(atan(d.x, d.z) / (2.0 * PI) + 0.5,
                acos(clamp(d.y, -1.0, 1.0)) / PI);
}

vec3 mappedNormal(vec3 geometricNormal) {
    vec3 n = normalize(geometricNormal);
    vec3 dpdx = dFdx(vWorldPosition);
    vec3 dpdy = dFdy(vWorldPosition);
    vec2 duvdx = dFdx(vUv);
    vec2 duvdy = dFdy(vUv);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) < 1.0e-8) return n;
    vec3 tangent = (dpdx * duvdy.y - dpdy * duvdx.y) / determinant;
    vec3 bitangent = (-dpdx * duvdy.x + dpdy * duvdx.x) / determinant;
    tangent = normalize(tangent - n * dot(n, tangent));
    bitangent = normalize(bitangent - n * dot(n, bitangent) -
                          tangent * dot(tangent, bitangent));
    vec3 sampled = texture(uTex1, vUv).xyz * 2.0 - 1.0;
    sampled.xy *= materialParams.x;
    sampled.y *= materialParams.y;
    return normalize(mat3(tangent, bitangent, n) * sampled);
}

float shadowVisibility(float nDotL) {
    if (renderParams.z < 0.5 || vLightClip.w <= 0.0) return 1.0;
    vec3 projected = vLightClip.xyz / vLightClip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    float bias = max(renderParams.y * (1.0 - nDotL), renderParams.y * 0.2);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float mapDepth = texture(uTex6, uv + vec2(x, y) * renderParams.x).r;
            visibility += projected.z - bias <= mapDepth ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

float pointLightFloat(uint word) {
    return uintBitsToFloat(pointLightWords[word]);
}

// Point-light shadows: see engine/lighting/point_shadows.h for the atlas and
// the clustered-buffer words this reads.
vec4 pointLightVec4(uint word) {
    return vec4(pointLightFloat(word), pointLightFloat(word + 1u),
                pointLightFloat(word + 2u), pointLightFloat(word + 3u));
}

float pointShadowVisibility(uint shadowSlot, vec3 worldPosition, vec3 toFragment,
                            float normalizedDistance, float nDotL) {
    uint matrixBase = pointLightWords[10];
    if (matrixBase == 0u) return 1.0;
    vec3 a = abs(toFragment);
    uint face = a.x >= a.y && a.x >= a.z ? (toFragment.x >= 0.0 ? 0u : 1u)
              : (a.y >= a.z ? (toFragment.y >= 0.0 ? 2u : 3u)
                            : (toFragment.z >= 0.0 ? 4u : 5u));
    uint base = matrixBase + ((shadowSlot - 1u) * 6u + face) * 16u;
    vec4 clip = pointLightVec4(base) * worldPosition.x +
                pointLightVec4(base + 4u) * worldPosition.y +
                pointLightVec4(base + 8u) * worldPosition.z + pointLightVec4(base + 12u);
    if (clip.w <= 0.0) return 1.0;
    vec2 local = clip.xy / clip.w;
    float rows = float(pointLightWords[12]);
    float texel = pointLightFloat(14u);
    float limit = 1.0 - 1.5 * texel;
    float bias = pointLightFloat(13u) * (1.0 + 2.0 * (1.0 - nDotL));
    float column = float(face);
    float row = float(shadowSlot - 1u);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = clamp(local + vec2(float(x), float(y)) * texel, vec2(-limit),
                             vec2(limit));
            vec2 atlas = vec2((tap.x + 1.0 + 2.0 * column) / 6.0 - 1.0,
                              (tap.y + 1.0 + 2.0 * row) / rows - 1.0);
            vec2 uv = atlas * 0.5 + 0.5;
            float stored = textureLod(uTex8, uv, 0.0).r;
            visibility += normalizedDistance - bias <= stored ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

// Clustered point lights: see engine/lighting/clustered_lights.h for the word
// layout and the shared cluster lookup.
vec3 pointLightsDirect(vec3 n, vec3 v, vec3 baseColor, float metallic, float roughness,
                       vec3 f0) {
    uint lightCount = pointLightWords[0];
    if (lightCount == 0u) return vec3(0.0);
    vec4 clip = viewProj * vec4(vWorldPosition, 1.0);
    if (clip.w <= 0.0) return vec3(0.0);
    uint tilesX = pointLightWords[1];
    uint tilesY = pointLightWords[2];
    uint slices = pointLightWords[3];
    vec2 ndc = clip.xy / clip.w;
    uint tileX = uint(clamp(floor((ndc.x * 0.5 + 0.5) * float(tilesX)), 0.0,
                            float(tilesX - 1u)));
    uint tileY = uint(clamp(floor((ndc.y * 0.5 + 0.5) * float(tilesY)), 0.0,
                            float(tilesY - 1u)));
    float sliceValue = floor(log2(max(clip.w, 1.0e-4)) * pointLightFloat(8u) +
                             pointLightFloat(9u));
    uint slice = uint(clamp(sliceValue, 0.0, float(slices - 1u)));
    uint cluster = (slice * tilesY + tileY) * tilesX + tileX;
    uint clusterWord = pointLightWords[5] + cluster * 2u;
    uint first = pointLightWords[clusterWord];
    uint count = pointLightWords[clusterWord + 1u];
    uint lightBase = pointLightWords[4];
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < count; ++i) {
        uint record = lightBase + pointLightWords[first + i] * 8u;
        vec3 position = vec3(pointLightFloat(record), pointLightFloat(record + 1u),
                             pointLightFloat(record + 2u));
        float range = pointLightFloat(record + 3u);
        vec3 toLight = position - vWorldPosition;
        float distanceSq = dot(toLight, toLight);
        float rangeSq = range * range;
        if (distanceSq >= rangeSq) continue;
        float lightDistance = sqrt(distanceSq);
        vec3 l = lightDistance > 1.0e-5 ? toLight / lightDistance : n;
        float nDotL = max(dot(n, l), 0.0);
        if (nDotL <= 0.0) continue;
        uint shadowSlot = pointLightWords[record + 7u];
        float shadow = shadowSlot != 0u
                           ? pointShadowVisibility(shadowSlot, vWorldPosition, -toLight,
                                                   lightDistance / range, nDotL)
                           : 1.0;
        if (shadow <= 0.0) continue;
        vec3 radiance = vec3(pointLightFloat(record + 4u), pointLightFloat(record + 5u),
                             pointLightFloat(record + 6u));
        vec3 h = normalize(v + l);
        float nDotV = max(dot(n, v), 0.0);
        float nDotH = max(dot(n, h), 0.0);
        float hDotV = max(dot(h, v), 0.0);
        vec3 f = fresnelSchlick(hDotV, f0);
        float d = distributionGgx(nDotH, roughness);
        float g = geometrySmith(nDotV, nDotL, roughness);
        vec3 specular = d * g * f / max(4.0 * nDotV * nDotL, 1.0e-5);
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * baseColor / PI;
        float ratio = distanceSq / rangeSq;
        float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        float attenuation = window * window / (distanceSq + 1.0);
        sum += (diffuse + specular) * radiance * (attenuation * nDotL * shadow);
    }
    return sum;
}

vec3 acesToneMap(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e),
                 0.0, 1.0);
}

void main() {
    int flags = int(ambientFlags.y + 0.5);
    vec4 baseSample = texture(uTex, vUv);
    vec4 vertexColor = (flags & 4) != 0 ? vColor : vec4(1.0);
    vec4 base = vec4(srgbToLinear(baseSample.rgb), baseSample.a) *
                baseColorFactor * vertexColor;
    if ((flags & 1) != 0 && base.a < materialParams.w) discard;

    vec3 emissive = srgbToLinear(texture(uTex4, vUv).rgb) * emissiveMetallic.rgb;
    vec3 color;
    if ((flags & 2) != 0) {
        color = base.rgb + emissive;
    } else {
        vec4 mr = texture(uTex2, vUv);
        float metallic = clamp(mr.b * emissiveMetallic.w, 0.0, 1.0);
        float roughness = clamp(mr.g * lightColorRoughness.w, 0.045, 1.0);
        vec3 v = normalize(cameraExposure.xyz - vWorldPosition);
        vec3 geometricNormal = (flags & 8) != 0
                                   ? normalize(vWorldNormal)
                                   : normalize(cross(dFdx(vWorldPosition), dFdy(vWorldPosition)));
        if ((flags & 8) != 0) {
            if (!gl_FrontFacing) geometricNormal = -geometricNormal;
        } else if (dot(geometricNormal, v) < 0.0) {
            geometricNormal = -geometricNormal;
        }
        vec3 n = mappedNormal(geometricNormal);
        vec3 l = normalize(-lightDirectionIntensity.xyz);
        vec3 h = normalize(v + l);
        float nDotV = max(dot(n, v), 0.0);
        float nDotL = max(dot(n, l), 0.0);
        float nDotH = max(dot(n, h), 0.0);
        float hDotV = max(dot(h, v), 0.0);
        vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
        vec3 f = fresnelSchlick(hDotV, f0);
        float d = distributionGgx(nDotH, roughness);
        float g = geometrySmith(nDotV, nDotL, roughness);
        vec3 specular = d * g * f / max(4.0 * nDotV * nDotL, 1.0e-5);
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * base.rgb / PI;
        vec3 radiance = lightColorRoughness.rgb * lightDirectionIntensity.w;
        vec3 direct = (diffuse + specular) * radiance * nDotL * shadowVisibility(nDotL);

        vec3 pointDirect = pointLightsDirect(n, v, base.rgb, metallic, roughness, f0);

        vec3 reflection = reflect(-v, n);
        vec3 irradiance = textureLod(uTex5, environmentUv(n), renderParams.w).rgb;
        vec3 prefiltered = textureLod(uTex5, environmentUv(reflection),
                                      roughness * renderParams.w).rgb;
        vec3 envF = fresnelSchlickRoughness(nDotV, f0, roughness);
        vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
        vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
        vec4 r = roughness * c0 + c1;
        float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
        vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
        vec3 indirectSpecular = prefiltered * (f0 * ab.x + ab.y);
        vec3 indirectDiffuse = (vec3(1.0) - envF) * (1.0 - metallic) *
                               base.rgb * irradiance;
        float ao = mix(1.0, texture(uTex3, vUv).r, materialParams.z);
        color = direct + pointDirect + (indirectDiffuse + indirectSpecular) * ao * ambientFlags.x +
                emissive;
    }
    vec3 exposed = color * cameraExposure.w;
    // ambientFlags.z: write linear HDR for the bloom chain, which tone maps at
    // composite time; otherwise tone map + encode here exactly as before.
    outColor = ambientFlags.z > 0.5 ? vec4(exposed, base.a)
                                    : vec4(linearToSrgb(acesToneMap(exposed)), base.a);
}
