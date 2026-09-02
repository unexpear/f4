Texture2D<float4> sourceTexture : register(t0, space0);
SamplerState sourceSampler : register(s0, space0);

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return sourceTexture.Sample(sourceSampler, uv);
}
