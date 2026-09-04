struct Scene {
    view_proj: mat4x4<f32>,
    model: mat4x4<f32>,
    light_dir: vec3<f32>,
    view_pos: vec3<f32>,
};

@group(0) @binding(0) var<uniform> scene: Scene;

struct FragmentInput {
    @location(0) color: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) world_position: vec3<f32>,
};

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4<f32> {
    let normal = normalize(input.normal);
    let light = normalize(-scene.light_dir);
    let view = normalize(scene.view_pos - input.world_position);
    let diffuse = max(dot(normal, light), 0.0);
    let rim = pow(1.0 - max(dot(normal, view), 0.0), 3.0);
    let lit = input.color * (0.28 + 0.72 * diffuse) + vec3<f32>(0.08, 0.11, 0.14) * rim;
    return vec4<f32>(lit, 1.0);
}
