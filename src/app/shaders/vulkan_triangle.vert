#version 450

layout(location = 0) out vec3 color;

const vec2 positions[3] = vec2[](
    vec2(-0.82, -0.72),
    vec2( 0.82, -0.58),
    vec2(-0.08,  0.80)
);

const vec3 colors[3] = vec3[](
    vec3(0.98, 0.28, 0.12),
    vec3(0.10, 0.80, 0.72),
    vec3(1.00, 0.82, 0.22)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    color = colors[gl_VertexIndex];
}
