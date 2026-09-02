#version 430 core

in float vDepth;
in vec2 vUv;
flat in vec3 vAlphaParams;
uniform sampler2D uTex;
layout(location = 0) out float outDepth;

void main() {
    if (vAlphaParams.z > 0.5 && texture(uTex, vUv).a * vAlphaParams.x < vAlphaParams.y) {
        discard;
    }
    outDepth = clamp(vDepth, 0.0, 1.0);
}
