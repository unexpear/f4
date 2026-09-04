#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float3 color;
};

vertex VertexOutput triangle_vertex(uint vertexId [[vertex_id]]) {
    constexpr float2 positions[3] = {
        float2(-0.72f, -0.62f), float2(0.72f, -0.62f), float2(0.0f, 0.72f)
    };
    constexpr float3 colors[3] = {
        float3(0.95f, 0.22f, 0.12f), float3(0.12f, 0.78f, 0.42f),
        float3(0.12f, 0.42f, 1.0f)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    output.color = colors[vertexId];
    return output;
}

fragment float4 triangle_fragment(VertexOutput input [[stage_in]]) [[color(0)]] {
    return float4(input.color, 1.0f);
}
