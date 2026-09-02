#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;

layout(set = 0, binding = 0, std140) uniform SceneUbo {
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

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;
layout(location = 4) out vec4 vLightClip;

void main() {
    vec4 world = model * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(normalMatrix) * aNormal;
    vUv = aUv;
    vColor = aColor;
    vLightClip = lightViewProj * world;
    gl_Position = viewProj * world;
}
