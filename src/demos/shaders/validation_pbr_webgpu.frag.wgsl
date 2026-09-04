struct Scene {
    view_proj: mat4x4<f32>,
    camera_pos: vec4<f32>,
    light_pos: vec4<f32>,
};

@group(0) @binding(0) var<uniform> scene: Scene;

const PI: f32 = 3.14159265359;

fn distribution_ggx(normal: vec3<f32>, halfway: vec3<f32>, roughness: f32) -> f32 {
    let alpha = roughness * roughness;
    let alpha_squared = alpha * alpha;
    let n_dot_h = max(dot(normal, halfway), 0.0);
    let denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
    return alpha_squared / max(PI * denominator * denominator, 0.0001);
}

fn geometry_schlick_ggx(n_dot_v: f32, roughness: f32) -> f32 {
    let radius = roughness + 1.0;
    let k = radius * radius / 8.0;
    return n_dot_v / max(n_dot_v * (1.0 - k) + k, 0.0001);
}

fn fresnel_schlick(cos_theta: f32, f0: vec3<f32>) -> vec3<f32> {
    return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - cos_theta, 5.0);
}

@fragment
fn fs_main(@location(0) world_position: vec3<f32>,
           @location(1) interpolated_normal: vec3<f32>,
           @location(2) material: vec3<f32>) -> @location(0) vec4<f32> {
    let metallic = clamp(material.x, 0.0, 1.0);
    let roughness = clamp(material.y, 0.06, 1.0);
    let albedo = mix(vec3<f32>(0.72, 0.08, 0.035), vec3<f32>(0.95, 0.58, 0.12),
                     material.z);
    let normal = normalize(interpolated_normal);
    let view_direction = normalize(scene.camera_pos.xyz - world_position);
    let to_light = scene.light_pos.xyz - world_position;
    let distance_squared = max(dot(to_light, to_light), 0.01);
    let light_direction = normalize(to_light);
    let halfway = normalize(view_direction + light_direction);
    let radiance = vec3<f32>(135.0) / distance_squared;
    let f0 = mix(vec3<f32>(0.04), albedo, metallic);
    let fresnel = fresnel_schlick(max(dot(halfway, view_direction), 0.0), f0);
    let distribution = distribution_ggx(normal, halfway, roughness);
    let geometry = geometry_schlick_ggx(max(dot(normal, view_direction), 0.0), roughness) *
                   geometry_schlick_ggx(max(dot(normal, light_direction), 0.0), roughness);
    let specular = distribution * geometry * fresnel /
                   max(4.0 * max(dot(normal, view_direction), 0.0) *
                       max(dot(normal, light_direction), 0.0), 0.001);
    let diffuse = (vec3<f32>(1.0) - fresnel) * (1.0 - metallic) * albedo / PI;
    var color = vec3<f32>(0.025) * albedo +
                (diffuse + specular) * radiance * max(dot(normal, light_direction), 0.0);
    color = color / (color + vec3<f32>(1.0));
    color = pow(color, vec3<f32>(1.0 / 2.2));
    return vec4<f32>(color, 1.0);
}
