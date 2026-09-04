#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOutput validation_fullscreen_vertex(uint vertexId [[vertex_id]]) {
    const float2 positions[] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    const float2 point = positions[vertexId];
    VertexOutput output;
    output.position = float4(point, 0.0f, 1.0f);
    output.uv = point * 0.5f + 0.5f;
    return output;
}
