#version 430 core

layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aUv;
layout(std140, binding = 0) uniform ShadowUbo {
    mat4 lightViewProj;
    mat4 model;
    vec4 alphaParams;
};
out float vDepth;
out vec2 vUv;
flat out vec3 vAlphaParams;

void main() {
    gl_Position = lightViewProj * model * vec4(aPosition, 1.0);
    vDepth = gl_Position.z / gl_Position.w;
    vUv = aUv;
    vAlphaParams = alphaParams.xyz;
}
