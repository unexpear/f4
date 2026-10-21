#version 430 core

in vec3 vWorldPosition;
in vec3 vFaceClip;
in vec2 vUv;
flat in vec4 vLight;
flat in vec3 vAlphaParams;
uniform sampler2D uTex;
layout(location = 0) out float outDistance;

void main() {
    vec2 local = vFaceClip.xy / vFaceClip.z;
    if (abs(local.x) > 1.0 || abs(local.y) > 1.0) discard;
    if (vAlphaParams.z > 0.5 && texture(uTex, vUv).a * vAlphaParams.x < vAlphaParams.y) {
        discard;
    }
    outDistance = clamp(length(vWorldPosition - vLight.xyz) * vLight.w, 0.0, 1.0);
}
