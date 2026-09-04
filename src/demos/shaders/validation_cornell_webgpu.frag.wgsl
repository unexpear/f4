struct Scene {
    view_proj: mat4x4<f32>,
    model: mat4x4<f32>,
    light_dir: vec3<f32>,
    view_pos: vec3<f32>,
};

@group(0) @binding(0) var<uniform> scene: Scene;

struct FragmentInput {
    @location(0) normal: vec3<f32>,
    @location(1) world_position: vec3<f32>,
    @location(2) color: vec4<f32>,
};

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4<f32> {
    let normal = normalize(input.normal);
    let light = normalize(-scene.light_dir);
    let view = normalize(scene.view_pos - input.world_position);
    let diffuse = max(dot(normal, light), 0.0);
    let half_vector = normalize(light + view);
    let specular = pow(max(dot(normal, half_vector), 0.0), 32.0);
    let color = input.color.rgb * (0.18 + 0.82 * diffuse) + vec3<f32>(0.18 * specular);
    return vec4<f32>(color, input.color.a);
}
