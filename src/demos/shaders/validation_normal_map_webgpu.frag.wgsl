@group(0) @binding(1) var normal_texture: texture_2d<f32>;
@group(0) @binding(2) var normal_sampler: sampler;

@fragment
fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let flipped = uv.x >= 0.5;
    let panel_uv = vec2<f32>(fract(uv.x * 2.0), uv.y);
    let tiled_uv = panel_uv * vec2<f32>(4.0);
    var tangent_normal = textureSample(normal_texture, normal_sampler, tiled_uv).xyz * 2.0 -
                         vec3<f32>(1.0);
    if (flipped) {
        tangent_normal.y = -tangent_normal.y;
    }
    let normal = normalize(tangent_normal);
    let light_direction = normalize(vec3<f32>(-0.45, 0.75, 1.0));
    let view_direction = vec3<f32>(0.0, 0.0, 1.0);
    let diffuse = max(dot(normal, light_direction), 0.0);
    let specular = pow(max(dot(reflect(-light_direction, normal), view_direction), 0.0), 32.0);
    let stripe = 0.5 + 0.5 * sin(tiled_uv.x * 3.14159);
    let base = mix(vec3<f32>(0.07, 0.24, 0.42), vec3<f32>(0.12, 0.55, 0.68), stripe);
    var color = base * (0.16 + 0.84 * diffuse) + vec3<f32>(0.7) * specular;
    if (uv.y > 0.92) {
        color = select(vec3<f32>(0.08, 0.68, 0.35), vec3<f32>(0.86, 0.2, 0.1), flipped);
    }
    if (abs(uv.x - 0.5) < 0.003) {
        color = vec3<f32>(0.95);
    }
    return vec4<f32>(pow(color, vec3<f32>(1.0 / 2.2)), 1.0);
}
