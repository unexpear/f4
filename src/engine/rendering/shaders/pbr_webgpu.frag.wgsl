struct SceneUbo {
    viewProj: mat4x4<f32>,
    model: mat4x4<f32>,
    normalMatrix: mat4x4<f32>,
    lightViewProj: mat4x4<f32>,
    baseColorFactor: vec4f,
    emissiveMetallic: vec4f,
    cameraExposure: vec4f,
    lightDirectionIntensity: vec4f,
    lightColorRoughness: vec4f,
    materialParams: vec4f,
    renderParams: vec4f,
    ambientFlags: vec4f,
};

@group(0) @binding(0) var<uniform> scene: SceneUbo;
@group(0) @binding(1) var baseColorTexture: texture_2d<f32>;
@group(0) @binding(2) var baseColorSampler: sampler;
@group(0) @binding(5) var normalTexture: texture_2d<f32>;
@group(0) @binding(6) var normalSampler: sampler;
@group(0) @binding(9) var metallicRoughnessTexture: texture_2d<f32>;
@group(0) @binding(10) var metallicRoughnessSampler: sampler;
@group(0) @binding(13) var occlusionTexture: texture_2d<f32>;
@group(0) @binding(14) var occlusionSampler: sampler;
@group(0) @binding(17) var emissiveTexture: texture_2d<f32>;
@group(0) @binding(18) var emissiveSampler: sampler;
@group(0) @binding(21) var environmentTexture: texture_2d<f32>;
@group(0) @binding(22) var environmentSampler: sampler;
@group(0) @binding(25) var shadowTexture: texture_2d<f32>;
@group(0) @binding(26) var shadowSampler: sampler;
// Storage slot 7 flattens to binding 7 * 4 + StorageBuffer(3) = 31.
@group(0) @binding(31) var<storage, read> pointLightWords: array<u32>;
// Point-shadow atlas, slot 8: bindings 8 * 4 + 1 = 33 and 8 * 4 + 2 = 34.
@group(0) @binding(33) var pointShadowTexture: texture_2d<f32>;
@group(0) @binding(34) var pointShadowSampler: sampler;

const PI = 3.14159265358979323846;

struct FragmentInput {
    @builtin(front_facing) frontFacing: bool,
    @location(0) worldPosition: vec3f,
    @location(1) worldNormal: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec4f,
    @location(4) lightClip: vec4f,
};

fn srgbToLinear(c: vec3f) -> vec3f {
    let low = c / 12.92;
    let high = pow((c + vec3f(0.055)) / 1.055, vec3f(2.4));
    return select(high, low, c <= vec3f(0.04045));
}

fn linearToSrgb(inputColor: vec3f) -> vec3f {
    let color = max(inputColor, vec3f(0.0));
    let low = color * 12.92;
    let high = 1.055 * pow(color, vec3f(1.0 / 2.4)) - vec3f(0.055);
    return select(high, low, color <= vec3f(0.0031308));
}

fn fresnelSchlick(cosTheta: f32, f0: vec3f) -> vec3f {
    return f0 + (vec3f(1.0) - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, f0: vec3f, roughness: f32) -> vec3f {
    return f0 + (max(vec3f(1.0 - roughness), f0) - f0) *
                    pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

fn distributionGgx(nDotH: f32, roughness: f32) -> f32 {
    let alpha = roughness * roughness;
    let alpha2 = alpha * alpha;
    let denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 1.0e-7);
}

fn geometrySchlick(cosine: f32, roughness: f32) -> f32 {
    let k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    return cosine / max(cosine * (1.0 - k) + k, 1.0e-7);
}

fn geometrySmith(nDotV: f32, nDotL: f32, roughness: f32) -> f32 {
    return geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness);
}

fn environmentUv(direction: vec3f) -> vec2f {
    let d = normalize(direction);
    return vec2f(atan2(d.x, d.z) / (2.0 * PI) + 0.5,
                 acos(clamp(d.y, -1.0, 1.0)) / PI);
}

fn mappedNormal(input: FragmentInput, geometricNormal: vec3f) -> vec3f {
    let n = normalize(geometricNormal);
    let positionDx = dpdx(input.worldPosition);
    let positionDy = dpdy(input.worldPosition);
    let uvDx = dpdx(input.uv);
    let uvDy = dpdy(input.uv);
    let determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) < 1.0e-8) { return n; }
    var tangent = (positionDx * uvDy.y - positionDy * uvDx.y) / determinant;
    var bitangent = (-positionDx * uvDy.x + positionDy * uvDx.x) / determinant;
    tangent = normalize(tangent - n * dot(n, tangent));
    bitangent = normalize(bitangent - n * dot(n, bitangent) -
                          tangent * dot(tangent, bitangent));
    var sampled = textureSampleGrad(normalTexture, normalSampler, input.uv,
                                    uvDx, uvDy).xyz * 2.0 - 1.0;
    sampled.x *= scene.materialParams.x;
    sampled.y *= scene.materialParams.x * scene.materialParams.y;
    return normalize(sampled.x * tangent + sampled.y * bitangent + sampled.z * n);
}

fn shadowVisibility(input: FragmentInput, nDotL: f32) -> f32 {
    if (scene.renderParams.z < 0.5 || input.lightClip.w <= 0.0) { return 1.0; }
    let projected = input.lightClip.xyz / input.lightClip.w;
    let uv = vec2f(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }
    let bias = max(scene.renderParams.y * (1.0 - nDotL), scene.renderParams.y * 0.2);
    var visibility = 0.0;
    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            let offset = vec2f(f32(x), f32(y)) * scene.renderParams.x;
            let mapDepth = textureSampleLevel(shadowTexture, shadowSampler,
                                              uv + offset, 0.0).r;
            visibility += select(0.0, 1.0, projected.z - bias <= mapDepth);
        }
    }
    return visibility / 9.0;
}

fn pointLightFloat(word: u32) -> f32 {
    return bitcast<f32>(pointLightWords[word]);
}

// Point-light shadows: see engine/lighting/point_shadows.h for the atlas and
// the clustered-buffer words this reads.
fn pointLightVec4(word: u32) -> vec4f {
    return vec4f(pointLightFloat(word), pointLightFloat(word + 1u),
                 pointLightFloat(word + 2u), pointLightFloat(word + 3u));
}

fn pointShadowVisibility(shadowSlot: u32, worldPosition: vec3f, toFragment: vec3f,
                         normalizedDistance: f32, nDotL: f32) -> f32 {
    let matrixBase = pointLightWords[10];
    if (matrixBase == 0u) { return 1.0; }
    let a = abs(toFragment);
    var face = 5u;
    if (a.x >= a.y && a.x >= a.z) {
        face = select(1u, 0u, toFragment.x >= 0.0);
    } else if (a.y >= a.z) {
        face = select(3u, 2u, toFragment.y >= 0.0);
    } else {
        face = select(5u, 4u, toFragment.z >= 0.0);
    }
    let base = matrixBase + ((shadowSlot - 1u) * 6u + face) * 16u;
    let clip = pointLightVec4(base) * worldPosition.x +
               pointLightVec4(base + 4u) * worldPosition.y +
               pointLightVec4(base + 8u) * worldPosition.z + pointLightVec4(base + 12u);
    if (clip.w <= 0.0) { return 1.0; }
    let local = clip.xy / clip.w;
    let rows = f32(pointLightWords[12]);
    let texel = pointLightFloat(14u);
    let limit = 1.0 - 1.5 * texel;
    let bias = pointLightFloat(13u) * (1.0 + 2.0 * (1.0 - nDotL));
    let column = f32(face);
    let row = f32(shadowSlot - 1u);
    var visibility = 0.0;
    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            let tap = clamp(local + vec2f(f32(x), f32(y)) * texel, vec2f(-limit),
                            vec2f(limit));
            let atlas = vec2f((tap.x + 1.0 + 2.0 * column) / 6.0 - 1.0,
                              (tap.y + 1.0 + 2.0 * row) / rows - 1.0);
            let uv = vec2f(atlas.x * 0.5 + 0.5, 0.5 - atlas.y * 0.5);
            let stored = textureSampleLevel(pointShadowTexture, pointShadowSampler, uv,
                                            0.0).r;
            visibility += select(0.0, 1.0, normalizedDistance - bias <= stored);
        }
    }
    return visibility / 9.0;
}

// Clustered point lights: see engine/lighting/clustered_lights.h for the word
// layout and the shared cluster lookup.
fn pointLightsDirect(worldPosition: vec3f, n: vec3f, v: vec3f, baseColor: vec3f,
                     metallic: f32, roughness: f32, f0: vec3f) -> vec3f {
    let lightCount = pointLightWords[0];
    if (lightCount == 0u) { return vec3f(0.0); }
    let clip = scene.viewProj * vec4f(worldPosition, 1.0);
    if (clip.w <= 0.0) { return vec3f(0.0); }
    let tilesX = pointLightWords[1];
    let tilesY = pointLightWords[2];
    let slices = pointLightWords[3];
    let ndc = clip.xy / clip.w;
    let tileX = u32(clamp(floor((ndc.x * 0.5 + 0.5) * f32(tilesX)), 0.0,
                          f32(tilesX - 1u)));
    let tileY = u32(clamp(floor((ndc.y * 0.5 + 0.5) * f32(tilesY)), 0.0,
                          f32(tilesY - 1u)));
    let sliceValue = floor(log2(max(clip.w, 1.0e-4)) * pointLightFloat(8u) +
                           pointLightFloat(9u));
    let slice = u32(clamp(sliceValue, 0.0, f32(slices - 1u)));
    let cluster = (slice * tilesY + tileY) * tilesX + tileX;
    let clusterWord = pointLightWords[5] + cluster * 2u;
    let first = pointLightWords[clusterWord];
    let count = pointLightWords[clusterWord + 1u];
    let lightBase = pointLightWords[4];
    var sum = vec3f(0.0);
    for (var i = 0u; i < count; i += 1u) {
        let record = lightBase + pointLightWords[first + i] * 8u;
        let position = vec3f(pointLightFloat(record), pointLightFloat(record + 1u),
                             pointLightFloat(record + 2u));
        let range = pointLightFloat(record + 3u);
        let toLight = position - worldPosition;
        let distanceSq = dot(toLight, toLight);
        let rangeSq = range * range;
        if (distanceSq >= rangeSq) { continue; }
        let lightDistance = sqrt(distanceSq);
        let l = select(n, toLight / lightDistance, lightDistance > 1.0e-5);
        let nDotL = max(dot(n, l), 0.0);
        if (nDotL <= 0.0) { continue; }
        let shadowSlot = pointLightWords[record + 7u];
        var shadow = 1.0;
        if (shadowSlot != 0u) {
            shadow = pointShadowVisibility(shadowSlot, worldPosition, -toLight,
                                           lightDistance / range, nDotL);
        }
        if (shadow <= 0.0) { continue; }
        let radiance = vec3f(pointLightFloat(record + 4u), pointLightFloat(record + 5u),
                             pointLightFloat(record + 6u));
        let h = normalize(v + l);
        let nDotV = max(dot(n, v), 0.0);
        let nDotH = max(dot(n, h), 0.0);
        let hDotV = max(dot(h, v), 0.0);
        let f = fresnelSchlick(hDotV, f0);
        let distribution = distributionGgx(nDotH, roughness);
        let geometry = geometrySmith(nDotV, nDotL, roughness);
        let specular = distribution * geometry * f / max(4.0 * nDotV * nDotL, 1.0e-5);
        let diffuse = (vec3f(1.0) - f) * (1.0 - metallic) * baseColor / PI;
        let ratio = distanceSq / rangeSq;
        let window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        let attenuation = window * window / (distanceSq + 1.0);
        sum += (diffuse + specular) * radiance * (attenuation * nDotL * shadow);
    }
    return sum;
}

fn acesToneMap(color: vec3f) -> vec3f {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((color * (a * color + vec3f(b))) /
                 (color * (c * color + vec3f(d)) + vec3f(e)), vec3f(0.0), vec3f(1.0));
}

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4f {
    let flags = i32(scene.ambientFlags.y + 0.5);
    let baseSample = textureSample(baseColorTexture, baseColorSampler, input.uv);
    let vertexColor = select(vec4f(1.0), input.color, (flags & 4) != 0);
    let base = vec4f(srgbToLinear(baseSample.rgb), baseSample.a) *
               scene.baseColorFactor * vertexColor;
    if ((flags & 1) != 0 && base.a < scene.materialParams.w) { discard; }

    let emissive = srgbToLinear(textureSample(emissiveTexture, emissiveSampler,
                                               input.uv).rgb) *
                   scene.emissiveMetallic.rgb;
    var color: vec3f;
    if ((flags & 2) != 0) {
        color = base.rgb + emissive;
    } else {
        let mr = textureSample(metallicRoughnessTexture, metallicRoughnessSampler, input.uv);
        let metallic = clamp(mr.b * scene.emissiveMetallic.w, 0.0, 1.0);
        let roughness = clamp(mr.g * scene.lightColorRoughness.w, 0.045, 1.0);
        let v = normalize(scene.cameraExposure.xyz - input.worldPosition);
        var geometricNormal = normalize(cross(dpdx(input.worldPosition),
                                               dpdy(input.worldPosition)));
        if ((flags & 8) != 0) {
            geometricNormal = normalize(input.worldNormal);
            if (!input.frontFacing) { geometricNormal = -geometricNormal; }
        } else if (dot(geometricNormal, v) < 0.0) {
            geometricNormal = -geometricNormal;
        }
        let n = mappedNormal(input, geometricNormal);
        let l = normalize(-scene.lightDirectionIntensity.xyz);
        let h = normalize(v + l);
        let nDotV = max(dot(n, v), 0.0);
        let nDotL = max(dot(n, l), 0.0);
        let nDotH = max(dot(n, h), 0.0);
        let hDotV = max(dot(h, v), 0.0);
        let f0 = mix(vec3f(0.04), base.rgb, vec3f(metallic));
        let f = fresnelSchlick(hDotV, f0);
        let distribution = distributionGgx(nDotH, roughness);
        let geometry = geometrySmith(nDotV, nDotL, roughness);
        let specular = distribution * geometry * f /
                       max(4.0 * nDotV * nDotL, 1.0e-5);
        let diffuse = (vec3f(1.0) - f) * (1.0 - metallic) * base.rgb / PI;
        let radiance = scene.lightColorRoughness.rgb * scene.lightDirectionIntensity.w;
        let direct = (diffuse + specular) * radiance * nDotL *
                     shadowVisibility(input, nDotL);

        let pointDirect = pointLightsDirect(input.worldPosition, n, v, base.rgb, metallic,
                                            roughness, f0);

        let reflection = reflect(-v, n);
        let irradiance = textureSampleLevel(environmentTexture, environmentSampler,
                                            environmentUv(n), scene.renderParams.w).rgb;
        let prefiltered = textureSampleLevel(environmentTexture, environmentSampler,
                                             environmentUv(reflection),
                                             roughness * scene.renderParams.w).rgb;
        let envF = fresnelSchlickRoughness(nDotV, f0, roughness);
        let c0 = vec4f(-1.0, -0.0275, -0.572, 0.022);
        let c1 = vec4f(1.0, 0.0425, 1.04, -0.04);
        let r = roughness * c0 + c1;
        let a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
        let ab = vec2f(-1.04, 1.04) * a004 + r.zw;
        let indirectSpecular = prefiltered * (f0 * ab.x + vec3f(ab.y));
        let indirectDiffuse = (vec3f(1.0) - envF) * (1.0 - metallic) *
                              base.rgb * irradiance;
        let ao = mix(1.0, textureSample(occlusionTexture, occlusionSampler,
                                        input.uv).r, scene.materialParams.z);
        color = direct + pointDirect + (indirectDiffuse + indirectSpecular) * ao *
                         scene.ambientFlags.x + emissive;
    }
    let exposed = color * scene.cameraExposure.w;
    // ambientFlags.z: write linear HDR for the bloom chain, which tone maps at
    // composite time; otherwise tone map + encode here exactly as before.
    if (scene.ambientFlags.z > 0.5) { return vec4f(exposed, base.a); }
    return vec4f(linearToSrgb(acesToneMap(exposed)), base.a);
}
