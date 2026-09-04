struct SceneUbo {
    viewProj: mat4x4<f32>,
    model: mat4x4<f32>,
    normalMatrix: mat4x4<f32>,
    lightViewProj: mat4x4<f32>,
    baseColorFactor: vec4f,
    emissiveMetallic: vec4f,
    cameraExposure: vec4f,
    lightDirectionIntensity: vec4f,
    lightColorRoughness: vec4f,
    materialParams: vec4f,
    renderParams: vec4f,
    ambientFlags: vec4f,
};

@group(0) @binding(0) var<uniform> scene: SceneUbo;

struct VertexInput {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec4f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) worldPosition: vec3f,
    @location(1) worldNormal: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec4f,
    @location(4) lightClip: vec4f,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    let world = scene.model * vec4f(input.position, 1.0);
    let normalTransform = mat3x3<f32>(scene.normalMatrix[0].xyz,
                                      scene.normalMatrix[1].xyz,
                                      scene.normalMatrix[2].xyz);
    var output: VertexOutput;
    output.position = scene.viewProj * world;
    output.worldPosition = world.xyz;
    output.worldNormal = normalTransform * input.normal;
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = scene.lightViewProj * world;
    return output;
}
