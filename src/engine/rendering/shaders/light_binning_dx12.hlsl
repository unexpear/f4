// Clustered point-light binning on the GPU; HLSL port of light_binning.comp
// (see that file and clustered_lights.h for the layout). Storage slots 0/1
// are UAVs u0/u1; the parameters are cbuffer b0.

cbuffer BinParams : register(b0, space0) {
    uint4 counts;  // clusterCount, visible, copyWords, maskWords
    uint4 offsets; // maskOffset, gridOffset, 0, 0
    uint4 grid;    // tilesX, tilesY, slices, 0
};

RWByteAddressBuffer src : register(u0, space0);
RWByteAddressBuffer dst : register(u1, space0);

uint loadSrc(uint word) { return src.Load(word * 4u); }
void storeDst(uint word, uint value) { dst.Store(word * 4u, value); }

[numthreads(64, 1, 1)]
void CSMain(uint3 globalId : SV_DispatchThreadID) {
    uint id = globalId.x;
    if (id < counts.z) storeDst(id, loadSrc(id));
    uint clusterCount = counts.x;
    if (id >= clusterCount) return;
    uint gridOffset = offsets.y;
    uint first = loadSrc(gridOffset + id * 2u);
    uint allowed = loadSrc(gridOffset + id * 2u + 1u);
    if (allowed == 0u) return;
    uint maskWords = counts.w;
    uint tilesX = grid.x;
    uint tilesY = grid.y;
    uint x = id % tilesX;
    uint y = (id / tilesX) % tilesY;
    uint z = id / (tilesX * tilesY);
    uint rowX = offsets.x + x * maskWords;
    uint rowY = offsets.x + (tilesX + y) * maskWords;
    uint rowZ = offsets.x + (tilesX + tilesY + z) * maskWords;
    uint written = 0u;
    for (uint word = 0u; word < maskWords && written < allowed; ++word) {
        uint bits = loadSrc(rowX + word) & loadSrc(rowY + word) & loadSrc(rowZ + word);
        while (bits != 0u && written < allowed) {
            uint bit = firstbitlow(bits);
            storeDst(first + written, word * 32u + bit);
            ++written;
            bits &= bits - 1u;
        }
    }
}
