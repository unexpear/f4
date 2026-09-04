#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 position [[position]];
    float4 color;
};

fragment float4 validation_vertex_color_fragment(FragmentInput input [[stage_in]])
    [[color(0)]] {
    return input.color;
}
