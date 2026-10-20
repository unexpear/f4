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

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let id = globalId.x;
    if (id < params.counts.z) {
        dst[id] = src[id];
    }
    let clusterCount = params.counts.x;
    if (id >= clusterCount) {
        return;
    }
    let gridOffset = params.offsets.y;
    let first = src[gridOffset + id * 2u];
    let allowed = src[gridOffset + id * 2u + 1u];
    if (allowed == 0u) {
        return;
    }
    let maskWords = params.counts.w;
    let tilesX = params.grid.x;
    let tilesY = params.grid.y;
    let x = id % tilesX;
    let y = (id / tilesX) % tilesY;
    let z = id / (tilesX * tilesY);
    let rowX = params.offsets.x + x * maskWords;
    let rowY = params.offsets.x + (tilesX + y) * maskWords;
    let rowZ = params.offsets.x + (tilesX + tilesY + z) * maskWords;
    var written = 0u;
    var word = 0u;
    loop {
        if (word >= maskWords || written >= allowed) {
            break;
        }
        var bits = src[rowX + word] & src[rowY + word] & src[rowZ + word];
        loop {
            if (bits == 0u || written >= allowed) {
                break;
            }
            let bit = firstTrailingBit(bits);
            dst[first + written] = word * 32u + bit;
            written = written + 1u;
            bits = bits & (bits - 1u);
        }
        word = word + 1u;
    }
}
