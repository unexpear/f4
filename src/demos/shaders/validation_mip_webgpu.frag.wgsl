@group(0) @binding(1) var level_zero_texture: texture_2d<f32>;
@group(0) @binding(2) var level_zero_sampler: sampler;
@group(0) @binding(5) var mip_texture: texture_2d<f32>;
@group(0) @binding(6) var mip_sampler: sampler;

@fragment
fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let mipmapped = uv.x >= 0.5;
    let local_x = fract(uv.x * 2.0) * 2.0 - 1.0;
    let horizon_distance = max(0.875 - uv.y, 0.012);
    let perspective = 1.0 / horizon_distance;
    let checker_uv = vec2<f32>(local_x * perspective * 2.0, perspective * 3.0);
    let gradient_x = dpdx(checker_uv);
    let gradient_y = dpdy(checker_uv);
    let level_zero = textureSampleGrad(level_zero_texture, level_zero_sampler, checker_uv,
                                       gradient_x, gradient_y).rgb;
    let filtered = textureSampleGrad(mip_texture, mip_sampler, checker_uv,
                                     gradient_x, gradient_y).rgb;
    if (uv.y > 0.86) {
        let marker = select(vec3<f32>(0.85, 0.18, 0.08), vec3<f32>(0.08, 0.65, 0.32),
                            mipmapped);
        return vec4<f32>(marker, 1.0);
    }
    let fog = smoothstep(8.0, 65.0, perspective);
    var color = mix(select(level_zero, filtered, mipmapped), vec3<f32>(0.16, 0.19, 0.22),
                    fog * 0.55);
    if (abs(uv.x - 0.5) < 0.003) {
        color = vec3<f32>(0.95);
    }
    return vec4<f32>(color, 1.0);
}
