ByteAddressBuffer inputsBuffer : register(t0, space0);

float probe(float inputValue) {
    const float a = inputValue * 0.75;
    const float b = inputValue * inputValue + 0.125;
    return (a + b) / (abs(inputValue) + 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint2 pixel = uint2(position.xy);
    const uint base = (pixel.y * 8 + pixel.x) * 4;
    return float4(probe(asfloat(inputsBuffer.Load((base + 0) * 4))),
                  probe(asfloat(inputsBuffer.Load((base + 1) * 4))),
                  probe(asfloat(inputsBuffer.Load((base + 2) * 4))),
                  probe(asfloat(inputsBuffer.Load((base + 3) * 4))));
}
