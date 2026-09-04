struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    let positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0));
    let point = positions[vertex_index];
    var output: VertexOutput;
    output.position = vec4<f32>(point, 0.0, 1.0);
    output.uv = point * 0.5 + vec2<f32>(0.5);
    return output;
}
