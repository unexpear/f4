// Clustered point-light binning on the GPU; WGSL port of light_binning.comp
// (see that file and clustered_lights.h for the passes and the layout).
// Storage slots 0/1 flatten to bindings 3/7; the parameters are binding 0.

struct BinParams {
    counts: vec4<u32>,  // pass, clusterCount, visible, copyWords
    offsets: vec4<u32>, // rangeOffset, maxIndexCount, gridOffset, indexOffset
    grid: vec4<u32>,    // tilesX, tilesY, slices, 0
};

@group(0) @binding(0) var<uniform> params: BinParams;
@group(0) @binding(3) var<storage, read_write> src: array<u32>;
@group(0) @binding(7) var<storage, read_write> dst: array<u32>;

var<workgroup> sRanges: array<u32, 192>;
var<workgroup> sScan: array<u32, 64>;

fn covers(i: u32, cluster: vec3<u32>) -> bool {
    let rx = sRanges[i * 3u];
    let ry = sRanges[i * 3u + 1u];
    let rz = sRanges[i * 3u + 2u];
    return cluster.x >= (rx & 0xffffu) && cluster.x <= (rx >> 16u) &&
           cluster.y >= (ry & 0xffffu) && cluster.y <= (ry >> 16u) &&
           cluster.z >= (rz & 0xffffu) && cluster.z <= (rz >> 16u);
}

fn binLights(fill: bool, lane: u32, cluster: u32) {
    let clusterCount = params.counts.y;
    let visible = params.counts.z;
    let rangeOffset = params.offsets.x;
    let gridOffset = params.offsets.z;
    let inGrid = cluster < clusterCount;
    let coord = vec3<u32>(cluster % params.grid.x, (cluster / params.grid.x) % params.grid.y,
                          cluster / (params.grid.x * params.grid.y));
    var first = 0u;
    var allowed = 0u;
    if (fill && inGrid) {
        first = dst[gridOffset + cluster * 2u];
        allowed = dst[gridOffset + cluster * 2u + 1u];
    }
    var count = 0u;
    for (var base = 0u; base < visible; base += 64u) {
        let light = base + lane;
        if (light < visible) {
            sRanges[lane * 3u] = src[rangeOffset + light * 3u];
            sRanges[lane * 3u + 1u] = src[rangeOffset + light * 3u + 1u];
            sRanges[lane * 3u + 2u] = src[rangeOffset + light * 3u + 2u];
        }
        workgroupBarrier();
        let n = min(64u, visible - base);
        if (inGrid) {
            for (var i = 0u; i < n; i += 1u) {
                if (covers(i, coord)) {
                    if (fill && count < allowed) {
                        dst[first + count] = base + i;
                    }
                    count += 1u;
                }
            }
        }
        workgroupBarrier();
    }
    if (!fill && inGrid) {
        dst[gridOffset + cluster * 2u + 1u] = count;
    }
}

fn scanCounts(lane: u32) {
    let clusterCount = params.counts.y;
    let maxIndexCount = params.offsets.y;
    let gridOffset = params.offsets.z;
    let indexOffset = params.offsets.w;
    var carry = 0u;
    for (var base = 0u; base < clusterCount; base += 64u) {
        let cluster = base + lane;
        var count = 0u;
        if (cluster < clusterCount) {
            count = dst[gridOffset + cluster * 2u + 1u];
        }
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
        let prefix = carry + sScan[lane] - count;
        if (cluster < clusterCount) {
            let start = min(prefix, maxIndexCount);
            dst[gridOffset + cluster * 2u] = indexOffset + start;
            dst[gridOffset + cluster * 2u + 1u] = min(count, maxIndexCount - start);
        }
        carry = carry + sScan[63];
        workgroupBarrier();
    }
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalId: vec3<u32>,
           @builtin(local_invocation_id) localId: vec3<u32>,
           @builtin(workgroup_id) groupId: vec3<u32>) {
    let passIndex = params.counts.x;
    if (passIndex == 0u) {
        let word = globalId.x;
        if (word < params.counts.w) {
            dst[word] = src[word];
        }
        if (groupId.x * 64u < params.counts.y) {
            binLights(false, localId.x, globalId.x);
        }
    } else if (passIndex == 1u) {
        scanCounts(localId.x);
    } else {
        binLights(true, localId.x, globalId.x);
    }
}
