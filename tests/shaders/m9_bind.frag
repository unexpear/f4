#version 450

layout(set = 0, binding = 1) uniform texture2D sourceTexture;
layout(set = 0, binding = 2) uniform sampler sourceSampler;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sampler2D(sourceTexture, sourceSampler), uv);
}
