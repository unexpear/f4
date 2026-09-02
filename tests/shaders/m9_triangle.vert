#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 0) out vec3 color;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    color = vec3(0.15, 0.75, 0.35);
}
