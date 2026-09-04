@group(0) @binding(1) var baseColorTexture: texture_2d<f32>;
@group(0) @binding(2) var baseColorSampler: sampler;

struct FragmentInput {
    @location(0) depth: f32,
    @location(1) uv: vec2f,
    @location(2) @interpolate(flat) alphaParams: vec3f,
};

@fragment
fn fs_main(input: FragmentInput) -> @location(0) f32 {
    let baseAlpha = textureSample(baseColorTexture, baseColorSampler, input.uv).a;
    if (input.alphaParams.z > 0.5 &&
        baseAlpha * input.alphaParams.x < input.alphaParams.y) {
        discard;
    }
    return clamp(input.depth, 0.0, 1.0);
}
