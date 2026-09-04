#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOutput bind_vertex(uint vertexId [[vertex_id]]) {
    constexpr float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    output.uv = positions[vertexId] * float2(0.5f, -0.5f) + 0.5f;
    return output;
}

fragment float4 bind_fragment(VertexOutput input [[stage_in]],
                              constant float4& tint [[buffer(16)]],
                              texture2d<float> source [[texture(0)]],
                              sampler sourceSampler [[sampler(0)]]) [[color(0)]] {
    return source.sample(sourceSampler, input.uv) * tint;
}
