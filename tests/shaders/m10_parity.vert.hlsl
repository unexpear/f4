float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)
    };
    return float4(positions[vertexId], 0.0, 1.0);
}
