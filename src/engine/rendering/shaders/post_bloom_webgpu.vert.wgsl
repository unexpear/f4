// Fullscreen triangle for the bloom chain; positions are already in clip space.
struct VertexInput {
    @location(0) position: vec2f,
};

@vertex
fn vs_main(input: VertexInput) -> @builtin(position) vec4f {
    return vec4f(input.position, 0.0, 1.0);
}
