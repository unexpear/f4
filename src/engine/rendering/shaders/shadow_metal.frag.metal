#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float depth;
    float2 uv;
    float3 alpha [[flat]];
};

fragment float shadow_fragment(VertexOutput input [[stage_in]],
                               texture2d<float> baseColorTexture [[texture(0)]],
                               sampler baseColorSampler [[sampler(0)]]) [[color(0)]] {
    if (input.alpha.z > 0.5f &&
        baseColorTexture.sample(baseColorSampler, input.uv).a * input.alpha.x < input.alpha.y) {
        discard_fragment();
    }
    return clamp(input.depth, 0.0f, 1.0f);
}
