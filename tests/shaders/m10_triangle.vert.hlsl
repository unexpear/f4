struct VertexInput {
    float2 position : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    return output;
}
