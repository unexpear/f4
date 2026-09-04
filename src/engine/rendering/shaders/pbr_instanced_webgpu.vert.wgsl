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
    @location(9) model0: vec4f,
    @location(10) model1: vec4f,
    @location(11) model2: vec4f,
    @location(12) model3: vec4f,
    @location(13) normal0: vec4f,
    @location(14) normal1: vec4f,
    @location(15) normal2: vec4f,
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
    let world = input.model0 * input.position.x + input.model1 * input.position.y +
                input.model2 * input.position.z + input.model3;
    var output: VertexOutput;
    output.position = scene.viewProj * world;
    output.worldPosition = world.xyz;
    output.worldNormal = input.normal0.xyz * input.normal.x +
                         input.normal1.xyz * input.normal.y +
                         input.normal2.xyz * input.normal.z;
    output.uv = input.uv;
    output.color = input.color;
    output.lightClip = scene.lightViewProj * world;
    return output;
}
