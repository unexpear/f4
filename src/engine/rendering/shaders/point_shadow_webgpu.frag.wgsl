@group(0) @binding(1) var baseColorTexture: texture_2d<f32>;
@group(0) @binding(2) var baseColorSampler: sampler;

struct FragmentInput {
    @location(0) worldPosition: vec3f,
    @location(1) faceClip: vec3f,
    @location(2) uv: vec2f,
    @location(3) @interpolate(flat) light: vec4f,
    @location(4) @interpolate(flat) alphaParams: vec3f,
};

@fragment
fn fs_main(input: FragmentInput) -> @location(0) f32 {
    let baseAlpha = textureSample(baseColorTexture, baseColorSampler, input.uv).a;
    let local = input.faceClip.xy / input.faceClip.z;
    if (abs(local.x) > 1.0 || abs(local.y) > 1.0) {
        discard;
    }
    if (input.alphaParams.z > 0.5 &&
        baseAlpha * input.alphaParams.x < input.alphaParams.y) {
        discard;
    }
    return clamp(length(input.worldPosition - input.light.xyz) * input.light.w, 0.0, 1.0);
}
