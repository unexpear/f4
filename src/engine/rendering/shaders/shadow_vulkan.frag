#version 450

layout(location = 0) in float vDepth;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in vec3 vAlphaParams;
layout(set = 0, binding = 1) uniform texture2D uTex;
layout(set = 0, binding = 2) uniform sampler uSmp;
layout(location = 0) out float outDepth;

void main() {
    if (vAlphaParams.z > 0.5 &&
        texture(sampler2D(uTex, uSmp), vUv).a * vAlphaParams.x < vAlphaParams.y) {
        discard;
    }
    outDepth = clamp(vDepth, 0.0, 1.0);
}
