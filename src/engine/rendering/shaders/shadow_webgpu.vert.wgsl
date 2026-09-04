struct ShadowUbo {
    lightViewProj: mat4x4<f32>,
    model: mat4x4<f32>,
    alphaParams: vec4f,
};

@group(0) @binding(0) var<uniform> shadow: ShadowUbo;

struct VertexInput {
    @location(0) position: vec3f,
    @location(2) uv: vec2f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) depth: f32,
    @location(1) uv: vec2f,
    @location(2) @interpolate(flat) alphaParams: vec3f,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = shadow.lightViewProj * shadow.model * vec4f(input.position, 1.0);
    output.depth = output.position.z / output.position.w;
    output.uv = input.uv;
    output.alphaParams = shadow.alphaParams.xyz;
    return output;
}
