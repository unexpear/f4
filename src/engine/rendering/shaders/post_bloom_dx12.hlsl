// HDR bloom post chain: see post_bloom_vulkan.frag for the four modes.
cbuffer PostUbo : register(b0, space0) {
    float4 params0; // source texel size xy, threshold, knee
    float4 params1; // mode, intensity, 1 / target size xy
};

Texture2D<float4> sourceTexture : register(t0, space0);
SamplerState sourceSampler : register(s0, space0);
Texture2D<float4> secondTexture : register(t1, space0);
SamplerState secondSampler : register(s1, space0);

struct VertexInput {
    float2 position : TEXCOORD0;
};

float4 VSMain(VertexInput input) : SV_Position {
    return float4(input.position, 0.0, 1.0);
}

float4 sample0(float2 uv) { return sourceTexture.SampleLevel(sourceSampler, uv, 0.0); }
float4 sample1(float2 uv) { return secondTexture.SampleLevel(secondSampler, uv, 0.0); }

float linearChannelToSrgb(float value) {
    value = max(value, 0.0);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 linearToSrgb(float3 c) {
    return float3(linearChannelToSrgb(c.x), linearChannelToSrgb(c.y),
                  linearChannelToSrgb(c.z));
}

float3 acesToneMap(float3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const float2 uv = position.xy * params1.zw;
    const int mode = (int)(params1.x + 0.5);
    const float2 t = params0.xy;
    if (mode <= 1) {
        float3 c = (sample0(uv + float2(-t.x, -t.y)) + sample0(uv + float2(t.x, -t.y)) +
                    sample0(uv + float2(-t.x, t.y)) + sample0(uv + t)).rgb * 0.25;
        if (mode == 0) {
            c = min(c, 64.0);
            const float brightness = max(c.r, max(c.g, c.b));
            const float knee = params0.w;
            float soft = clamp(brightness - params0.z + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1.0e-5);
            c *= max(soft, brightness - params0.z) / max(brightness, 1.0e-5);
        }
        return float4(c, 1.0);
    }
    if (mode == 2) {
        float3 s = sample0(uv + float2(-t.x, -t.y)).rgb + sample0(uv + float2(t.x, -t.y)).rgb +
                   sample0(uv + float2(-t.x, t.y)).rgb + sample0(uv + t).rgb;
        s += 2.0 * (sample0(uv + float2(0.0, -t.y)).rgb + sample0(uv + float2(-t.x, 0.0)).rgb +
                    sample0(uv + float2(t.x, 0.0)).rgb + sample0(uv + float2(0.0, t.y)).rgb);
        s += 4.0 * sample0(uv).rgb;
        return float4(sample1(uv).rgb + s / 16.0, 1.0);
    }
    const float4 scene = sample0(uv);
    const float3 color = scene.rgb + sample1(uv).rgb * params1.y;
    return float4(linearToSrgb(acesToneMap(color)), scene.a);
}
