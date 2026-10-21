// Point-light shadow face: see engine/lighting/point_shadows.h.
cbuffer PointShadowUbo : register(b0, space0) {
    column_major float4x4 faceViewProj;
    column_major float4x4 reserved; // unused: casters are instanced
    float4 lightPositionInvRange;
    float4 tileTransform;
    float4 alphaParams;
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD2;
    // Per-instance caster transform columns (same stream as the PBR pass).
    float4 model0 : TEXCOORD9;
    float4 model1 : TEXCOORD10;
    float4 model2 : TEXCOORD11;
    float4 model3 : TEXCOORD12;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 faceClip : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation float4 light : TEXCOORD3;
    nointerpolation float3 alpha : TEXCOORD4;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    const float4 world = input.model0 * input.position.x + input.model1 * input.position.y +
                         input.model2 * input.position.z + input.model3;
    const float4 clip = mul(faceViewProj, world);
    output.worldPosition = world.xyz;
    output.faceClip = clip.xyw;
    output.uv = input.uv;
    output.light = lightPositionInvRange;
    output.alpha = alphaParams.xyz;
    output.position = float4(clip.xy * tileTransform.xy + tileTransform.zw * clip.w, clip.z,
                             clip.w);
    return output;
}

Texture2D<float4> baseColorTexture : register(t0, space0);
SamplerState baseColorSampler : register(s0, space0);

float PSMain(VertexOutput input) : SV_Target0 {
    const float2 local = input.faceClip.xy / input.faceClip.z;
    if (abs(local.x) > 1.0 || abs(local.y) > 1.0) discard;
    if (input.alpha.z > 0.5 &&
        baseColorTexture.Sample(baseColorSampler, input.uv).a * input.alpha.x < input.alpha.y) {
        discard;
    }
    return saturate(length(input.worldPosition - input.light.xyz) * input.light.w);
}
