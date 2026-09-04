struct FragmentOutput {
    @location(0) albedo: vec4<f32>,
    @location(1) normal: vec4<f32>,
};

@fragment
fn fs_main(@location(0) interpolated_normal: vec3<f32>,
           @location(1) albedo: vec4<f32>) -> FragmentOutput {
    var output: FragmentOutput;
    output.albedo = albedo;
    output.normal = vec4<f32>(normalize(interpolated_normal) * 0.5 + vec3<f32>(0.5), 1.0);
    return output;
}
