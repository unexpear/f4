#version 450

layout(set = 0, binding = 0, std140) uniform Params {
    vec4 offset;
} params;

layout(location = 0) out vec2 uv;

const vec2 positions[3] = vec2[](
    vec2(-0.8, -0.8),
    vec2( 0.8, -0.8),
    vec2( 0.0,  0.8)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex] + params.offset.xy, 0.0, 1.0);
    uv = vec2(0.5);
}
