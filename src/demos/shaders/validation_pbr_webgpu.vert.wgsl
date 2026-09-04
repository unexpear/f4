struct Scene {
    view_proj: mat4x4<f32>,
    camera_pos: vec4<f32>,
    light_pos: vec4<f32>,
};

@group(0) @binding(0) var<uniform> scene: Scene;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) offset: vec3<f32>,
    @location(3) material: vec3<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) world_position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) material: vec3<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.world_position = input.position + input.offset;
    output.normal = input.normal;
    output.material = input.material;
    output.position = scene.view_proj * vec4<f32>(output.world_position, 1.0);
    return output;
}
