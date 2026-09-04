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
    @location(3) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) world_position: vec3<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    let world = scene.model * vec4<f32>(input.position, 1.0);
    var output: VertexOutput;
    output.position = scene.view_proj * world;
    output.color = input.color.rgb;
    output.normal = normalize(mat3x3<f32>(scene.model[0].xyz,
                                          scene.model[1].xyz,
                                          scene.model[2].xyz) * input.normal);
    output.world_position = world.xyz;
    return output;
}
