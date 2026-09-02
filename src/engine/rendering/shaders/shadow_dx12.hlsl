cbuffer ShadowUbo : register(b0, space0) {
    column_major float4x4 lightViewProj;
    column_major float4x4 model;
    float4 alphaParams;
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD2;
};

struct VertexOutput {
    float4 position : SV_Position;
    float depth : TEXCOORD0;
    float2 uv : TEXCOORD1;
    nointerpolation float3 alpha : TEXCOORD2;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(lightViewProj, mul(model, float4(input.position, 1.0)));
    output.depth = output.position.z / output.position.w;
    output.uv = input.uv;
    output.alpha = alphaParams.xyz;
    return output;
}

Texture2D<float4> baseColorTexture : register(t0, space0);
SamplerState baseColorSampler : register(s0, space0);

float PSMain(VertexOutput input) : SV_Target0 {
    if (input.alpha.z > 0.5 &&
        baseColorTexture.Sample(baseColorSampler, input.uv).a * input.alpha.x < input.alpha.y) {
        discard;
    }
    return saturate(input.depth);
}
