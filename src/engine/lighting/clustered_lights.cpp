#include "engine/lighting/clustered_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/math/vec4.h"

namespace f4::engine {

namespace {

[[nodiscard]] auto floatBits(float value) -> std::uint32_t {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] auto tileFor(float ndc, std::uint32_t tiles) -> std::uint32_t {
    const float scaled = std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(tiles));
    if (!(scaled > 0.0f)) return 0;
    const float last = static_cast<float>(tiles - 1);
    return static_cast<std::uint32_t>(std::min(scaled, last));
}

struct LightRange {
    std::uint32_t source = 0;
    std::uint32_t x0 = 0, x1 = 0;
    std::uint32_t y0 = 0, y1 = 0;
    std::uint32_t z0 = 0, z1 = 0;
};

// Culls `lights` against the camera and computes each survivor's cluster box.
// Shared by the CPU binning and the GPU binning input so both bin from the
// exact same boxes.
void gatherLightRanges(std::span<const ClusteredLightInput> lights, const math::Mat4& view,
                       const math::Mat4& projection, float nearClamped, float farClamped,
                       const ClusterSliceParams& sliceParams, std::uint32_t tilesX,
                       std::uint32_t tilesY, std::uint32_t slices,
                       containers::Array<LightRange>& ranges, ClusteredLightStats& stats) {
    ranges.clear();
    ranges.reserve(lights.size());
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const ClusteredLightInput& light = lights[index];
        ++stats.submitted;
        const float range = light.range;
        if (!std::isfinite(range) || range <= 0.0f || !std::isfinite(light.position.x) ||
            !std::isfinite(light.position.y) || !std::isfinite(light.position.z)) {
            continue;
        }
        if (std::max({light.radiance.x, light.radiance.y, light.radiance.z}) <= 0.0f) {
            continue;
        }
        const math::Vec3 viewCenter = view.transformPoint(light.position);
        const float depth = -viewCenter.z;
        const float zMin = depth - range;
        const float zMax = depth + range;
        if (zMax < nearClamped || zMin > farClamped) continue;

        LightRange lightRange;
        lightRange.source = static_cast<std::uint32_t>(index);
        lightRange.z0 = clusterSliceForDepth(std::max(zMin, nearClamped), sliceParams, slices);
        lightRange.z1 = clusterSliceForDepth(std::min(zMax, farClamped), sliceParams, slices);
        if (zMin <= nearClamped) {
            // The sphere crosses the near plane, so its screen footprint is
            // unbounded; cover every tile conservatively.
            lightRange.x1 = tilesX - 1;
            lightRange.y1 = tilesY - 1;
        } else {
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            for (std::uint32_t corner = 0; corner < 8; ++corner) {
                const math::Vec3 offset((corner & 1u) != 0 ? range : -range,
                                        (corner & 2u) != 0 ? range : -range,
                                        (corner & 4u) != 0 ? range : -range);
                const math::Vec4 clip =
                    projection.mul(math::Vec4(viewCenter + offset, 1.0f));
                const float ndcX = clip.x / clip.w;
                const float ndcY = clip.y / clip.w;
                minX = std::min(minX, ndcX);
                maxX = std::max(maxX, ndcX);
                minY = std::min(minY, ndcY);
                maxY = std::max(maxY, ndcY);
            }
            if (maxX < -1.0f || minX > 1.0f || maxY < -1.0f || minY > 1.0f) continue;
            lightRange.x0 = tileFor(minX, tilesX);
            lightRange.x1 = tileFor(maxX, tilesX);
            lightRange.y0 = tileFor(minY, tilesY);
            lightRange.y1 = tileFor(maxY, tilesY);
        }
        ranges.push(lightRange);
    }
}

void writeLightRecords(std::span<const ClusteredLightInput> lights,
                       const containers::Array<LightRange>& ranges, std::uint32_t* records) {
    for (std::size_t lightIndex = 0; lightIndex < ranges.size(); ++lightIndex) {
        const ClusteredLightInput& light = lights[ranges[lightIndex].source];
        std::uint32_t* record = records + lightIndex * kPointLightRecordWords;
        record[0] = floatBits(light.position.x);
        record[1] = floatBits(light.position.y);
        record[2] = floatBits(light.position.z);
        record[3] = floatBits(light.range);
        record[4] = floatBits(std::max(light.radiance.x, 0.0f));
        record[5] = floatBits(std::max(light.radiance.y, 0.0f));
        record[6] = floatBits(std::max(light.radiance.z, 0.0f));
        record[7] = light.shadowSlot;
    }
}

} // namespace

auto clusterSliceParams(float nearPlane, float farPlane, std::uint32_t slices)
    -> ClusterSliceParams {
    const float nearClamped = std::max(nearPlane, 1.0e-4f);
    const float farClamped = std::max(farPlane, nearClamped * 1.001f);
    const float logRange = std::log2(farClamped) - std::log2(nearClamped);
    ClusterSliceParams params;
    params.scale = static_cast<float>(std::max(slices, 1u)) / logRange;
    params.bias = -std::log2(nearClamped) * params.scale;
    return params;
}

auto clusterSliceForDepth(float viewDepth, const ClusterSliceParams& params,
                          std::uint32_t slices) -> std::uint32_t {
    const std::uint32_t count = std::max(slices, 1u);
    const float slice =
        std::floor(std::log2(std::max(viewDepth, 1.0e-4f)) * params.scale + params.bias);
    if (!(slice > 0.0f)) return 0;
    return static_cast<std::uint32_t>(std::min(slice, static_cast<float>(count - 1)));
}

auto buildClusteredLightBuffer(std::span<const ClusteredLightInput> lights,
                               const math::Mat4& view, const math::Mat4& projection,
                               float nearPlane, float farPlane,
                               const ClusterGridConfig& config,
                               containers::Array<std::uint32_t>& words)
    -> ClusteredLightStats {
    ClusteredLightStats stats;
    const std::uint32_t tilesX = std::max(config.tilesX, 1u);
    const std::uint32_t tilesY = std::max(config.tilesY, 1u);
    const std::uint32_t slices = std::max(config.slices, 1u);
    const float nearClamped = std::max(nearPlane, 1.0e-4f);
    const float farClamped = std::max(farPlane, nearClamped * 1.001f);
    const ClusterSliceParams sliceParams = clusterSliceParams(nearClamped, farClamped, slices);
    const std::uint32_t clusterCount = tilesX * tilesY * slices;

    containers::Array<LightRange> ranges;
    gatherLightRanges(lights, view, projection, nearClamped, farClamped, sliceParams, tilesX,
                      tilesY, slices, ranges, stats);
    containers::Array<std::uint32_t> counts;
    if (!ranges.isEmpty()) {
        counts.resize(clusterCount);
        std::fill(counts.begin(), counts.end(), 0u);
    }
    for (const LightRange& lightRange : ranges) {
        for (std::uint32_t z = lightRange.z0; z <= lightRange.z1; ++z) {
            for (std::uint32_t y = lightRange.y0; y <= lightRange.y1; ++y) {
                for (std::uint32_t x = lightRange.x0; x <= lightRange.x1; ++x) {
                    ++counts[(z * tilesY + y) * tilesX + x];
                }
            }
        }
    }

    const std::uint32_t visible = static_cast<std::uint32_t>(ranges.size());
    stats.visible = visible;
    const std::uint32_t lightOffset = kPointLightHeaderWords;
    const std::uint32_t gridOffset = lightOffset + visible * kPointLightRecordWords;
    const std::uint32_t indexOffset =
        visible == 0 ? gridOffset : gridOffset + clusterCount * kPointLightClusterWords;

    // Clip per-cluster counts to the global index budget in cluster order.
    std::uint32_t totalIndices = 0;
    if (visible != 0) {
        for (std::uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
            const std::uint32_t remaining = config.maxIndexCount - totalIndices;
            const std::uint32_t allowed = std::min(counts[cluster], remaining);
            stats.droppedIndices += counts[cluster] - allowed;
            counts[cluster] = allowed;
            totalIndices += allowed;
            stats.maxLightsPerCluster = std::max(stats.maxLightsPerCluster, allowed);
        }
    }
    stats.indexCount = totalIndices;

    words.clear();
    words.resize(static_cast<std::size_t>(indexOffset) + totalIndices);
    std::fill(words.begin(), words.end(), 0u);
    words[0] = visible;
    words[1] = tilesX;
    words[2] = tilesY;
    words[3] = slices;
    words[4] = lightOffset;
    words[5] = gridOffset;
    words[6] = indexOffset;
    words[7] = totalIndices;
    words[8] = floatBits(sliceParams.scale);
    words[9] = floatBits(sliceParams.bias);
    if (visible == 0) return stats;

    writeLightRecords(lights, ranges, words.data() + lightOffset);

    std::uint32_t cursor = indexOffset;
    for (std::uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
        std::uint32_t* record = words.data() + gridOffset + cluster * kPointLightClusterWords;
        record[0] = cursor;
        record[1] = 0;
        cursor += counts[cluster];
    }
    for (std::uint32_t lightIndex = 0; lightIndex < visible; ++lightIndex) {
        const LightRange& lightRange = ranges[lightIndex];
        for (std::uint32_t z = lightRange.z0; z <= lightRange.z1; ++z) {
            for (std::uint32_t y = lightRange.y0; y <= lightRange.y1; ++y) {
                for (std::uint32_t x = lightRange.x0; x <= lightRange.x1; ++x) {
                    const std::uint32_t cluster = (z * tilesY + y) * tilesX + x;
                    std::uint32_t* record =
                        words.data() + gridOffset + cluster * kPointLightClusterWords;
                    if (record[1] >= counts[cluster]) continue;
                    words[record[0] + record[1]] = lightIndex;
                    ++record[1];
                }
            }
        }
    }
    return stats;
}

auto buildClusteredLightBinningInput(std::span<const ClusteredLightInput> lights,
                                     const math::Mat4& view, const math::Mat4& projection,
                                     float nearPlane, float farPlane,
                                     const ClusterGridConfig& config,
                                     std::uint32_t reservedWords,
                                     containers::Array<std::uint32_t>& words,
                                     ClusteredLightGpuParams& params) -> ClusteredLightStats {
    ClusteredLightStats stats;
    params = {};
    const std::uint32_t tilesX = std::max(config.tilesX, 1u);
    const std::uint32_t tilesY = std::max(config.tilesY, 1u);
    const std::uint32_t slices = std::max(config.slices, 1u);
    const float nearClamped = std::max(nearPlane, 1.0e-4f);
    const float farClamped = std::max(farPlane, nearClamped * 1.001f);
    const ClusterSliceParams sliceParams = clusterSliceParams(nearClamped, farClamped, slices);
    const std::uint32_t clusterCount = tilesX * tilesY * slices;

    containers::Array<LightRange> ranges;
    gatherLightRanges(lights, view, projection, nearClamped, farClamped, sliceParams, tilesX,
                      tilesY, slices, ranges, stats);
    const std::uint32_t visible = static_cast<std::uint32_t>(ranges.size());
    stats.visible = visible;

    // Per-cluster counts without visiting clusters per light: add each box to
    // a 3D difference array (+/-1 at its 8 corners), then prefix-sum the
    // three axes. Then cap in cluster order exactly like the CPU binning.
    const std::uint32_t strideX = tilesX + 1u;
    const std::uint32_t strideY = (tilesY + 1u) * strideX;
    containers::Array<std::int32_t> diff;
    diff.resize(static_cast<std::size_t>(strideY) * (slices + 1u));
    std::fill(diff.begin(), diff.end(), 0);
    for (const LightRange& range : ranges) {
        const std::uint32_t xs[2] = {range.x0, range.x1 + 1u};
        const std::uint32_t ys[2] = {range.y0 * strideX, (range.y1 + 1u) * strideX};
        const std::uint32_t zs[2] = {range.z0 * strideY, (range.z1 + 1u) * strideY};
        for (std::uint32_t corner = 0; corner < 8u; ++corner) {
            const std::uint32_t ix = corner & 1u;
            const std::uint32_t iy = (corner >> 1) & 1u;
            const std::uint32_t iz = corner >> 2;
            diff[xs[ix] + ys[iy] + zs[iz]] += ((ix + iy + iz) & 1u) != 0 ? -1 : 1;
        }
    }
    for (std::uint32_t z = 0; z < slices; ++z) {
        for (std::uint32_t y = 0; y < tilesY; ++y) {
            std::int32_t* row = diff.data() + z * strideY + y * strideX;
            for (std::uint32_t x = 1; x < tilesX; ++x) row[x] += row[x - 1];
        }
    }
    for (std::uint32_t z = 0; z < slices; ++z) {
        for (std::uint32_t y = 1; y < tilesY; ++y) {
            std::int32_t* row = diff.data() + z * strideY + y * strideX;
            const std::int32_t* above = row - strideX;
            for (std::uint32_t x = 0; x < tilesX; ++x) row[x] += above[x];
        }
    }
    for (std::uint32_t z = 1; z < slices; ++z) {
        for (std::uint32_t y = 0; y < tilesY; ++y) {
            std::int32_t* row = diff.data() + z * strideY + y * strideX;
            const std::int32_t* below = row - strideY;
            for (std::uint32_t x = 0; x < tilesX; ++x) row[x] += below[x];
        }
    }

    const std::uint32_t lightOffset = kPointLightHeaderWords;
    const std::uint32_t reservedOffset = lightOffset + visible * kPointLightRecordWords;
    const std::uint32_t gridOffset = reservedOffset + reservedWords;
    const std::uint32_t indexOffset = gridOffset + clusterCount * kPointLightClusterWords;
    const std::uint32_t maskOffset = indexOffset;
    const std::uint32_t maskWords = (visible + 31u) / 32u;
    const std::uint32_t maskRows = tilesX + tilesY + slices;

    words.clear();
    words.resize(static_cast<std::size_t>(maskOffset) +
                 static_cast<std::size_t>(maskRows) * maskWords);
    std::fill(words.begin(), words.end(), 0u);

    std::uint32_t totalIndices = 0;
    std::uint64_t references = 0;
    for (std::uint32_t z = 0; z < slices; ++z) {
        for (std::uint32_t y = 0; y < tilesY; ++y) {
            const std::int32_t* row = diff.data() + z * strideY + y * strideX;
            for (std::uint32_t x = 0; x < tilesX; ++x) {
                const std::uint32_t count = static_cast<std::uint32_t>(row[x]);
                references += count;
                const std::uint32_t allowed =
                    std::min(count, config.maxIndexCount - totalIndices);
                const std::uint32_t cluster = (z * tilesY + y) * tilesX + x;
                std::uint32_t* record =
                    words.data() + gridOffset + cluster * kPointLightClusterWords;
                record[0] = indexOffset + totalIndices;
                record[1] = allowed;
                totalIndices += allowed;
                stats.maxLightsPerCluster = std::max(stats.maxLightsPerCluster, allowed);
            }
        }
    }
    stats.indexCount = totalIndices;
    stats.droppedIndices = static_cast<std::uint32_t>(references - totalIndices);

    words[0] = visible;
    words[1] = tilesX;
    words[2] = tilesY;
    words[3] = slices;
    words[4] = lightOffset;
    words[5] = gridOffset;
    words[6] = indexOffset;
    words[7] = totalIndices;
    words[8] = floatBits(sliceParams.scale);
    words[9] = floatBits(sliceParams.bias);
    writeLightRecords(lights, ranges, words.data() + lightOffset);
    std::uint32_t* masks = words.data() + maskOffset;
    for (std::uint32_t lightIndex = 0; lightIndex < visible; ++lightIndex) {
        const LightRange& range = ranges[lightIndex];
        const std::uint32_t word = lightIndex >> 5;
        const std::uint32_t bit = 1u << (lightIndex & 31u);
        for (std::uint32_t x = range.x0; x <= range.x1; ++x) masks[x * maskWords + word] |= bit;
        for (std::uint32_t y = range.y0; y <= range.y1; ++y) {
            masks[(tilesX + y) * maskWords + word] |= bit;
        }
        for (std::uint32_t z = range.z0; z <= range.z1; ++z) {
            masks[(tilesX + tilesY + z) * maskWords + word] |= bit;
        }
    }

    params.tilesX = tilesX;
    params.tilesY = tilesY;
    params.slices = slices;
    params.clusterCount = clusterCount;
    params.visible = visible;
    params.copyWords = indexOffset;
    params.maskOffset = maskOffset;
    params.maskWords = maskWords;
    params.maxIndexCount = config.maxIndexCount;
    params.gridOffset = gridOffset;
    params.indexOffset = indexOffset;
    params.reservedOffset = reservedOffset;
    params.totalWords = indexOffset + totalIndices;
    return stats;
}

} // namespace f4::engine
