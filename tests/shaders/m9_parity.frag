#version 450

layout(set = 0, binding = 3, std430) readonly buffer Inputs {
    float values[];
} inputsBuffer;

layout(location = 0) out vec4 outColor;

float probe(float inputValue) {
    float a = inputValue * 0.75;
    float b = inputValue * inputValue + 0.125;
    return (a + b) / (abs(inputValue) + 1.0);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int base = (pixel.y * 8 + pixel.x) * 4;
    outColor = vec4(probe(inputsBuffer.values[base + 0]),
                    probe(inputsBuffer.values[base + 1]),
                    probe(inputsBuffer.values[base + 2]),
                    probe(inputsBuffer.values[base + 3]));
}
