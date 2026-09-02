#version 450

layout(location = 0) in vec3 color;
layout(location = 0) out vec4 outColor;

void main() {
    float scanline = 0.96 + 0.04 * step(1.0, mod(floor(gl_FragCoord.y / 3.0), 2.0));
    outColor = vec4(color * scanline, 1.0);
}
