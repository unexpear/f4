#version 430 core

// Fullscreen triangle for the bloom chain; positions are already in clip space.
layout(location = 0) in vec2 aPosition;
// Texture coordinate of the pixel, top-down (v = 0 at NDC +y); the fragment
// shader uses it to tell the window from an offscreen target.
layout(location = 0) out vec2 vUv;

void main() {
    vUv = vec2(aPosition.x * 0.5 + 0.5, 0.5 - aPosition.y * 0.5);
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
