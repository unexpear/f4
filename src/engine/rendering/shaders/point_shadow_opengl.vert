#version 430 core

// Point-light shadow face: see engine/lighting/point_shadows.h.
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aUv;
// Per-instance caster transform (same instance stream as the PBR pass).
layout(location = 9) in vec4 iModel0;
layout(location = 10) in vec4 iModel1;
layout(location = 11) in vec4 iModel2;
layout(location = 12) in vec4 iModel3;
layout(std140, binding = 0) uniform PointShadowUbo {
    mat4 faceViewProj;
    mat4 reserved; // unused: casters are instanced
    vec4 lightPositionInvRange;
    vec4 tileTransform;
    vec4 alphaParams;
};
out vec3 vWorldPosition;
out vec3 vFaceClip;
out vec2 vUv;
flat out vec4 vLight;
flat out vec3 vAlphaParams;

void main() {
    vec4 world = iModel0 * aPosition.x + iModel1 * aPosition.y + iModel2 * aPosition.z +
                 iModel3;
    vec4 clip = faceViewProj * world;
    vWorldPosition = world.xyz;
    vFaceClip = clip.xyw;
    vUv = aUv;
    vLight = lightPositionInvRange;
    vAlphaParams = alphaParams.xyz;
    gl_Position = vec4(clip.xy * tileTransform.xy + tileTransform.zw * clip.w, clip.z, clip.w);
}
