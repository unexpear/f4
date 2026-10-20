// Clustered point-light binning on the GPU; HLSL port of light_binning.comp
// (see that file and clustered_lights.h for the passes and the layout).
// Storage slots 0/1 are UAVs u0/u1; the parameters are cbuffer b0.

cbuffer BinParams : register(b0, space0) {
    uint4 counts;  // pass, clusterCount, visible, copyWords
    uint4 offsets; // rangeOffset, maxIndexCount, gridOffset, indexOffset
    uint4 grid;    // tilesX, tilesY, slices, 0
};

RWByteAddressBuffer src : register(u0, space0);
RWByteAddressBuffer dst : register(u1, space0);

groupshared uint sRanges[64 * 3];
groupshared uint sScan[64];

uint loadSrc(uint word) { return src.Load(word * 4u); }
uint loadDst(uint word) { return dst.Load(word * 4u); }
void storeDst(uint word, uint value) { dst.Store(word * 4u, value); }

bool covers(uint i, uint3 cluster) {
    uint rx = sRanges[i * 3u];
    uint ry = sRanges[i * 3u + 1u];
    uint rz = sRanges[i * 3u + 2u];
    return cluster.x >= (rx & 0xffffu) && cluster.x <= (rx >> 16) &&
           cluster.y >= (ry & 0xffffu) && cluster.y <= (ry >> 16) &&
           cluster.z >= (rz & 0xffffu) && cluster.z <= (rz >> 16);
}

void binLights(bool fill, uint lane, uint cluster) {
    uint clusterCount = counts.y;
    uint visible = counts.z;
    uint rangeOffset = offsets.x;
    uint gridOffset = offsets.z;
    bool inGrid = cluster < clusterCount;
    uint3 coord = uint3(cluster % grid.x, (cluster / grid.x) % grid.y,
                        cluster / (grid.x * grid.y));
    uint first = 0u;
    uint allowed = 0u;
    if (fill && inGrid) {
        first = loadDst(gridOffset + cluster * 2u);
        allowed = loadDst(gridOffset + cluster * 2u + 1u);
    }
    uint count = 0u;
    for (uint base = 0u; base < visible; base += 64u) {
        uint light = base + lane;
        if (light < visible) {
            sRanges[lane * 3u] = loadSrc(rangeOffset + light * 3u);
            sRanges[lane * 3u + 1u] = loadSrc(rangeOffset + light * 3u + 1u);
            sRanges[lane * 3u + 2u] = loadSrc(rangeOffset + light * 3u + 2u);
        }
        GroupMemoryBarrierWithGroupSync();
        uint n = min(64u, visible - base);
        if (inGrid) {
            for (uint i = 0u; i < n; ++i) {
                if (covers(i, coord)) {
                    if (fill && count < allowed) storeDst(first + count, base + i);
                    ++count;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (!fill && inGrid) storeDst(gridOffset + cluster * 2u + 1u, count);
}

void scanCounts(uint lane) {
    uint clusterCount = counts.y;
    uint maxIndexCount = offsets.y;
    uint gridOffset = offsets.z;
    uint indexOffset = offsets.w;
    uint carry = 0u;
    for (uint base = 0u; base < clusterCount; base += 64u) {
        uint cluster = base + lane;
        uint count = cluster < clusterCount ? loadDst(gridOffset + cluster * 2u + 1u) : 0u;
        sScan[lane] = count;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1u; offset < 64u; offset <<= 1u) {
            uint add = lane >= offset ? sScan[lane - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            sScan[lane] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        uint prefix = carry + sScan[lane] - count;
        if (cluster < clusterCount) {
            uint start = min(prefix, maxIndexCount);
            storeDst(gridOffset + cluster * 2u, indexOffset + start);
            storeDst(gridOffset + cluster * 2u + 1u, min(count, maxIndexCount - start));
        }
        carry += sScan[63];
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(64, 1, 1)]
void CSMain(uint3 globalId : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID,
            uint3 groupId : SV_GroupID) {
    uint passIndex = counts.x;
    if (passIndex == 0u) {
        uint word = globalId.x;
        if (word < counts.w) storeDst(word, loadSrc(word));
        if (groupId.x * 64u < counts.y) binLights(false, localId.x, globalId.x);
    } else if (passIndex == 1u) {
        scanCounts(localId.x);
    } else {
        binLights(true, localId.x, globalId.x);
    }
}
