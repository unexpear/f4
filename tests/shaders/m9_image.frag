#version 450

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    outColor = ((pixel.x + pixel.y) & 1) == 0
                   ? vec4(1.0, 0.0, 0.0, 1.0)
                   : vec4(0.0, 1.0, 0.0, 1.0);
}
