struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-0.72, -0.62), float2(0.72, -0.62), float2(0.0, 0.72)
    };
    const float3 colors[3] = {
        float3(0.95, 0.22, 0.12), float3(0.12, 0.78, 0.42), float3(0.12, 0.42, 1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.color = colors[vertexId];
    return output;
}
