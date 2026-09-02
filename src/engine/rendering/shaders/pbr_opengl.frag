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
        color = direct + (indirectDiffuse + indirectSpecular) * ao * ambientFlags.x +
                emissive;
    }
    outColor = vec4(linearToSrgb(acesToneMap(color * cameraExposure.w)), base.a);
}
