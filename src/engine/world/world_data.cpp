#include "engine/world/world_data.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "core/assert.h"
#include "core/noise/noise.h"

namespace {

[[nodiscard]] auto hashWords(std::initializer_list<std::uint32_t> words) noexcept
    -> std::size_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t word : words) {
        hash ^= word;
        hash *= 1099511628211ull;
    }
    return static_cast<std::size_t>(hash);
}

} // namespace

auto std::hash<f4::world::ChunkCoord>::operator()(
    const f4::world::ChunkCoord& value) const noexcept -> std::size_t {
    return hashWords({static_cast<std::uint32_t>(value.x),
                      static_cast<std::uint32_t>(value.y),
                      static_cast<std::uint32_t>(value.z)});
}

auto std::hash<f4::world::RegionCoord>::operator()(
    const f4::world::RegionCoord& value) const noexcept -> std::size_t {
    return hashWords({static_cast<std::uint32_t>(value.x),
                      static_cast<std::uint32_t>(value.z)});
}

auto std::hash<f4::world::AxialCoord>::operator()(
    const f4::world::AxialCoord& value) const noexcept -> std::size_t {
    return hashWords({static_cast<std::uint32_t>(value.q),
                      static_cast<std::uint32_t>(value.r)});
}

auto std::hash<f4::world::HexCornerCoord>::operator()(
    const f4::world::HexCornerCoord& value) const noexcept -> std::size_t {
    return hashWords({static_cast<std::uint32_t>(value.x2),
                      static_cast<std::uint32_t>(static_cast<std::uint64_t>(value.x2) >> 32),
                      static_cast<std::uint32_t>(value.y2),
                      static_cast<std::uint32_t>(static_cast<std::uint64_t>(value.y2) >> 32)});
}

namespace f4::world {

auto floorDiv(std::int32_t value, std::int32_t positiveDivisor) -> std::int32_t {
    F4_ASSERT(positiveDivisor > 0, "floorDiv requires a positive divisor");
    const std::int32_t quotient = value / positiveDivisor;
    const std::int32_t remainder = value % positiveDivisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

auto floorMod(std::int32_t value, std::int32_t positiveDivisor) -> std::int32_t {
    F4_ASSERT(positiveDivisor > 0, "floorMod requires a positive divisor");
    const std::int32_t remainder = value % positiveDivisor;
    return remainder < 0 ? remainder + positiveDivisor : remainder;
}

BlockRegistry::BlockRegistry() {
    blocks_.set(0, {false, false, 0, 0.0f});
    blocks_.set(1, {true, true, 0, std::numeric_limits<float>::infinity()});
    blocks_.set(2, {true, true, 1, 1.0f});
}

auto BlockRegistry::setEngineBlock(std::uint16_t id, const BlockDesc& desc) -> bool {
    if (id >= 256) return false;
    blocks_.set(id, desc);
    return true;
}

auto BlockRegistry::registerGameBlock(std::uint16_t id, const BlockDesc& desc) -> bool {
    return id >= 256 && blocks_.insert(id, desc);
}

auto BlockRegistry::find(std::uint16_t id) const -> const BlockDesc* {
    return blocks_.find(id);
}

auto operator==(const VoxelCells& a, const VoxelCells& b) -> bool {
    if (a.values.size() != b.values.size()) return false;
    return std::equal(a.values.begin(), a.values.end(), b.values.begin());
}

auto isVoxelEmpty(const VoxelCells& cells) -> bool {
    return std::all_of(cells.values.begin(), cells.values.end(),
                       [](std::uint16_t value) { return value == 0; });
}

auto packedVoxelValue(const PackedVoxelCells& cells, std::uint32_t index)
    -> std::optional<std::uint16_t> {
    if (index >= kVoxelCellCount || cells.palette.isEmpty() || cells.palette.size() > 256) {
        return std::nullopt;
    }
    std::uint32_t paletteIndex = 0;
    if (cells.bitsPerIndex == 0) {
        if (cells.palette.size() != 1 || !cells.indices.isEmpty()) return std::nullopt;
    } else if (cells.bitsPerIndex == 4) {
        if (cells.palette.size() > 16 ||
            cells.indices.size() != (kVoxelCellCount + 1u) / 2u) {
            return std::nullopt;
        }
        const std::uint8_t packed = cells.indices[index / 2u];
        paletteIndex = index % 2u == 0 ? packed & 0x0fu : packed >> 4u;
    } else if (cells.bitsPerIndex == 8) {
        if (cells.indices.size() != kVoxelCellCount) return std::nullopt;
        paletteIndex = cells.indices[index];
    } else {
        return std::nullopt;
    }
    return paletteIndex < cells.palette.size()
        ? std::optional<std::uint16_t>(cells.palette[paletteIndex])
        : std::nullopt;
}

auto packVoxelCells(const VoxelCells& cells) -> std::optional<PackedVoxelCells> {
    if (cells.values.size() != kVoxelCellCount) return std::nullopt;
    PackedVoxelCells output;
    containers::Array<std::uint8_t> unpackedIndices;
    unpackedIndices.resize(kVoxelCellCount);
    for (std::uint32_t index = 0; index < kVoxelCellCount; ++index) {
        const std::uint16_t value = cells.values[index];
        std::size_t paletteIndex = 0;
        while (paletteIndex < output.palette.size() &&
               output.palette[paletteIndex] != value) {
            ++paletteIndex;
        }
        if (paletteIndex == output.palette.size()) {
            if (output.palette.size() == 256) return std::nullopt;
            output.palette.push(value);
        }
        unpackedIndices[index] = static_cast<std::uint8_t>(paletteIndex);
    }
    if (output.palette.size() == 1) return output;
    if (output.palette.size() <= 16) {
        output.bitsPerIndex = 4;
        output.indices.resize((kVoxelCellCount + 1u) / 2u);
        for (std::uint32_t index = 0; index < kVoxelCellCount; index += 2) {
            output.indices[index / 2u] = static_cast<std::uint8_t>(
                unpackedIndices[index] | (unpackedIndices[index + 1u] << 4u));
        }
    } else {
        output.bitsPerIndex = 8;
        output.indices = std::move(unpackedIndices);
    }
    return output;
}

auto unpackVoxelCells(const PackedVoxelCells& cells) -> std::optional<VoxelCells> {
    VoxelCells output;
    output.values.resize(kVoxelCellCount);
    for (std::uint32_t index = 0; index < kVoxelCellCount; ++index) {
        const std::optional<std::uint16_t> value = packedVoxelValue(cells, index);
        if (!value.has_value()) return std::nullopt;
        output.values[index] = *value;
    }
    return output;
}

auto packedVoxelStorageBytes(const PackedVoxelCells& cells) -> std::size_t {
    return cells.palette.size() * sizeof(std::uint16_t) + cells.indices.size();
}

auto generateVoxelCells(std::uint64_t worldSeed, ChunkCoord coord,
                         const VoxelPolicyParams& params) -> VoxelCells {
    VoxelCells output;
    if (!std::isfinite(params.baseHeight) || !std::isfinite(params.heightAmplitude) ||
        !std::isfinite(params.frequency) || params.frequency < 0.0f) {
        return output;
    }
    output.values.resize(kVoxelCellCount);
    for (std::uint32_t z = 0; z < kVoxelEdge; ++z) {
        for (std::uint32_t x = 0; x < kVoxelEdge; ++x) {
            const std::int64_t globalX = static_cast<std::int64_t>(coord.x) * kVoxelEdge + x;
            const std::int64_t globalZ = static_cast<std::int64_t>(coord.z) * kVoxelEdge + z;
            const float terrain = params.baseHeight + params.heightAmplitude * noise::fbm2D(
                {}, static_cast<float>(globalX) * params.frequency,
                static_cast<float>(globalZ) * params.frequency, worldSeed);
            if (!std::isfinite(terrain)) return {};
            for (std::uint32_t y = 0; y < kVoxelEdge; ++y) {
                const std::int64_t globalY =
                    static_cast<std::int64_t>(coord.y) * kVoxelEdge + y;
                std::uint16_t value = 0;
                if (globalY <= 0) value = 1;
                else if (static_cast<float>(globalY) <= terrain) value = 2;
                output.values[voxelIndex(x, y, z)] = value;
            }
        }
    }
    return output;
}

namespace {

auto raycastGrid(math::Vec3 origin, math::Vec3 direction, float maxDistance,
                 const f4::world::VoxelFetch& fetch)
    -> std::optional<f4::world::VoxelRayHit> {
    using namespace f4;
    using namespace f4::world;
    if (!fetch || maxDistance < 0.0f || !std::isfinite(maxDistance) ||
        !std::isfinite(origin.x) ||
        !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z)) {
        return std::nullopt;
    }
    direction = math::normalized(direction);
    if (math::lengthSq(direction) == 0.0f) return std::nullopt;
    constexpr float intMin = static_cast<float>(std::numeric_limits<std::int32_t>::min());
    constexpr float intMax = static_cast<float>(std::numeric_limits<std::int32_t>::max());
    if (origin.x < intMin || origin.x >= intMax || origin.y < intMin ||
        origin.y >= intMax || origin.z < intMin || origin.z >= intMax) {
        return std::nullopt;
    }
    std::int32_t x = static_cast<std::int32_t>(std::floor(origin.x));
    std::int32_t y = static_cast<std::int32_t>(std::floor(origin.y));
    std::int32_t z = static_cast<std::int32_t>(std::floor(origin.z));
    if (fetch(x, y, z) != 0) return VoxelRayHit{x, y, z, {}, 0.0f};
    const auto axis = [](float position, float component) {
        struct AxisStep { std::int32_t step; float next; float delta; };
        if (component > 0.0f) {
            return AxisStep{1, (std::floor(position) + 1.0f - position) / component,
                            1.0f / component};
        }
        if (component < 0.0f) {
            return AxisStep{-1, (position - std::floor(position)) / -component,
                            1.0f / -component};
        }
        return AxisStep{0, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    };
    auto xAxis = axis(origin.x, direction.x);
    auto yAxis = axis(origin.y, direction.y);
    auto zAxis = axis(origin.z, direction.z);
    float t = 0.0f;
    math::Vec3 normal{};
    const double crossings = std::ceil(static_cast<double>(maxDistance) *
        (std::fabs(direction.x) + std::fabs(direction.y) + std::fabs(direction.z)));
    if (crossings > static_cast<double>(std::numeric_limits<std::size_t>::max() - 4u)) {
        return std::nullopt;
    }
    const std::size_t maxSteps = static_cast<std::size_t>(crossings) + 4u;
    for (std::size_t step = 0; step < maxSteps && t <= maxDistance; ++step) {
        if (xAxis.next <= yAxis.next && xAxis.next <= zAxis.next) {
            if ((xAxis.step < 0 && x == std::numeric_limits<std::int32_t>::min()) ||
                (xAxis.step > 0 && x == std::numeric_limits<std::int32_t>::max())) break;
            x += xAxis.step;
            t = xAxis.next;
            xAxis.next += xAxis.delta;
            normal = {-static_cast<float>(xAxis.step), 0.0f, 0.0f};
        } else if (yAxis.next <= zAxis.next) {
            if ((yAxis.step < 0 && y == std::numeric_limits<std::int32_t>::min()) ||
                (yAxis.step > 0 && y == std::numeric_limits<std::int32_t>::max())) break;
            y += yAxis.step;
            t = yAxis.next;
            yAxis.next += yAxis.delta;
            normal = {0.0f, -static_cast<float>(yAxis.step), 0.0f};
        } else {
            if ((zAxis.step < 0 && z == std::numeric_limits<std::int32_t>::min()) ||
                (zAxis.step > 0 && z == std::numeric_limits<std::int32_t>::max())) break;
            z += zAxis.step;
            t = zAxis.next;
            zAxis.next += zAxis.delta;
            normal = {0.0f, 0.0f, -static_cast<float>(zAxis.step)};
        }
        if (t <= maxDistance && fetch(x, y, z) != 0) {
            return VoxelRayHit{x, y, z, normal, t};
        }
    }
    return std::nullopt;
}

} // namespace

auto raycastVoxelWorld(math::Vec3 origin, math::Vec3 direction, float maxDistance,
                       const VoxelFetch& fetch) -> std::optional<VoxelRayHit> {
    return raycastGrid(origin, direction, maxDistance, fetch);
}

auto raycastVoxels(const VoxelCells& cells, math::Vec3 origin, math::Vec3 direction,
                   float maxDistance) -> std::optional<VoxelRayHit> {
    if (cells.values.size() != kVoxelCellCount) return std::nullopt;
    return raycastGrid(origin, direction, maxDistance,
        [&](std::int32_t x, std::int32_t y, std::int32_t z) -> std::uint16_t {
            if (x < 0 || y < 0 || z < 0 || x >= static_cast<std::int32_t>(kVoxelEdge) ||
                y >= static_cast<std::int32_t>(kVoxelEdge) ||
                z >= static_cast<std::int32_t>(kVoxelEdge)) {
                return 0;
            }
            return cells.values[voxelIndex(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y),
                                           static_cast<std::uint32_t>(z))];
        });
}

auto operator==(const HeightTile& a, const HeightTile& b) -> bool {
    if (a.samples.size() != b.samples.size()) return false;
    return std::equal(a.samples.begin(), a.samples.end(), b.samples.begin());
}

auto generateHeightTile(std::uint64_t worldSeed, ChunkCoord coord,
                         const HeightPolicyParams& params) -> HeightTile {
    HeightTile output;
    if (!std::isfinite(params.baseHeight) || !std::isfinite(params.heightAmplitude) ||
        !std::isfinite(params.frequency) || params.frequency < 0.0f) {
        return output;
    }
    output.samples.resize(kHeightSampleCount);
    for (std::uint32_t z = 0; z < kHeightSamples; ++z) {
        for (std::uint32_t x = 0; x < kHeightSamples; ++x) {
            const std::int64_t globalX = static_cast<std::int64_t>(coord.x) * kHeightQuads + x;
            const std::int64_t globalZ = static_cast<std::int64_t>(coord.z) * kHeightQuads + z;
            const float value = noise::fbm2D(
                {}, static_cast<float>(globalX) * params.frequency,
                static_cast<float>(globalZ) * params.frequency, worldSeed);
            HeightSample sample;
            sample.height = params.baseHeight + params.heightAmplitude * value;
            if (!std::isfinite(sample.height)) return {};
            sample.biomeId = value < -0.2f ? 0u : (value > 0.35f ? 2u : 1u);
            const std::uint8_t rock = static_cast<std::uint8_t>(
                std::clamp((value + 1.0f) * 48.0f, 0.0f, 96.0f));
            sample.materialWeights = {
                static_cast<std::uint8_t>(255u - rock), rock, 0, 0};
            output.samples[heightSampleIndex(x, z)] = sample;
        }
    }
    return output;
}

auto sampleHeight(const HeightTile& tile, float localX, float localZ)
    -> std::optional<HeightSample> {
    if (tile.samples.size() != kHeightSampleCount || !std::isfinite(localX) ||
        !std::isfinite(localZ)) {
        return std::nullopt;
    }
    localX = std::clamp(localX, 0.0f, static_cast<float>(kHeightQuads));
    localZ = std::clamp(localZ, 0.0f, static_cast<float>(kHeightQuads));
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(localX));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(localZ));
    const std::uint32_t x1 = std::min(x0 + 1u, kHeightQuads);
    const std::uint32_t z1 = std::min(z0 + 1u, kHeightQuads);
    const std::array<const HeightSample*, 4> corners{{
        &tile.samples[heightSampleIndex(x0, z0)],
        &tile.samples[heightSampleIndex(x1, z0)],
        &tile.samples[heightSampleIndex(x0, z1)],
        &tile.samples[heightSampleIndex(x1, z1)]}};
    for (const HeightSample* corner : corners) {
        const std::uint32_t weightSum = static_cast<std::uint32_t>(corner->materialWeights[0]) +
            corner->materialWeights[1] + corner->materialWeights[2] + corner->materialWeights[3];
        if (!std::isfinite(corner->height) || weightSum != 255u) return std::nullopt;
    }
    const float tx = localX - static_cast<float>(x0);
    const float tz = localZ - static_cast<float>(z0);
    const float h00 = tile.samples[heightSampleIndex(x0, z0)].height;
    const float h10 = tile.samples[heightSampleIndex(x1, z0)].height;
    const float h01 = tile.samples[heightSampleIndex(x0, z1)].height;
    const float h11 = tile.samples[heightSampleIndex(x1, z1)].height;
    const float nearHeight = h00 + (h10 - h00) * tx;
    const float farHeight = h01 + (h11 - h01) * tx;
    HeightSample result;
    result.height = nearHeight + (farHeight - nearHeight) * tz;
    const std::uint32_t nearestX = static_cast<std::uint32_t>(std::floor(localX + 0.5f));
    const std::uint32_t nearestZ = static_cast<std::uint32_t>(std::floor(localZ + 0.5f));
    result.biomeId = tile.samples[heightSampleIndex(nearestX, nearestZ)].biomeId;

    std::array<float, 4> interpolated{};
    std::array<std::uint32_t, 4> quantized{};
    std::uint32_t sum = 0;
    for (std::size_t layer = 0; layer < interpolated.size(); ++layer) {
        const float w00 = tile.samples[heightSampleIndex(x0, z0)].materialWeights[layer];
        const float w10 = tile.samples[heightSampleIndex(x1, z0)].materialWeights[layer];
        const float w01 = tile.samples[heightSampleIndex(x0, z1)].materialWeights[layer];
        const float w11 = tile.samples[heightSampleIndex(x1, z1)].materialWeights[layer];
        const float nearWeight = w00 + (w10 - w00) * tx;
        const float farWeight = w01 + (w11 - w01) * tx;
        interpolated[layer] = std::clamp(
            nearWeight + (farWeight - nearWeight) * tz, 0.0f, 255.0f);
        quantized[layer] = static_cast<std::uint32_t>(std::floor(interpolated[layer]));
        sum += quantized[layer];
    }
    while (sum < 255u) {
        std::size_t best = 0;
        float bestRemainder = -1.0f;
        for (std::size_t layer = 0; layer < interpolated.size(); ++layer) {
            const float remainder = interpolated[layer] - static_cast<float>(quantized[layer]);
            if (remainder > bestRemainder && quantized[layer] < 255u) {
                best = layer;
                bestRemainder = remainder;
            }
        }
        ++quantized[best];
        ++sum;
    }
    while (sum > 255u) {
        std::size_t best = 0;
        float bestRemainder = 2.0f;
        for (std::size_t layer = 0; layer < interpolated.size(); ++layer) {
            const float remainder = interpolated[layer] - static_cast<float>(quantized[layer]);
            if (quantized[layer] > 0u && remainder < bestRemainder) {
                best = layer;
                bestRemainder = remainder;
            }
        }
        --quantized[best];
        --sum;
    }
    for (std::size_t layer = 0; layer < quantized.size(); ++layer) {
        result.materialWeights[layer] = static_cast<std::uint8_t>(quantized[layer]);
    }
    return result;
}

auto bilinearHeight(const HeightTile& tile, float localX, float localZ) -> float {
    const std::optional<HeightSample> sample = sampleHeight(tile, localX, localZ);
    return sample.has_value() ? sample->height : 0.0f;
}

auto heightNormal(std::int32_t globalX, std::int32_t globalZ,
                   const std::function<float(std::int32_t, std::int32_t)>& fetch)
    -> math::Vec3 {
    if (!fetch || globalX == std::numeric_limits<std::int32_t>::min() ||
        globalX == std::numeric_limits<std::int32_t>::max() ||
        globalZ == std::numeric_limits<std::int32_t>::min() ||
        globalZ == std::numeric_limits<std::int32_t>::max()) {
        return {0.0f, 1.0f, 0.0f};
    }
    const float left = fetch(globalX - 1, globalZ);
    const float right = fetch(globalX + 1, globalZ);
    const float down = fetch(globalX, globalZ - 1);
    const float up = fetch(globalX, globalZ + 1);
    return math::normalized({left - right, 2.0f, down - up});
}

auto HexTile::contains(AxialCoord coordinate) const -> bool {
    const std::int64_t s = -static_cast<std::int64_t>(coordinate.q) - coordinate.r;
    const std::int64_t limit = radius;
    return std::abs(static_cast<std::int64_t>(coordinate.q)) <= limit &&
           std::abs(static_cast<std::int64_t>(coordinate.r)) <= limit &&
           std::abs(s) <= limit;
}

auto HexTile::index(AxialCoord coordinate) const -> std::optional<std::uint32_t> {
    if (!contains(coordinate) || width == 0 ||
        radius > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    const std::int32_t row = coordinate.r + static_cast<std::int32_t>(radius);
    const std::int32_t column = coordinate.q + floorDiv(coordinate.r, 2) +
                                static_cast<std::int32_t>(radius);
    if (row < 0 || column < 0 || row >= static_cast<std::int32_t>(width) ||
        column >= static_cast<std::int32_t>(width)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(row) * width + static_cast<std::uint32_t>(column);
}

auto HexTile::corner(AxialCoord coordinate, std::uint32_t cornerIndex) const
    -> std::optional<float> {
    if (!contains(coordinate) || cornerIndex >= 6) return std::nullopt;
    const float* value = cornerHeights.find(hexCorner(coordinate, cornerIndex));
    return value != nullptr ? std::optional<float>(*value) : std::nullopt;
}

auto HexTile::centerHeight(AxialCoord coordinate) const -> std::optional<float> {
    float sum = 0.0f;
    for (std::uint32_t cornerIndex = 0; cornerIndex < 6; ++cornerIndex) {
        const std::optional<float> value = corner(coordinate, cornerIndex);
        if (!value.has_value()) return std::nullopt;
        sum += *value;
    }
    return sum * (1.0f / 6.0f);
}

auto hexDistance(AxialCoord a, AxialCoord b) -> std::uint32_t {
    const std::int64_t dq = static_cast<std::int64_t>(a.q) - b.q;
    const std::int64_t dr = static_cast<std::int64_t>(a.r) - b.r;
    const std::uint64_t distance = static_cast<std::uint64_t>(
        (std::abs(dq) + std::abs(dr) + std::abs(dq + dr)) / 2);
    return static_cast<std::uint32_t>(
        std::min(distance, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
}

auto hexCorner(AxialCoord cell, std::uint32_t cornerIndex) -> HexCornerCoord {
    static constexpr std::array<HexCornerCoord, 6> offsets{{
        {1, 1}, {0, 2}, {-1, 1}, {-1, -1}, {0, -2}, {1, -1}}};
    if (cornerIndex >= offsets.size()) return {};
    const HexCornerCoord center{
        2 * static_cast<std::int64_t>(cell.q) + cell.r,
        3 * static_cast<std::int64_t>(cell.r)};
    return {center.x2 + offsets[cornerIndex].x2,
            center.y2 + offsets[cornerIndex].y2};
}

auto hexRing(AxialCoord center, std::uint32_t radius) -> containers::Array<AxialCoord> {
    containers::Array<AxialCoord> output;
    if (radius == 0) {
        output.push(center);
        return output;
    }
    const std::int64_t signedRadius = radius;
    if (radius > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        static_cast<std::int64_t>(center.q) - signedRadius <
            std::numeric_limits<std::int32_t>::min() ||
        static_cast<std::int64_t>(center.q) + signedRadius >
            std::numeric_limits<std::int32_t>::max() ||
        static_cast<std::int64_t>(center.r) - signedRadius <
            std::numeric_limits<std::int32_t>::min() ||
        static_cast<std::int64_t>(center.r) + signedRadius >
            std::numeric_limits<std::int32_t>::max()) {
        return output;
    }
    output.reserve(static_cast<std::size_t>(radius) * 6u);
    AxialCoord current{
        center.q + kHexDirections[4].q * static_cast<std::int32_t>(radius),
        center.r + kHexDirections[4].r * static_cast<std::int32_t>(radius)};
    for (const AxialCoord direction : kHexDirections) {
        for (std::uint32_t step = 0; step < radius; ++step) {
            output.push(current);
            current.q += direction.q;
            current.r += direction.r;
        }
    }
    return output;
}

auto hexSpiral(AxialCoord center, std::uint32_t radius) -> containers::Array<AxialCoord> {
    containers::Array<AxialCoord> output;
    if (radius > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return output;
    }
    const std::uint64_t cellCount = 1ull + 3ull * radius * (static_cast<std::uint64_t>(radius) + 1ull);
    if (cellCount > std::numeric_limits<std::size_t>::max()) return output;
    output.reserve(static_cast<std::size_t>(cellCount));
    output.push(center);
    for (std::uint32_t ring = 1; ring <= radius; ++ring) {
        containers::Array<AxialCoord> cells = hexRing(center, ring);
        for (AxialCoord cell : cells) output.push(cell);
    }
    return output;
}

namespace {

[[nodiscard]] auto roundAxial(double q, double r) -> AxialCoord {
    const double x = q;
    const double z = r;
    const double y = -x - z;
    std::int64_t rx = static_cast<std::int64_t>(std::round(x));
    std::int64_t ry = static_cast<std::int64_t>(std::round(y));
    std::int64_t rz = static_cast<std::int64_t>(std::round(z));
    const double xDifference = std::fabs(static_cast<double>(rx) - x);
    const double yDifference = std::fabs(static_cast<double>(ry) - y);
    const double zDifference = std::fabs(static_cast<double>(rz) - z);
    if (xDifference > yDifference && xDifference > zDifference) rx = -ry - rz;
    else if (yDifference > zDifference) ry = -rx - rz;
    else rz = -rx - ry;
    (void)ry;
    return {static_cast<std::int32_t>(rx), static_cast<std::int32_t>(rz)};
}

} // namespace

auto hexLine(AxialCoord from, AxialCoord to) -> containers::Array<AxialCoord> {
    const std::uint32_t distance = hexDistance(from, to);
    containers::Array<AxialCoord> output;
    if (distance == std::numeric_limits<std::uint32_t>::max()) return output;
    output.reserve(static_cast<std::size_t>(distance) + 1u);
    if (distance == 0) {
        output.push(from);
        return output;
    }
    for (std::uint32_t step = 0; step <= distance; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(distance);
        output.push(roundAxial(
            static_cast<double>(from.q) +
                (static_cast<double>(to.q) - static_cast<double>(from.q)) * t,
            static_cast<double>(from.r) +
                (static_cast<double>(to.r) - static_cast<double>(from.r)) * t));
    }
    return output;
}

auto generateHexTile(std::uint64_t worldSeed, ChunkCoord coord,
                     const HexPolicyParams& params) -> HexTile {
    HexTile output;
    if (params.radius > 2048u || !std::isfinite(params.heightAmplitude) ||
        !std::isfinite(params.frequency) || params.frequency < 0.0f) {
        return output;
    }
    output.radius = params.radius;
    output.width = params.radius * 2u + 1u;
    output.cells.resize(static_cast<std::size_t>(output.width) * output.width);
    const std::int32_t radius = static_cast<std::int32_t>(params.radius);
    for (std::int32_t r = -radius; r <= radius; ++r) {
        const std::int32_t qMin = std::max(-radius, -r - radius);
        const std::int32_t qMax = std::min(radius, -r + radius);
        for (std::int32_t q = qMin; q <= qMax; ++q) {
            const AxialCoord cell{q, r};
            const std::optional<std::uint32_t> index = output.index(cell);
            if (!index.has_value()) continue;
            const std::int64_t globalQ = q + static_cast<std::int64_t>(coord.x) * output.width;
            const std::int64_t globalR = r + static_cast<std::int64_t>(coord.z) * output.width;
            if (globalQ < std::numeric_limits<std::int32_t>::min() ||
                globalQ > std::numeric_limits<std::int32_t>::max() ||
                globalR < std::numeric_limits<std::int32_t>::min() ||
                globalR > std::numeric_limits<std::int32_t>::max()) {
                return {};
            }
            const AxialCoord global{static_cast<std::int32_t>(globalQ),
                                    static_cast<std::int32_t>(globalR)};
            const float centerValue = noise::value2D(
                static_cast<float>(global.q) * params.frequency,
                static_cast<float>(global.r) * params.frequency, worldSeed);
            output.cells[*index] = {
                static_cast<std::uint8_t>(centerValue < -0.2f ? 0 :
                                          (centerValue > 0.35f ? 2 : 1)),
                kInvalidOwner};
            for (std::uint32_t cornerIndex = 0; cornerIndex < 6; ++cornerIndex) {
                const HexCornerCoord localCorner = hexCorner(cell, cornerIndex);
                if (output.cornerHeights.contains(localCorner)) continue;
                const HexCornerCoord globalCorner = hexCorner(global, cornerIndex);
                constexpr float sqrtThreeOverTwo = 0.8660254037844386f;
                const float worldX = static_cast<float>(globalCorner.x2) * sqrtThreeOverTwo;
                const float worldZ = static_cast<float>(globalCorner.y2) * 0.5f;
                const float height = params.heightAmplitude * noise::value2D(
                    worldX * params.frequency, worldZ * params.frequency, worldSeed);
                if (!std::isfinite(height)) return {};
                output.cornerHeights.set(localCorner, height);
            }
        }
    }
    return output;
}

auto CellValue::voxelValue(std::uint16_t value) -> CellValue {
    CellValue result;
    result.kind = PolicyKind::Voxel;
    result.voxel = value;
    return result;
}

auto CellValue::heightValue(const HeightSample& value) -> CellValue {
    CellValue result;
    result.kind = PolicyKind::Height;
    result.height = value;
    return result;
}

auto CellValue::hexValue(const HexCell& value) -> CellValue {
    CellValue result;
    result.kind = PolicyKind::Hex;
    result.hex = value;
    return result;
}

RegionMap::RegionMap(std::int32_t tileExtentMeters, PolicyKind defaultPolicy)
    : tileExtentMeters_(tileExtentMeters > 0 ? tileExtentMeters : 512),
      defaultPolicy_(defaultPolicy) {
    F4_ASSERT(tileExtentMeters > 0, "region tile extent must be positive");
}

auto RegionMap::regionFor(std::int32_t worldX, std::int32_t worldZ) const -> RegionCoord {
    return {floorDiv(worldX, tileExtentMeters_), floorDiv(worldZ, tileExtentMeters_)};
}

auto RegionMap::policyAt(std::int32_t worldX, std::int32_t worldZ) const -> PolicyKind {
    const PolicyKind* policy = regions_.find(regionFor(worldX, worldZ));
    return policy != nullptr ? *policy : defaultPolicy_;
}

void RegionMap::set(RegionCoord coord, PolicyKind policy) { regions_.set(coord, policy); }

auto RegionMap::erase(RegionCoord coord) -> bool { return regions_.erase(coord); }

ChunkStore::ChunkStore(std::size_t capacity) : chunks_(capacity) {
    byCoord_.reserve(capacity);
}

auto ChunkStore::create(ChunkCoord coord, std::uint64_t worldSeed, PolicyKind policy,
                        const PolicyParams& params) -> ChunkHandle {
    if (const ChunkHandle* existing = byCoord_.find(coord); existing != nullptr) {
        return *existing;
    }
    const ChunkHandle handle = chunks_.acquire();
    ChunkRecord* chunk = chunks_.get(handle);
    chunk->coord = coord;
    chunk->worldSeed = worldSeed;
    chunk->policy = policy;
    chunk->params = params;
    byCoord_.set(coord, handle);
    return handle;
}

auto ChunkStore::destroy(ChunkHandle handle) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    byCoord_.erase(chunk->coord);
    chunks_.release(handle);
    return true;
}

auto ChunkStore::find(ChunkCoord coord) const -> ChunkHandle {
    const ChunkHandle* handle = byCoord_.find(coord);
    return handle != nullptr ? *handle : ChunkHandle{};
}

auto ChunkStore::state(ChunkHandle handle) const -> ChunkState {
    const ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return ChunkState::Invalid;
    std::lock_guard lock(chunk->mutex);
    return chunk->state;
}

auto ChunkStore::dirty(ChunkHandle handle) const -> DirtyFlags {
    const ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return {};
    std::lock_guard lock(chunk->mutex);
    return chunk->dirty;
}

auto ChunkStore::version(ChunkHandle handle) const -> std::uint64_t {
    const ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return 0;
    std::lock_guard lock(chunk->mutex);
    return chunk->version;
}

auto ChunkStore::liveCount() const -> std::size_t { return chunks_.liveCount(); }

auto ChunkStore::queue(ChunkHandle handle) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state != ChunkState::Invalid) return false;
    chunk->state = ChunkState::Queued;
    return true;
}

auto ChunkStore::beginGeneration(ChunkHandle handle)
    -> std::optional<ChunkGenerationRequest> {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return std::nullopt;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state != ChunkState::Queued) return std::nullopt;
    chunk->state = ChunkState::LayoutDone;
    return ChunkGenerationRequest{
        chunk->coord, chunk->worldSeed, chunk->policy, chunk->params, chunk->version};
}

auto generateChunkBase(const ChunkGenerationRequest& request)
    -> std::optional<ChunkBase> {
    ChunkBase generated;
    switch (request.policy) {
    case PolicyKind::Voxel: {
        VoxelCells cells =
            VoxelPolicy::generate(request.worldSeed, request.coord, request.params.voxel);
        if (cells.values.size() != kVoxelCellCount) return std::nullopt;
        std::optional<PackedVoxelCells> packed = packVoxelCells(cells);
        generated = packed.has_value() ? ChunkBase(std::move(*packed))
                                       : ChunkBase(std::move(cells));
        break;
    }
    case PolicyKind::Height:
        generated = HeightPolicy::generate(request.worldSeed, request.coord,
                                           request.params.height);
        if (std::get<HeightTile>(generated).samples.size() != kHeightSampleCount) {
            return std::nullopt;
        }
        break;
    case PolicyKind::Hex:
        generated = HexPolicy::generate(request.worldSeed, request.coord, request.params.hex);
        if (std::get<HexTile>(generated).cells.isEmpty()) return std::nullopt;
        break;
    }
    return generated;
}

namespace {

[[nodiscard]] auto validGeneratedBase(PolicyKind policy, const ChunkBase& generated) -> bool {
    switch (policy) {
    case PolicyKind::Voxel: {
        const VoxelCells* cells = std::get_if<VoxelCells>(&generated);
        if (cells != nullptr) return cells->values.size() == kVoxelCellCount;
        const PackedVoxelCells* packed = std::get_if<PackedVoxelCells>(&generated);
        if (packed == nullptr) return false;
        for (std::uint32_t index = 0; index < kVoxelCellCount; ++index) {
            if (!packedVoxelValue(*packed, index).has_value()) return false;
        }
        return true;
    }
    case PolicyKind::Height: {
        const HeightTile* tile = std::get_if<HeightTile>(&generated);
        return tile != nullptr && tile->samples.size() == kHeightSampleCount;
    }
    case PolicyKind::Hex: {
        const HexTile* tile = std::get_if<HexTile>(&generated);
        return tile != nullptr && !tile->cells.isEmpty();
    }
    }
    return false;
}

} // namespace

auto ChunkStore::publishGenerated(ChunkHandle handle,
                                  const ChunkGenerationRequest& request,
                                  ChunkBase generated) -> bool {
    if (!validGeneratedBase(request.policy, generated)) return false;
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state != ChunkState::LayoutDone || chunk->version != request.sourceVersion ||
        chunk->coord != request.coord || chunk->worldSeed != request.worldSeed ||
        chunk->policy != request.policy || chunk->params != request.params) {
        return false;
    }
    chunk->base = std::move(generated);
    chunk->state = ChunkState::CellsReady;
    chunk->dirty = {false, true, true};
    return true;
}

auto ChunkStore::generate(ChunkHandle handle) -> bool {
    const std::optional<ChunkGenerationRequest> request = beginGeneration(handle);
    if (!request.has_value()) return false;
    std::optional<ChunkBase> generated = generateChunkBase(*request);
    if (!generated.has_value()) {
        ChunkRecord* chunk = chunks_.get(handle);
        if (chunk != nullptr) {
            std::lock_guard lock(chunk->mutex);
            if (chunk->state == ChunkState::LayoutDone &&
                chunk->version == request->sourceVersion && chunk->coord == request->coord &&
                chunk->worldSeed == request->worldSeed && chunk->policy == request->policy &&
                chunk->params == request->params) {
                chunk->state = ChunkState::Queued;
            }
        }
        return false;
    }
    return publishGenerated(handle, *request, std::move(*generated));
}

auto ChunkStore::regenerate(ChunkHandle handle) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    {
        std::lock_guard lock(chunk->mutex);
        if (chunk->state < ChunkState::CellsReady) return false;
        if (chunk->version == std::numeric_limits<std::uint64_t>::max()) return false;
        ++chunk->version;
        chunk->state = ChunkState::Queued;
        chunk->dirty = {true, true, true};
    }
    return generate(handle);
}

auto ChunkStore::validCell(const ChunkRecord& chunk, std::uint32_t index,
                           const CellValue& value) const -> bool {
    if (value.kind != chunk.policy) return false;
    switch (chunk.policy) {
    case PolicyKind::Voxel: return index < kVoxelCellCount;
    case PolicyKind::Height: {
        if (index >= kHeightSampleCount) return false;
        const auto sum = static_cast<std::uint32_t>(value.height.materialWeights[0]) +
                         value.height.materialWeights[1] + value.height.materialWeights[2] +
                         value.height.materialWeights[3];
        return std::isfinite(value.height.height) && sum == 255;
    }
    case PolicyKind::Hex: {
        const HexTile* tile = std::get_if<HexTile>(&chunk.base);
        return tile != nullptr && index < tile->cells.size() &&
               tile->cells[index].terrainType != kInvalidTerrain &&
               value.hex.terrainType != kInvalidTerrain;
    }
    }
    return false;
}

auto ChunkStore::edit(ChunkHandle handle, std::uint32_t index, const CellValue& value) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    ChunkCoord coord;
    PolicyKind policy = PolicyKind::Voxel;
    std::array<ChunkCoord, 26> borderNeighbors{};
    std::array<std::uint32_t, 26> mirroredIndices{};
    mirroredIndices.fill(std::numeric_limits<std::uint32_t>::max());
    std::size_t borderNeighborCount = 0;
    const auto addNeighbor = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz,
                                 std::uint32_t mirroredIndex =
                                     std::numeric_limits<std::uint32_t>::max()) {
        const std::int64_t x = static_cast<std::int64_t>(coord.x) + dx;
        const std::int64_t y = static_cast<std::int64_t>(coord.y) + dy;
        const std::int64_t z = static_cast<std::int64_t>(coord.z) + dz;
        if (x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            y > std::numeric_limits<std::int32_t>::max() ||
            z < std::numeric_limits<std::int32_t>::min() ||
            z > std::numeric_limits<std::int32_t>::max()) {
            return;
        }
        borderNeighbors[borderNeighborCount++] = {
            static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(z)};
        mirroredIndices[borderNeighborCount - 1u] = mirroredIndex;
    };
    {
        std::lock_guard lock(chunk->mutex);
        if (chunk->state < ChunkState::CellsReady || !validCell(*chunk, index, value)) {
            return false;
        }
        if (chunk->version == std::numeric_limits<std::uint64_t>::max()) return false;
        coord = chunk->coord;
        policy = chunk->policy;
        switch (policy) {
        case PolicyKind::Voxel: {
            const std::uint32_t x = index & 31u;
            const std::uint32_t z = (index >> 5u) & 31u;
            const std::uint32_t y = (index >> 10u) & 31u;
            const std::int32_t dx = x == 0 ? -1 : (x + 1u == kVoxelEdge ? 1 : 0);
            const std::int32_t dy = y == 0 ? -1 : (y + 1u == kVoxelEdge ? 1 : 0);
            const std::int32_t dz = z == 0 ? -1 : (z + 1u == kVoxelEdge ? 1 : 0);
            for (std::int32_t useX = 0; useX <= (dx != 0 ? 1 : 0); ++useX) {
                for (std::int32_t useY = 0; useY <= (dy != 0 ? 1 : 0); ++useY) {
                    for (std::int32_t useZ = 0; useZ <= (dz != 0 ? 1 : 0); ++useZ) {
                        if (useX == 0 && useY == 0 && useZ == 0) continue;
                        addNeighbor(dx * useX, dy * useY, dz * useZ);
                    }
                }
            }
            break;
        }
        case PolicyKind::Height: {
            const std::uint32_t x = index % kHeightSamples;
            const std::uint32_t z = index / kHeightSamples;
            if (x == 0) addNeighbor(-1, 0, 0, heightSampleIndex(kHeightQuads, z));
            if (x == kHeightQuads) addNeighbor(1, 0, 0, heightSampleIndex(0, z));
            if (z == 0) addNeighbor(0, 0, -1, heightSampleIndex(x, kHeightQuads));
            if (z == kHeightQuads) addNeighbor(0, 0, 1, heightSampleIndex(x, 0));
            if (x == 0 && z == 0) {
                addNeighbor(-1, 0, -1, heightSampleIndex(kHeightQuads, kHeightQuads));
            } else if (x == 0 && z == kHeightQuads) {
                addNeighbor(-1, 0, 1, heightSampleIndex(kHeightQuads, 0));
            } else if (x == kHeightQuads && z == 0) {
                addNeighbor(1, 0, -1, heightSampleIndex(0, kHeightQuads));
            } else if (x == kHeightQuads && z == kHeightQuads) {
                addNeighbor(1, 0, 1, heightSampleIndex(0, 0));
            }
            break;
        }
        case PolicyKind::Hex: {
            const HexTile* tile = std::get_if<HexTile>(&chunk->base);
            if (tile == nullptr || tile->width == 0) return false;
            const std::int32_t r = static_cast<std::int32_t>(index / tile->width) -
                                   static_cast<std::int32_t>(tile->radius);
            const std::int32_t q = static_cast<std::int32_t>(index % tile->width) -
                                   floorDiv(r, 2) -
                                   static_cast<std::int32_t>(tile->radius);
            for (const AxialCoord direction : kHexDirections) {
                if (!tile->contains({q + direction.q, r + direction.r})) {
                    addNeighbor(direction.q, 0, direction.r);
                }
            }
            break;
        }
        }
        chunk->deltas.set(index, value);
        ++chunk->version;
        chunk->dirty = {true, true, true};
        if (chunk->state == ChunkState::Meshed) chunk->state = ChunkState::CellsReady;
    }

    // Never hold two chunk locks at once. Neighbor topology is main-thread
    // owned until M16, while each dirty/version transition remains synchronized.
    for (std::size_t i = 0; i < borderNeighborCount; ++i) {
        ChunkRecord* neighbor = chunks_.get(find(borderNeighbors[i]));
        if (neighbor == nullptr) continue;
        std::lock_guard neighborLock(neighbor->mutex);
        if (neighbor->policy != policy || neighbor->state < ChunkState::CellsReady ||
            neighbor->version == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        if (policy == PolicyKind::Height &&
            mirroredIndices[i] != std::numeric_limits<std::uint32_t>::max()) {
            neighbor->deltas.set(mirroredIndices[i], value);
            neighbor->dirty.cellsDirty = true;
        }
        neighbor->dirty.meshDirty = true;
        neighbor->dirty.bordersDirty = true;
        ++neighbor->version;
        if (neighbor->state == ChunkState::Meshed) neighbor->state = ChunkState::CellsReady;
    }
    return true;
}

auto ChunkStore::cell(ChunkHandle handle, std::uint32_t index) const
    -> std::optional<CellValue> {
    const ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return std::nullopt;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state < ChunkState::CellsReady) return std::nullopt;
    if (const CellValue* delta = chunk->deltas.find(index); delta != nullptr) return *delta;
    switch (chunk->policy) {
    case PolicyKind::Voxel: {
        const VoxelCells* cells = std::get_if<VoxelCells>(&chunk->base);
        if (cells != nullptr && index < cells->values.size()) {
            return CellValue::voxelValue(cells->values[index]);
        }
        const PackedVoxelCells* packed = std::get_if<PackedVoxelCells>(&chunk->base);
        if (packed == nullptr) return std::nullopt;
        const std::optional<std::uint16_t> value = packedVoxelValue(*packed, index);
        return value.has_value() ? std::optional<CellValue>(CellValue::voxelValue(*value))
                                 : std::nullopt;
    }
    case PolicyKind::Height: {
        const HeightTile* tile = std::get_if<HeightTile>(&chunk->base);
        if (tile == nullptr || index >= tile->samples.size()) return std::nullopt;
        return CellValue::heightValue(tile->samples[index]);
    }
    case PolicyKind::Hex: {
        const HexTile* tile = std::get_if<HexTile>(&chunk->base);
        if (tile == nullptr || index >= tile->cells.size() ||
            tile->cells[index].terrainType == kInvalidTerrain) {
            return std::nullopt;
        }
        return CellValue::hexValue(tile->cells[index]);
    }
    }
    return std::nullopt;
}

auto ChunkStore::snapshot(ChunkHandle handle) const -> std::optional<ChunkSnapshot> {
    const ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return std::nullopt;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state < ChunkState::CellsReady) return std::nullopt;
    ChunkSnapshot output;
    output.coord = chunk->coord;
    output.worldSeed = chunk->worldSeed;
    output.policy = chunk->policy;
    output.params = chunk->params;
    output.state = chunk->state;
    output.dirty = chunk->dirty;
    output.version = chunk->version;
    output.base = chunk->base;
    output.deltas.reserve(chunk->deltas.size());
    for (const auto& [index, value] : chunk->deltas) {
        output.deltas.push({index, value});
    }
    std::sort(output.deltas.begin(), output.deltas.end(),
              [](const ChunkDelta& a, const ChunkDelta& b) { return a.index < b.index; });
    return output;
}

auto ChunkStore::markDirty(ChunkHandle handle, DirtyFlags flags) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state < ChunkState::CellsReady) return false;
    const bool changed = flags.cellsDirty || flags.meshDirty || flags.bordersDirty;
    if (changed && chunk->version == std::numeric_limits<std::uint64_t>::max()) return false;
    chunk->dirty.cellsDirty = chunk->dirty.cellsDirty || flags.cellsDirty;
    chunk->dirty.meshDirty = chunk->dirty.meshDirty || flags.meshDirty;
    chunk->dirty.bordersDirty = chunk->dirty.bordersDirty || flags.bordersDirty;
    if (changed) ++chunk->version;
    if (changed && chunk->state == ChunkState::Meshed) {
        chunk->state = ChunkState::CellsReady;
    }
    return true;
}

auto ChunkStore::markMeshed(ChunkHandle handle, std::uint64_t snapshotVersion) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state < ChunkState::CellsReady || chunk->version != snapshotVersion) {
        return false;
    }
    chunk->state = ChunkState::Meshed;
    chunk->dirty = {};
    return true;
}

namespace {

constexpr std::uint32_t kDeltaChunkId = 0x44433446u; // "F4CD" little-endian
constexpr std::uint32_t kDeltaVersion = 1;

[[nodiscard]] constexpr auto cellWireBytes(PolicyKind policy) -> std::size_t {
    switch (policy) {
    case PolicyKind::Voxel: return sizeof(std::uint16_t);
    case PolicyKind::Height:
        return sizeof(float) + sizeof(std::uint16_t) + 4u * sizeof(std::uint8_t);
    case PolicyKind::Hex: return sizeof(std::uint8_t) + sizeof(std::uint32_t);
    }
    return 0;
}

void writeCell(memory::MemoryWriter& writer, const CellValue& value, PolicyKind policy) {
    switch (policy) {
    case PolicyKind::Voxel:
        writer.writeValue(value.voxel);
        break;
    case PolicyKind::Height:
        writer.writeValue(value.height.height);
        writer.writeValue(value.height.biomeId);
        for (std::uint8_t weight : value.height.materialWeights) writer.writeValue(weight);
        break;
    case PolicyKind::Hex:
        writer.writeValue(value.hex.terrainType);
        writer.writeValue(value.hex.owner);
        break;
    }
}

[[nodiscard]] auto readCell(memory::MemoryReader& reader, PolicyKind policy,
                            CellValue& output) -> bool {
    switch (policy) {
    case PolicyKind::Voxel: {
        std::uint16_t value = 0;
        if (!reader.readValue(value)) return false;
        output = CellValue::voxelValue(value);
        return true;
    }
    case PolicyKind::Height: {
        HeightSample value;
        if (!reader.readValue(value.height) || !reader.readValue(value.biomeId)) return false;
        for (std::uint8_t& weight : value.materialWeights) {
            if (!reader.readValue(weight)) return false;
        }
        output = CellValue::heightValue(value);
        return true;
    }
    case PolicyKind::Hex: {
        HexCell value;
        if (!reader.readValue(value.terrainType) || !reader.readValue(value.owner)) return false;
        output = CellValue::hexValue(value);
        return true;
    }
    }
    return false;
}

} // namespace

auto ChunkStore::saveDeltas(ChunkHandle handle, memory::MemoryWriter& output) const -> bool {
    output.reset();
    const std::optional<ChunkSnapshot> copy = snapshot(handle);
    if (!copy.has_value()) return false;
    if (copy->deltas.isEmpty()) return true;
    memory::MemoryWriter payload;
    payload.writeValue(copy->coord.x);
    payload.writeValue(copy->coord.y);
    payload.writeValue(copy->coord.z);
    payload.writeValue(copy->version);
    payload.writeValue(static_cast<std::uint32_t>(copy->deltas.size()));
    payload.writeValue(static_cast<std::uint8_t>(copy->policy));
    for (const ChunkDelta& delta : copy->deltas) {
        payload.writeValue(delta.index);
        writeCell(payload, delta.value, copy->policy);
    }
    memory::writeStreamPreamble(output);
    return memory::writeChunk(output, kDeltaChunkId, kDeltaVersion, payload.bytes().data(),
                              payload.bytes().size());
}

auto ChunkStore::loadDeltas(ChunkHandle handle, const memory::Array<std::byte>& input) -> bool {
    ChunkRecord* chunk = chunks_.get(handle);
    if (chunk == nullptr) return false;
    std::lock_guard lock(chunk->mutex);
    if (chunk->state < ChunkState::CellsReady) return false;
    if (input.isEmpty()) {
        if (chunk->version == std::numeric_limits<std::uint64_t>::max()) return false;
        chunk->deltas.clear();
        ++chunk->version;
        chunk->dirty = {true, true, true};
        if (chunk->state == ChunkState::Meshed) chunk->state = ChunkState::CellsReady;
        return true;
    }
    memory::MemoryReader stream(input);
    if (!memory::checkStreamPreamble(stream)) return false;
    memory::ChunkView chunkView;
    if (!memory::readChunkView(stream, chunkView) || !stream.atEnd() ||
        chunkView.id != kDeltaChunkId || chunkView.version != kDeltaVersion) {
        return false;
    }
    memory::MemoryReader payload(chunkView.payload, chunkView.payloadSize);
    ChunkCoord serializedCoord;
    std::uint64_t overlayVersion = 0;
    std::uint32_t count = 0;
    std::uint8_t serializedPolicy = 0;
    if (!payload.readValue(serializedCoord.x) || !payload.readValue(serializedCoord.y) ||
        !payload.readValue(serializedCoord.z) || !payload.readValue(overlayVersion) ||
        !payload.readValue(count) || !payload.readValue(serializedPolicy) ||
        serializedCoord != chunk->coord ||
        serializedPolicy != static_cast<std::uint8_t>(chunk->policy)) {
        return false;
    }
    const std::size_t wireBytes = sizeof(std::uint32_t) + cellWireBytes(chunk->policy);
    if (wireBytes == 0 || count > payload.remaining() / wireBytes ||
        payload.remaining() != static_cast<std::size_t>(count) * wireBytes) return false;
    containers::HashMap<std::uint32_t, CellValue> loaded;
    loaded.reserve(count);
    for (std::uint32_t entry = 0; entry < count; ++entry) {
        std::uint32_t cellIndex = 0;
        CellValue value;
        if (!payload.readValue(cellIndex) || !readCell(payload, chunk->policy, value) ||
            !validCell(*chunk, cellIndex, value) || !loaded.insert(cellIndex, value)) {
            return false;
        }
    }
    if (!payload.atEnd()) return false;
    const std::uint64_t priorVersion = std::max(chunk->version, overlayVersion);
    if (priorVersion == std::numeric_limits<std::uint64_t>::max()) return false;
    chunk->deltas = std::move(loaded);
    chunk->version = priorVersion + 1u;
    chunk->dirty = {true, true, true};
    if (chunk->state == ChunkState::Meshed) chunk->state = ChunkState::CellsReady;
    return true;
}

} // namespace f4::world
