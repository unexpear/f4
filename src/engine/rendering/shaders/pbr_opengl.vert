#version 430 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;

layout(std140, binding = 0) uniform SceneUbo {
    mat4 viewProj;
    mat4 model;
    mat4 normalMatrix;
    mat4 lightViewProj;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 cameraExposure;
    vec4 lightDirectionIntensity;
    vec4 lightColorRoughness;
    vec4 materialParams;
    vec4 renderParams;
    vec4 ambientFlags;
};

out vec3 vWorldPosition;
out vec3 vWorldNormal;
out vec2 vUv;
out vec4 vColor;
out vec4 vLightClip;

void main() {
    vec4 world = model * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(normalMatrix) * aNormal;
    vUv = aUv;
    vColor = aColor;
    vLightClip = lightViewProj * world;
    gl_Position = viewProj * world;
}
