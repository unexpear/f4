#version 450

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vFaceClip;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in vec4 vLight;
layout(location = 4) flat in vec3 vAlphaParams;
layout(set = 0, binding = 1) uniform texture2D uTex;
layout(set = 0, binding = 2) uniform sampler uSmp;
layout(location = 0) out float outDistance;

void main() {
    vec2 local = vFaceClip.xy / vFaceClip.z;
    if (abs(local.x) > 1.0 || abs(local.y) > 1.0) discard;
    if (vAlphaParams.z > 0.5 &&
        texture(sampler2D(uTex, uSmp), vUv).a * vAlphaParams.x < vAlphaParams.y) {
        discard;
    }
    outDistance = clamp(length(vWorldPosition - vLight.xyz) * vLight.w, 0.0, 1.0);
}
