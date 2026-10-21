#include <metal_stdlib>
using namespace metal;

// Fullscreen triangle for the bloom chain; positions are already in clip space.
struct VertexInput {
    float2 position [[attribute(0)]];
};

struct VertexOutput {
    float4 position [[position]];
};

vertex VertexOutput post_bloom_vertex(VertexInput input [[stage_in]]) {
    VertexOutput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    return output;
}
