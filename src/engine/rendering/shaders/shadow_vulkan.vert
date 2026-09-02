#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aUv;
layout(set = 0, binding = 0, std140) uniform ShadowUbo {
    mat4 lightViewProj;
    mat4 model;
    vec4 alphaParams;
};
layout(location = 0) out float vDepth;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out vec3 vAlphaParams;

void main() {
    gl_Position = lightViewProj * model * vec4(aPosition, 1.0);
    vDepth = gl_Position.z / gl_Position.w;
    vUv = aUv;
    vAlphaParams = alphaParams.xyz;
}
