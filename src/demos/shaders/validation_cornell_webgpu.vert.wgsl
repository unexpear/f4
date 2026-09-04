struct Scene {
    view_proj: mat4x4<f32>,
    model: mat4x4<f32>,
    light_dir: vec3<f32>,
    view_pos: vec3<f32>,
};

@group(0) @binding(0) var<uniform> scene: Scene;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) normal: vec3<f32>,
    @location(1) world_position: vec3<f32>,
    @location(2) color: vec4<f32>,
};

fn normal_matrix(model: mat4x4<f32>) -> mat3x3<f32> {
    let a = model[0].xyz;
    let b = model[1].xyz;
    let c = model[2].xyz;
    let inverse_determinant = 1.0 / dot(a, cross(b, c));
    return mat3x3<f32>(cross(b, c) * inverse_determinant,
                       cross(c, a) * inverse_determinant,
                       cross(a, b) * inverse_determinant);
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    let world = scene.model * vec4<f32>(input.position, 1.0);
    var output: VertexOutput;
    output.position = scene.view_proj * world;
    output.normal = normal_matrix(scene.model) * input.normal;
    output.world_position = world.xyz;
    output.color = input.color;
    return output;
}
