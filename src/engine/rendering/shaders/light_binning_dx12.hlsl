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

groupshared uint sScan[64];

uint loadSrc(uint word) { return src.Load(word * 4u); }
void storeDst(uint word, uint value) { dst.Store(word * 4u, value); }

[numthreads(64, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    uint lane = localId.x;
    uint group = groupId.x;
    uint copyWord = group * 64u + lane;
    if (copyWord < counts.z) storeDst(copyWord, loadSrc(copyWord));
    if (group >= counts.x) return; // whole workgroup
    uint gridOffset = offsets.y;
    uint first = loadSrc(gridOffset + group * 2u);
    uint allowed = loadSrc(gridOffset + group * 2u + 1u);
    if (allowed == 0u) return; // whole workgroup
    uint maskWords = counts.w;
    uint tilesX = grid.x;
    uint tilesY = grid.y;
    uint x = group % tilesX;
    uint y = (group / tilesX) % tilesY;
    uint z = group / (tilesX * tilesY);
    uint rowX = offsets.x + x * maskWords;
    uint rowY = offsets.x + (tilesX + y) * maskWords;
    uint rowZ = offsets.x + (tilesX + tilesY + z) * maskWords;
    uint base = 0u;
    for (uint chunk = 0u; chunk < maskWords && base < allowed; chunk += 64u) {
        uint word = chunk + lane;
        uint bits = 0u;
        if (word < maskWords) {
            bits = loadSrc(rowX + word) & loadSrc(rowY + word) & loadSrc(rowZ + word);
        }
        uint count = countbits(bits);
        sScan[lane] = count;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1u; offset < 64u; offset <<= 1u) {
            uint add = lane >= offset ? sScan[lane - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            sScan[lane] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        uint position = base + sScan[lane] - count;
        while (bits != 0u && position < allowed) {
            storeDst(first + position, word * 32u + firstbitlow(bits));
            ++position;
            bits &= bits - 1u;
        }
        base += sScan[63];
        GroupMemoryBarrierWithGroupSync();
    }
}
