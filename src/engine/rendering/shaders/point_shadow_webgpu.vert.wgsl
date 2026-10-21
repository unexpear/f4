// Point-light shadow face: see engine/lighting/point_shadows.h.
struct PointShadowUbo {
    faceViewProj: mat4x4<f32>,
    reserved: mat4x4<f32>, // unused: casters are instanced
    lightPositionInvRange: vec4f,
    tileTransform: vec4f,
    alphaParams: vec4f,
};

@group(0) @binding(0) var<uniform> pointShadow: PointShadowUbo;

struct VertexInput {
    @location(0) position: vec3f,
    @location(2) uv: vec2f,
    // Per-instance caster transform (same instance stream as the PBR pass).
    @location(9) model0: vec4f,
    @location(10) model1: vec4f,
    @location(11) model2: vec4f,
    @location(12) model3: vec4f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) worldPosition: vec3f,
    @location(1) faceClip: vec3f,
    @location(2) uv: vec2f,
    @location(3) @interpolate(flat) light: vec4f,
    @location(4) @interpolate(flat) alphaParams: vec3f,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let world = input.model0 * input.position.x + input.model1 * input.position.y +
                input.model2 * input.position.z + input.model3;
    let clip = pointShadow.faceViewProj * world;
    output.worldPosition = world.xyz;
    output.faceClip = clip.xyw;
    output.uv = input.uv;
    output.light = pointShadow.lightPositionInvRange;
    output.alphaParams = pointShadow.alphaParams.xyz;
    output.position = vec4f(clip.xy * pointShadow.tileTransform.xy +
                            pointShadow.tileTransform.zw * clip.w, clip.z, clip.w);
    return output;
}
