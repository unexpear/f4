#version 450

// Fullscreen triangle for the bloom chain; positions are already in clip space.
layout(location = 0) in vec2 aPosition;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
