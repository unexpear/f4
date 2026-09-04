#include <metal_stdlib>
using namespace metal;

struct Scene { float4x4 viewProj; };

struct VertexInput {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOutput {
    float4 position [[position]];
    float4 color;
};

vertex VertexOutput validation_world_color_vertex(
    VertexInput input [[stage_in]], constant Scene& scene [[buffer(16)]]) {
    VertexOutput output;
    output.position = scene.viewProj * float4(input.position, 1.0f);
    output.color = input.color;
    return output;
}
