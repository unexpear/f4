#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOutput {
    float4 position [[position]];
    float4 color;
};

vertex VertexOutput validation_screen_color_vertex(VertexInput input [[stage_in]]) {
    VertexOutput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.color = input.color;
    return output;
}
