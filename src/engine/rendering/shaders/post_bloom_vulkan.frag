#version 450

layout(set = 0, binding = 0, std140) uniform PostUbo {
    vec4 params0; // source texel size xy, threshold, knee
    vec4 params1; // mode, intensity, 1 / target size xy
};
layout(set = 0, binding = 1) uniform texture2D uTex;
layout(set = 0, binding = 2) uniform sampler uSmp;
layout(set = 0, binding = 5) uniform texture2D uTex1;
layout(set = 0, binding = 6) uniform sampler uSmp1;
layout(location = 0) out vec4 outColor;

#define SAMPLE0(p) textureLod(sampler2D(uTex, uSmp), (p), 0.0)
#define SAMPLE1(p) textureLod(sampler2D(uTex1, uSmp1), (p), 0.0)

// HDR bloom post chain: see Renderer::recordBloom. One shader, four modes
// chosen by params1.x: 0 prefilter (threshold + 2x downsample), 1 downsample,
// 2 upsample (tent of the coarser level + this level), 3 composite (scene +
// bloom * intensity, then the same ACES + sRGB encode as the PBR pass).
// Texture coordinates come from the pixel position, so no backend needs a
// flip: every API rasterizes and samples its own render targets the same way.
vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(c, vec3(0.0031308)));
}

vec3 acesToneMap(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec2 uv = gl_FragCoord.xy * params1.zw;
    int mode = int(params1.x + 0.5);
    vec2 t = params0.xy;
    if (mode <= 1) {
        vec3 c = (SAMPLE0(uv + vec2(-t.x, -t.y)) + SAMPLE0(uv + vec2(t.x, -t.y)) +
                  SAMPLE0(uv + vec2(-t.x, t.y)) + SAMPLE0(uv + t)).rgb * 0.25;
        if (mode == 0) {
            c = min(c, vec3(64.0));
            float brightness = max(c.r, max(c.g, c.b));
            float knee = params0.w;
            float soft = clamp(brightness - params0.z + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1.0e-5);
            c *= max(soft, brightness - params0.z) / max(brightness, 1.0e-5);
        }
        outColor = vec4(c, 1.0);
    } else if (mode == 2) {
        vec3 s = SAMPLE0(uv + vec2(-t.x, -t.y)).rgb + SAMPLE0(uv + vec2(t.x, -t.y)).rgb +
                 SAMPLE0(uv + vec2(-t.x, t.y)).rgb + SAMPLE0(uv + t).rgb;
        s += 2.0 * (SAMPLE0(uv + vec2(0.0, -t.y)).rgb + SAMPLE0(uv + vec2(-t.x, 0.0)).rgb +
                    SAMPLE0(uv + vec2(t.x, 0.0)).rgb + SAMPLE0(uv + vec2(0.0, t.y)).rgb);
        s += 4.0 * SAMPLE0(uv).rgb;
        outColor = vec4(SAMPLE1(uv).rgb + s / 16.0, 1.0);
    } else {
        vec4 scene = SAMPLE0(uv);
        vec3 color = scene.rgb + SAMPLE1(uv).rgb * params1.y;
        outColor = vec4(linearToSrgb(acesToneMap(color)), scene.a);
    }
}
