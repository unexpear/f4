#include <metal_stdlib>
using namespace metal;

// HDR bloom post chain: see post_bloom_vulkan.frag for the four modes.
struct PostUbo {
    float4 params0; // source texel size xy, threshold, knee
    float4 params1; // mode, intensity, 1 / target size xy
};

struct VertexOutput {
    float4 position [[position]];
};

float3 linearToSrgb(float3 c) {
    c = max(c, float3(0.0f));
    const float3 low = c * 12.92f;
    const float3 high = 1.055f * pow(c, float3(1.0f / 2.4f)) - 0.055f;
    return select(high, low, c <= float3(0.0031308f));
}

float3 acesToneMap(float3 color) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0f, 1.0f);
}

#define S0(p) sourceTexture.sample(sourceSampler, (p), level(0.0f))
#define S1(p) secondTexture.sample(secondSampler, (p), level(0.0f))

fragment float4 post_bloom_fragment(VertexOutput input [[stage_in]],
                                    constant PostUbo& post [[buffer(16)]],
                                    texture2d<float> sourceTexture [[texture(0)]],
                                    sampler sourceSampler [[sampler(0)]],
                                    texture2d<float> secondTexture [[texture(1)]],
                                    sampler secondSampler [[sampler(1)]]) {
    const float2 uv = input.position.xy * post.params1.zw;
    const int mode = int(post.params1.x + 0.5f);
    const float2 t = post.params0.xy;
    if (mode <= 1) {
        float3 c = (S0(uv + float2(-t.x, -t.y)) + S0(uv + float2(t.x, -t.y)) +
                    S0(uv + float2(-t.x, t.y)) + S0(uv + t)).rgb * 0.25f;
        if (mode == 0) {
            c = min(c, float3(64.0f));
            const float brightness = max(c.r, max(c.g, c.b));
            const float knee = post.params0.w;
            float soft = clamp(brightness - post.params0.z + knee, 0.0f, 2.0f * knee);
            soft = soft * soft / (4.0f * knee + 1.0e-5f);
            c *= max(soft, brightness - post.params0.z) / max(brightness, 1.0e-5f);
        }
        return float4(c, 1.0f);
    }
    if (mode == 2) {
        float3 s = S0(uv + float2(-t.x, -t.y)).rgb + S0(uv + float2(t.x, -t.y)).rgb +
                   S0(uv + float2(-t.x, t.y)).rgb + S0(uv + t).rgb;
        s += 2.0f * (S0(uv + float2(0.0f, -t.y)).rgb + S0(uv + float2(-t.x, 0.0f)).rgb +
                     S0(uv + float2(t.x, 0.0f)).rgb + S0(uv + float2(0.0f, t.y)).rgb);
        s += 4.0f * S0(uv).rgb;
        return float4(S1(uv).rgb + s / 16.0f, 1.0f);
    }
    const float4 scene = S0(uv);
    const float3 color = scene.rgb + S1(uv).rgb * post.params1.y;
    return float4(linearToSrgb(acesToneMap(color)), scene.a);
}
