#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
    float4 position [[position]];
    float3 normal;
    float4 albedo;
};

struct FragmentOutput {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
};

fragment FragmentOutput validation_gbuffer_fragment(FragmentInput input [[stage_in]]) {
    FragmentOutput output;
    output.albedo = input.albedo;
    output.normal = float4(normalize(input.normal) * 0.5f + 0.5f, 1.0f);
    return output;
}
