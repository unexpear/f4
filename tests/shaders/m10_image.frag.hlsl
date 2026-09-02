float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint2 pixel = uint2(position.xy);
    return ((pixel.x + pixel.y) & 1) == 0
               ? float4(1.0, 0.0, 0.0, 1.0)
               : float4(0.0, 1.0, 0.0, 1.0);
}
