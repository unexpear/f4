#include <metal_stdlib>
using namespace metal;

struct CubeUniforms {
    float4x4 viewProj;
    float4x4 model;
    float4 lightDir;
    float4 viewPos;
};

struct CubeFragmentInput {
    float4 position [[position]];
    float3 normal;
    float3 worldPosition;
    float3 albedo;
};

fragment float4 cube_fragment(
    CubeFragmentInput input [[stage_in]],
    constant CubeUniforms& uniforms [[buffer(16)]]) [[color(0)]] {
    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(-uniforms.lightDir.xyz);
    const float3 viewDirection = normalize(uniforms.viewPos.xyz - input.worldPosition);
    const float diffuseAmount = max(dot(normal, lightDirection), 0.0f);
    const float3 reflected = reflect(-lightDirection, normal);
    const float specularAmount = pow(max(dot(viewDirection, reflected), 0.0f), 24.0f);
    const float3 ambient = float3(0.45f, 0.45f, 0.48f);
    const float3 diffuse = diffuseAmount * float3(0.55f, 0.45f, 0.35f);
    const float3 specular = specularAmount * float3(0.35f);
    return float4(input.albedo * (ambient + diffuse) + specular, 1.0f);
}
