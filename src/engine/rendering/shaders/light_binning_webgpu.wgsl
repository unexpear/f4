// Clustered point-light binning on the GPU; WGSL port of light_binning.comp
// (see that file and clustered_lights.h for the layout). Storage slots 0/1
// flatten to bindings 3/7; the parameters are binding 0.

struct BinParams {
    counts: vec4<u32>,  // clusterCount, visible, copyWords, maskWords
    offsets: vec4<u32>, // maskOffset, gridOffset, 0, 0
    grid: vec4<u32>,    // tilesX, tilesY, slices, 0
};

@group(0) @binding(0) var<uniform> params: BinParams;
@group(0) @binding(3) var<storage, read_write> src: array<u32>;
@group(0) @binding(7) var<storage, read_write> dst: array<u32>;

var<workgroup> sScan: array<u32, 64>;

// WGSL needs workgroup barriers in uniform control flow, so every workgroup
// walks all mask chunks (no early exit on the count) and only the writes
// depend on per-cluster values.
@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(local_invocation_id) localId: vec3<u32>,
           @builtin(workgroup_id) groupId: vec3<u32>) {
    let lane = localId.x;
    let group = groupId.x;
    let copyWord = group * 64u + lane;
    if (copyWord < params.counts.z) {
        dst[copyWord] = src[copyWord];
    }
    if (group >= params.counts.x) {
        return;
    }
    let gridOffset = params.offsets.y;
    let first = src[gridOffset + group * 2u];
    let allowed = src[gridOffset + group * 2u + 1u];
    let maskWords = params.counts.w;
    let tilesX = params.grid.x;
    let tilesY = params.grid.y;
    let x = group % tilesX;
    let y = (group / tilesX) % tilesY;
    let z = group / (tilesX * tilesY);
    let rowX = params.offsets.x + x * maskWords;
    let rowY = params.offsets.x + (tilesX + y) * maskWords;
    let rowZ = params.offsets.x + (tilesX + tilesY + z) * maskWords;
    var base = 0u;
    for (var chunk = 0u; chunk < maskWords; chunk = chunk + 64u) {
        let word = chunk + lane;
        var bits = 0u;
        if (word < maskWords && allowed != 0u) {
            bits = src[rowX + word] & src[rowY + word] & src[rowZ + word];
        }
        let count = countOneBits(bits);
        sScan[lane] = count;
        workgroupBarrier();
        for (var offset = 1u; offset < 64u; offset = offset << 1u) {
            var add = 0u;
            if (lane >= offset) {
                add = sScan[lane - offset];
            }
            workgroupBarrier();
            sScan[lane] = sScan[lane] + add;
            workgroupBarrier();
        }
        var position = base + sScan[lane] - count;
        loop {
            if (bits == 0u || position >= allowed) {
                break;
            }
            dst[first + position] = word * 32u + firstTrailingBit(bits);
            position = position + 1u;
            bits = bits & (bits - 1u);
        }
        base = base + sScan[63];
        workgroupBarrier();
    }
}
