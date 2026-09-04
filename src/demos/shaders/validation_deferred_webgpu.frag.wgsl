@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var albedo_sampler: sampler;
@group(0) @binding(5) var normal_texture: texture_2d<f32>;
@group(0) @binding(6) var normal_sampler: sampler;

@fragment
fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let albedo_sample = textureSample(albedo_texture, albedo_sampler, uv);
    let encoded_normal = textureSample(normal_texture, normal_sampler, uv).xyz;
    if (albedo_sample.a < 0.5) {
        let low = vec3<f32>(0.018, 0.032, 0.065);
        let high = vec3<f32>(0.11, 0.19, 0.28);
        return vec4<f32>(mix(low, high, uv.y), 1.0);
    }
    let normal = normalize(encoded_normal * 2.0 - vec3<f32>(1.0));
    let light_direction = normalize(vec3<f32>(-0.45, 0.8, 0.55));
    let diffuse = max(dot(normal, light_direction), 0.0);
    let rim = pow(1.0 - max(normal.z, 0.0), 2.0);
    let color = albedo_sample.rgb * (0.16 + 0.84 * diffuse) + vec3<f32>(0.12) * rim;
    return vec4<f32>(pow(color, vec3<f32>(1.0 / 2.2)), 1.0);
}
