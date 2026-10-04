#include "engine/world/world_streaming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <span>

#include "core/jobs/job_system.h"
#include "engine/world/terrain_regions.h"

namespace {

using namespace f4;
using namespace f4::world;

constexpr std::uint32_t kCellStage = 0;
constexpr std::uint32_t kMeshStage = 1;

[[nodiscard]] auto keyLess(const StreamingChunkKey& a, const StreamingChunkKey& b) -> bool {
    if (a.policy != b.policy) return a.policy < b.policy;
    if (a.coord.x != b.coord.x) return a.coord.x < b.coord.x;
    if (a.coord.y != b.coord.y) return a.coord.y < b.coord.y;
    return a.coord.z < b.coord.z;
}

struct StreamingKeyLess {
    auto operator()(const StreamingChunkKey& a, const StreamingChunkKey& b) const -> bool {
        return keyLess(a, b);
    }
};

[[nodiscard]] auto chunkExtent(PolicyKind policy, const PolicyParams& params) -> std::int32_t {
    switch (policy) {
    case PolicyKind::Voxel: return static_cast<std::int32_t>(kVoxelEdge);
    case PolicyKind::Height: return static_cast<std::int32_t>(kHeightQuads);
    case PolicyKind::Hex: {
        const std::uint64_t width = static_cast<std::uint64_t>(params.hex.radius) * 2u + 1u;
        return width <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
            ? static_cast<std::int32_t>(width)
            : 1;
    }
    }
    return 1;
}

[[nodiscard]] auto cameraChunk(float coordinate, std::int32_t extent)
    -> std::optional<std::int32_t> {
    if (!std::isfinite(coordinate) || extent <= 0) return std::nullopt;
    const double value = std::floor(static_cast<double>(coordinate) / extent);
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] auto addChunkOffset(std::int32_t value, std::int64_t offset)
    -> std::optional<std::int32_t> {
    const std::int64_t result = static_cast<std::int64_t>(value) + offset;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(result);
}

[[nodiscard]] auto cameraCenter(PolicyKind policy, math::Vec3 camera,
                                const WorldStreamingConfig& config) -> std::optional<ChunkCoord> {
    if (policy == PolicyKind::Hex) {
        const auto cell = hexCellAt(camera, config.hexRadius);
        return cell ? hexChunkAt(*cell, config.policyParams.hex.radius, config.policyParams.hex.layout) : std::nullopt;
    }
    const auto extent = chunkExtent(policy, config.policyParams);
    const auto x = cameraChunk(camera.x, extent);
    const auto y = policy == PolicyKind::Voxel ? cameraChunk(camera.y, extent) : std::optional<std::int32_t>(0);
    const auto z = cameraChunk(camera.z, extent);
    return x && y && z ? std::optional<ChunkCoord>({*x, *y, *z}) : std::nullopt;
}

[[nodiscard]] auto chunkCenter(StreamingChunkKey key, const WorldStreamingConfig& config)
    -> std::optional<std::array<long double, 3>> {
    if (key.policy == PolicyKind::Hex) {
        const auto origin = hexChunkOrigin(key.coord, config.policyParams.hex.radius, config.policyParams.hex.layout);
        if (!origin || !std::isfinite(config.hexRadius) || config.hexRadius <= 0.f) return std::nullopt;
        return std::array<long double, 3>{
            (2LL * origin->q + origin->r) * 0.8660254037844386468L * config.hexRadius,
            0.L, origin->r * 1.5L * config.hexRadius};
    }
    const auto extent = chunkExtent(key.policy, config.policyParams);
    return std::array<long double, 3>{static_cast<long double>(key.coord.x) * extent + extent * 0.5L,
        key.policy == PolicyKind::Voxel ? static_cast<long double>(key.coord.y) * extent + extent * 0.5L : 0.L,
        static_cast<long double>(key.coord.z) * extent + extent * 0.5L};
}

struct HorizontalBounds {
    std::int64_t minX, maxX, minZ, maxZ;
};

[[nodiscard]] auto terrainOrigin(StreamingChunkKey key, const WorldStreamingConfig& config) -> math::Vec3 {
    const float x = static_cast<float>(key.coord.x), y = static_cast<float>(key.coord.y), z = static_cast<float>(key.coord.z);
    if (key.policy == PolicyKind::Voxel) return {x * kVoxelEdge, y * kVoxelEdge, z * kVoxelEdge};
    if (key.policy == PolicyKind::Height) return {x * kHeightQuads, 0.f, z * kHeightQuads};
    const auto origin = hexChunkOrigin(key.coord, config.policyParams.hex.radius, config.policyParams.hex.layout);
    if (!origin) return {};
    return {static_cast<float>(2LL * origin->q + origin->r) * 0.8660254037844386f * config.hexRadius,
            0.f, static_cast<float>(origin->r) * 1.5f * config.hexRadius};
}

[[nodiscard]] auto chunkFootprint(StreamingChunkKey key, const WorldStreamingConfig& config) -> TerrainFootprint {
    const auto origin = terrainOrigin(key, config);
    if (key.policy == PolicyKind::Hex) {
        const auto radius = config.policyParams.hex.radius;
        if (config.regionalSurfaceOwnership) {
            const auto axial = hexChunkOrigin(key.coord, radius, config.policyParams.hex.layout);
            if (!axial) return {NAN, NAN, NAN, NAN};
            const std::int64_t x2 = 2LL * axial->q + axial->r, z2 = 3LL * axial->r;
            const std::int64_t rx = 2LL * radius + 1, rz = 3LL * radius + 2;
            const auto low = hexLatticePosition({x2 - rx, z2 - rz}, config.hexRadius);
            const auto high = hexLatticePosition({x2 + rx, z2 + rz}, config.hexRadius);
            if (!low || !high) return {NAN, NAN, NAN, NAN};
            return {low.value().x, high.value().x, low.value().y, high.value().y};
        }
        // Same local corner-lattice arithmetic as meshHexChunk; conservative
        // rectangular bounds can admit an empty clipped mesh at a hex corner.
        const float x = static_cast<float>(2ull * radius + 1) * 0.8660254037844386f * config.hexRadius;
        const float z = static_cast<float>(3ull * radius + 2) * 0.5f * config.hexRadius;
        return {double(origin.x) - x, double(origin.x) + x, double(origin.z) - z, double(origin.z) + z};
    }
    const auto extent = chunkExtent(key.policy, config.policyParams);
    return {origin.x, double(origin.x) + extent, origin.z, double(origin.z) + extent};
}

[[nodiscard]] auto horizontalBounds(PolicyKind policy, math::Vec3 camera,
                                    const WorldStreamingConfig& config)
    -> std::optional<HorizontalBounds> {
    constexpr auto lo = std::numeric_limits<std::int32_t>::min();
    constexpr auto hi = std::numeric_limits<std::int32_t>::max();
    if (!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z)) return std::nullopt;
    if (!config.horizontalHalfExtentMeters) {
        const auto center = cameraCenter(policy, camera, config);
        if (!center) return std::nullopt;
        const std::int64_t radius = config.horizontalRadius;
        return HorizontalBounds{std::max<std::int64_t>(lo, center->x - radius),
            std::min<std::int64_t>(hi, center->x + radius),
            std::max<std::int64_t>(lo, center->z - radius),
            std::min<std::int64_t>(hi, center->z + radius)};
    }

    // Invert the same center lattice used for rendering/region selection. The
    // inverse of an axis-aligned square is a parallelogram for either hex layout.
    const auto origin = chunkCenter({policy, {}}, config);
    const auto xStep = chunkCenter({policy, {1, 0, 0}}, config);
    const auto zStep = chunkCenter({policy, {0, 0, 1}}, config);
    if (!origin || !xStep || !zStep) return std::nullopt;
    const long double a = (*xStep)[0] - (*origin)[0], b = (*zStep)[0] - (*origin)[0];
    const long double c = (*xStep)[2] - (*origin)[2], d = (*zStep)[2] - (*origin)[2];
    const long double determinant = a * d - b * c;
    if (!std::isfinite(determinant) || determinant <= 0.L) return std::nullopt;
    const long double radius = *config.horizontalHalfExtentMeters;
    long double minX = std::numeric_limits<long double>::infinity(), minZ = minX;
    long double maxX = -minX, maxZ = -minX;
    for (const int sx : {-1, 1}) {
        for (const int sz : {-1, 1}) {
            const long double x = static_cast<long double>(camera.x) + sx * radius - (*origin)[0];
            const long double z = static_cast<long double>(camera.z) + sz * radius - (*origin)[2];
            const long double cx = (d * x - b * z) / determinant;
            const long double cz = (a * z - c * x) / determinant;
            minX = std::min(minX, cx); maxX = std::max(maxX, cx);
            minZ = std::min(minZ, cz); maxZ = std::max(maxZ, cz);
        }
    }
    // One conservative lattice-cell margin protects exact-edge candidates from
    // inverse rounding. Forward center tests below decide actual admission.
    minX = std::max(std::floor(minX) - 1.L, static_cast<long double>(lo));
    minZ = std::max(std::floor(minZ) - 1.L, static_cast<long double>(lo));
    maxX = std::min(std::ceil(maxX) + 1.L, static_cast<long double>(hi));
    maxZ = std::min(std::ceil(maxZ) + 1.L, static_cast<long double>(hi));
    if (minX > maxX || minZ > maxZ) return std::nullopt;
    return HorizontalBounds{static_cast<std::int64_t>(minX), static_cast<std::int64_t>(maxX),
        static_cast<std::int64_t>(minZ), static_cast<std::int64_t>(maxZ)};
}

[[nodiscard]] auto squaredDistance(StreamingChunkKey key, math::Vec3 camera,
                                   const WorldStreamingConfig& config) -> std::uint64_t {
    const auto center = chunkCenter(key, config);
    if (!center) return std::numeric_limits<std::uint64_t>::max();
    const long double dx = (*center)[0] - static_cast<long double>(camera.x);
    const long double dy = (*center)[1] - static_cast<long double>(camera.y);
    const long double dz = (*center)[2] - static_cast<long double>(camera.z);
    const long double distance = dx * dx + dy * dy + dz * dz;
    if (distance >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(distance);
}

[[nodiscard]] auto deltaValue(const ChunkSnapshot& snapshot, std::uint32_t index)
    -> const CellValue* {
    const auto found = std::lower_bound(
        snapshot.deltas.begin(), snapshot.deltas.end(), index,
        [](const ChunkDelta& delta, std::uint32_t target) { return delta.index < target; });
    return found != snapshot.deltas.end() && found->index == index ? &found->value : nullptr;
}

[[nodiscard]] auto snapshotVoxel(const ChunkSnapshot& snapshot, std::uint32_t index)
    -> std::optional<std::uint16_t> {
    if (snapshot.policy != PolicyKind::Voxel || index >= kVoxelCellCount) return std::nullopt;
    if (const CellValue* delta = deltaValue(snapshot, index); delta != nullptr) {
        return delta->kind == PolicyKind::Voxel ? std::optional<std::uint16_t>(delta->voxel)
                                                : std::nullopt;
    }
    const VoxelCells* cells = std::get_if<VoxelCells>(&snapshot.base);
    if (cells != nullptr && cells->values.size() == kVoxelCellCount) {
        return cells->values[index];
    }
    const PackedVoxelCells* packed = std::get_if<PackedVoxelCells>(&snapshot.base);
    return packed != nullptr ? packedVoxelValue(*packed, index) : std::nullopt;
}

[[nodiscard]] auto snapshotHeight(const ChunkSnapshot& snapshot, std::uint32_t index)
    -> std::optional<HeightSample> {
    if (snapshot.policy != PolicyKind::Height || index >= kHeightSampleCount) {
        return std::nullopt;
    }
    if (const CellValue* delta = deltaValue(snapshot, index); delta != nullptr) {
        return delta->kind == PolicyKind::Height ? std::optional<HeightSample>(delta->height)
                                                 : std::nullopt;
    }
    const HeightTile* tile = std::get_if<HeightTile>(&snapshot.base);
    if (tile == nullptr || tile->samples.size() != kHeightSampleCount) return std::nullopt;
    return tile->samples[index];
}

[[nodiscard]] auto snapshotHexPresent(const ChunkSnapshot& snapshot, AxialCoord global) -> bool {
    if (snapshot.policy != PolicyKind::Hex) return false;
    const HexTile* tile = std::get_if<HexTile>(&snapshot.base);
    if (tile == nullptr || tile->width == 0) return false;
    const auto origin = hexChunkOrigin(snapshot.coord, tile->radius, tile->layout);
    if (!origin) return false;
    const std::int64_t localQ = static_cast<std::int64_t>(global.q) - origin->q;
    const std::int64_t localR = static_cast<std::int64_t>(global.r) - origin->r;
    if (localQ < std::numeric_limits<std::int32_t>::min() ||
        localQ > std::numeric_limits<std::int32_t>::max() ||
        localR < std::numeric_limits<std::int32_t>::min() ||
        localR > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    const std::optional<std::uint32_t> index = tile->index(
        {static_cast<std::int32_t>(localQ), static_cast<std::int32_t>(localR)});
    if (!index.has_value()) return false;
    if (const CellValue* delta = deltaValue(snapshot, *index); delta != nullptr) {
        return delta->kind == PolicyKind::Hex && delta->hex.terrainType != kInvalidTerrain;
    }
    return tile->cells[*index].terrainType != kInvalidTerrain;
}

[[nodiscard]] auto heightSkirtEdges(const ChunkSnapshot& center,
                                   const std::vector<ChunkSnapshot>& neighbors) -> std::uint8_t {
    std::uint8_t edges = 0x0f;
    constexpr std::array<ChunkCoord, 4> directions{{{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}}};
    for (std::size_t edge = 0; edge < directions.size(); ++edge) {
        for (const auto& neighbor : neighbors) {
            if (neighbor.policy != PolicyKind::Height || neighbor.coord.y != center.coord.y ||
                static_cast<std::int64_t>(neighbor.coord.x) - center.coord.x != directions[edge].x ||
                static_cast<std::int64_t>(neighbor.coord.z) - center.coord.z != directions[edge].z) continue;
            bool matches = true;
            for (std::uint32_t i = 0; i < kHeightSamples; ++i) {
                const auto cx = edge == 1 ? kHeightQuads : edge == 3 ? 0u : i;
                const auto cz = edge == 0 ? 0u : edge == 2 ? kHeightQuads : i;
                const auto nx = edge == 1 ? 0u : edge == 3 ? kHeightQuads : i;
                const auto nz = edge == 0 ? kHeightQuads : edge == 2 ? 0u : i;
                const auto a = snapshotHeight(center, heightSampleIndex(cx, cz));
                const auto b = snapshotHeight(neighbor, heightSampleIndex(nx, nz));
                if (!a || !b || a->height != b->height) { matches = false; break; }
            }
            if (matches) edges &= static_cast<std::uint8_t>(~(1u << edge));
            break;
        }
    }
    return edges;
}

auto mapSolidError(TerrainSolidError error) -> StreamingSolidError {
    switch (error) {
    case TerrainSolidError::InvalidData: return StreamingSolidError::InvalidData;
    case TerrainSolidError::IncompleteProfile: return StreamingSolidError::IncompleteProfile;
    case TerrainSolidError::PrecisionLoss: return StreamingSolidError::PrecisionLoss;
    case TerrainSolidError::DiscontinuousHeight: return StreamingSolidError::DiscontinuousHeight;
    case TerrainSolidError::NonmanifoldCorner: return StreamingSolidError::NonmanifoldCorner;
    case TerrainSolidError::InconsistentSector: return StreamingSolidError::InconsistentSector;
    case TerrainSolidError::HexFloorInversion: return StreamingSolidError::HexFloorInversion;
    case TerrainSolidError::UnsupportedUnderside: return StreamingSolidError::UnsupportedUnderside;
    case TerrainSolidError::UnsupportedVoxel: return StreamingSolidError::UnsupportedVoxel;
    case TerrainSolidError::VoxelCavity: return StreamingSolidError::VoxelCavity;
    case TerrainSolidError::VoxelOverhang: return StreamingSolidError::VoxelOverhang;
    }
    return StreamingSolidError::InvalidData;
}

auto solidResult(Result<TerrainSolidSample, TerrainSolidError> result)
    -> Result<TerrainSolidSample, StreamingSolidError> {
    if (!result) return mapSolidError(result.error());
    return result.value();
}

auto onRegionPlane(double coordinate, double extent) -> bool {
    if (!std::isfinite(coordinate) || !(extent > 0)) return false;
    const double quot = coordinate / extent;
    if (!std::isfinite(quot)) return false;
    auto index = static_cast<std::int64_t>(std::floor(quot));
    if (static_cast<double>(index) * extent > coordinate) --index;
    return static_cast<double>(index) * extent == coordinate;
}

auto narrowFloor(double value) -> std::optional<std::int32_t> {
    if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    const double floored = std::floor(value);
    if (floored < std::numeric_limits<std::int32_t>::min() ||
        floored > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(floored);
}

auto integralPlane(double plane) -> std::optional<std::int32_t> {
    if (!std::isfinite(plane) || plane < std::numeric_limits<std::int32_t>::min() ||
        plane > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    const auto narrowed = static_cast<std::int32_t>(plane);
    return static_cast<double>(narrowed) == plane ? std::optional<std::int32_t>(narrowed) : std::nullopt;
}

auto segmentBegin(double tangent, double extent) -> std::optional<double> {
    if (!std::isfinite(tangent) || !(extent > 0)) return std::nullopt;
    const double quot = tangent / extent;
    if (!std::isfinite(quot)) return std::nullopt;
    auto index = static_cast<std::int64_t>(std::floor(quot));
    if (static_cast<double>(index) * extent > tangent) --index;
    const double begin = static_cast<double>(index) * extent;
    const double end = static_cast<double>(index + 1) * extent;
    if (!std::isfinite(begin) || !std::isfinite(end) || !(begin < end) || tangent < begin || tangent > end) {
        return std::nullopt;
    }
    return begin;
}

} // namespace

auto std::hash<f4::world::StreamingChunkKey>::operator()(
    const f4::world::StreamingChunkKey& value) const noexcept -> std::size_t {
    const std::size_t coordinate = std::hash<f4::world::ChunkCoord>{}(value.coord);
    const std::size_t policy = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(value.policy));
    return coordinate ^ (policy + 0x9e3779b9u + (coordinate << 6u) + (coordinate >> 2u));
}

namespace f4::world {

struct StreamingBoundarySnapshot::Data {
    std::size_t meshBytes = 0;
    StreamingBoundaryRequest request;
    std::uint64_t revision = 0;
    std::shared_ptr<const std::byte> owner;
    std::array<std::vector<StreamingBoundarySource>, 2> sources;
    std::array<std::vector<MeshData>, 2> meshes;
    std::array<std::vector<math::Vec3>, 2> origins;
};

struct StreamingBoundaryReplacement::Data {
    StreamingBoundarySnapshot snapshot;
    TerrainBoundaryReplacement geometry;
};

auto StreamingBoundarySnapshot::meshBytes() const -> std::size_t {
    return data_ ? data_->meshBytes : 0;
}

auto StreamingBoundaryReplacement::geometry() const -> const TerrainBoundaryReplacement* {
    return data_ ? &data_->geometry : nullptr;
}

auto StreamingBoundaryReplacement::sources(bool positive) const -> std::span<const StreamingBoundarySource> {
    return data_ ? data_->snapshot.sources(positive) : std::span<const StreamingBoundarySource>{};
}

auto StreamingBoundarySnapshot::buildReplacement(BoundaryReplacementLimits limits, std::uint16_t negativeMaterial,
                                                std::uint16_t positiveMaterial) const
    -> Result<StreamingBoundaryReplacement, BoundaryReplacementError> {
    if (!data_) return BoundaryReplacementError::InvalidData;
    std::array<std::vector<TerrainBoundaryMesh>, 2> views;
    for (unsigned side = 0; side < 2; ++side)
        for (std::size_t i = 0; i < data_->meshes[side].size(); ++i)
            views[side].push_back({&data_->meshes[side][i], data_->origins[side][i]});
    auto built = buildTerrainBoundaryReplacement(views[0], views[1], data_->request.line, limits, negativeMaterial, positiveMaterial);
    if (!built) return built.error();
    auto data = std::make_shared<StreamingBoundaryReplacement::Data>();
    data->snapshot = *this; data->geometry = std::move(built).value();
    StreamingBoundaryReplacement result; result.data_ = std::move(data);
    return result;
}

auto StreamingBoundarySnapshot::profile(std::size_t segmentLimit) const
    -> Result<containers::Array<TerrainBoundarySpan>, TerrainBoundaryError> {
    if (!data_) return TerrainBoundaryError::InvalidData;
    std::array<std::vector<TerrainBoundaryMesh>, 2> views;
    for (unsigned side = 0; side < 2; ++side)
        for (std::size_t i = 0; i < data_->meshes[side].size(); ++i)
            views[side].push_back({&data_->meshes[side][i], data_->origins[side][i]});
    return compareTerrainBoundaryTops(views[0], views[1], data_->request.line, segmentLimit);
}

auto StreamingBoundarySnapshot::sources(bool positive) const -> std::span<const StreamingBoundarySource> {
    if (!data_) return {};
    return data_->sources[positive ? 1 : 0];
}

auto StreamingBoundarySnapshot::regionRevision() const -> std::optional<std::uint64_t> {
    return data_ ? std::optional<std::uint64_t>(data_->revision) : std::nullopt;
}

struct StreamingBoundaryComposition::Data {
    std::vector<StreamingBoundarySnapshot> snapshots;
    std::vector<StreamingBoundarySource> sources;
    TerrainBoundaryComposition geometry;
};

auto StreamingBoundaryComposition::build(std::span<const StreamingBoundarySnapshot> snapshots,
                                         BoundaryCompositionLimits limits, std::size_t snapshotBytes)
    -> Result<StreamingBoundaryComposition, BoundaryReplacementError> {
    using Error = BoundaryReplacementError;
    if (snapshots.empty() || limits.maxBoundaries == 0 || limits.maxBoundaries > 256) return Error::InvalidData;
    if (snapshots.size() > limits.maxBoundaries) return Error::LimitExceeded;
    auto data = std::make_shared<Data>();
    std::vector<TerrainBoundaryMesh> sources;
    std::vector<std::array<std::vector<std::size_t>, 2>> memberships(snapshots.size());
    std::vector<TerrainBoundaryRequest> requests;
    std::size_t bytes = 0;
    for (std::size_t b = 0; b < snapshots.size(); ++b) {
        if (!snapshots[b].data_) return Error::InvalidData;
        const auto& capture = *snapshots[b].data_;
        const auto& first = *snapshots[0].data_;
        if (capture.owner != first.owner || capture.revision != first.revision) return Error::InvalidData;
        for (unsigned side = 0; side < 2; ++side) {
            for (std::size_t i = 0; i < capture.sources[side].size(); ++i) {
                const auto& mesh = capture.meshes[side][i];
                const auto account = [&](std::size_t count, std::size_t size) {
                    if (count > (snapshotBytes - bytes) / size) return false;
                    bytes += count * size; return true;
                };
                if (!account(mesh.vertices.size(), sizeof(TerrainVertex)) || !account(mesh.indices.size(), sizeof(std::uint32_t)) ||
                    !account(mesh.surfaces.size(), sizeof(TerrainSurface))) return Error::LimitExceeded;
                const auto& source = capture.sources[side][i];
                const auto found = std::find_if(data->sources.begin(), data->sources.end(), [&](const auto& s) { return s.key == source.key; });
                std::size_t index = static_cast<std::size_t>(found - data->sources.begin());
                if (found == data->sources.end()) {
                    if (data->sources.size() == limits.mesh.maxSources) return Error::LimitExceeded;
                    data->sources.push_back(source);
                    sources.push_back({&mesh, capture.origins[side][i]});
                } else if (found->handle != source.handle || found->cellVersion != source.cellVersion ||
                           found->meshVersion != source.meshVersion || sources[index].translation != capture.origins[side][i]) {
                    return Error::InvalidData;
                }
                memberships[b][side].push_back(index);
            }
        }
        requests.push_back({capture.request.line, memberships[b][0], memberships[b][1]});
        data->snapshots.push_back(snapshots[b]);
    }
    auto built = composeTerrainBoundaryReplacements(sources, requests, limits);
    if (!built) return built.error();
    data->geometry = std::move(built).value();
    StreamingBoundaryComposition result; result.data_ = std::move(data); return result;
}

auto StreamingBoundaryComposition::geometry() const -> const TerrainBoundaryComposition* {
    return data_ ? &data_->geometry : nullptr;
}
auto StreamingBoundaryComposition::sources() const -> std::span<const StreamingBoundarySource> {
    return data_ ? std::span<const StreamingBoundarySource>{data_->sources} : std::span<const StreamingBoundarySource>{};
}
auto StreamingBoundaryComposition::raycast(math::Vec3 origin, math::Vec3 direction, float maxDistance,
                                           bool cullBackFaces) const -> std::optional<StreamingBoundaryHit> {
    if (!data_) return {};
    auto hit = raycastTerrainMesh(data_->geometry.mesh, origin, direction, maxDistance, {}, cullBackFaces);
    if (!hit) return {};
    const auto triangle = hit->triangleIndex;
    const auto& stamp = data_->geometry.triangles[triangle];
    const auto& source = data_->sources[stamp.sourceIndex];
    hit->triangleIndex = stamp.sourceTriangle;
    return StreamingBoundaryHit{{source.key, source.handle, source.meshVersion, *hit, data_->snapshots[0].regionRevision()},
        triangle, stamp.wall ? std::optional<std::size_t>{stamp.boundaryIndex} : std::nullopt};
}

struct StreamingSurfaceView::Data {
    std::vector<StreamingSurfaceSource> sources;
    std::optional<StreamingBoundaryComposition> composition;
};

auto StreamingSurfaceView::build(std::span<const StreamingSurfaceSource> sources,
                                 std::optional<StreamingBoundaryComposition> composition)
    -> Result<StreamingSurfaceView, StreamingSurfaceViewError> {
    if (sources.empty()) return StreamingSurfaceViewError::InvalidData;
    auto data = std::make_shared<Data>();
    data->sources.assign(sources.begin(), sources.end());
    for (const auto& source : data->sources) {
        if (!source.handle.isValid() || !source.owner || !source.geometry ||
            !std::isfinite(source.origin.x) || !std::isfinite(source.origin.y) ||
            !std::isfinite(source.origin.z)) return StreamingSurfaceViewError::InvalidData;
        const auto duplicate = std::count_if(data->sources.begin(), data->sources.end(),
            [&](const auto& candidate) { return candidate.key == source.key; });
        if (duplicate != 1) return StreamingSurfaceViewError::DuplicateSource;
    }
    if (composition) {
        for (const auto& dependency : composition->sources()) {
            const auto found = std::find_if(data->sources.begin(), data->sources.end(),
                [&](const auto& source) { return source.key == dependency.key; });
            if (found == data->sources.end() || found->handle != dependency.handle ||
                found->meshVersion != dependency.meshVersion)
                return StreamingSurfaceViewError::MissingCompositionSource;
        }
        data->composition = std::move(composition);
    }
    StreamingSurfaceView result;
    result.data_ = std::move(data);
    return result;
}

auto StreamingSurfaceView::sources() const -> std::span<const StreamingSurfaceSource> {
    return data_ ? std::span<const StreamingSurfaceSource>{data_->sources} : std::span<const StreamingSurfaceSource>{};
}

auto StreamingSurfaceView::composition() const -> const StreamingBoundaryComposition* {
    return data_ && data_->composition ? &*data_->composition : nullptr;
}

auto StreamingSurfaceView::raycast(math::Vec3 origin, math::Vec3 direction, float maxDistance,
                                  bool cullBackFaces) const -> std::optional<StreamingBoundaryHit> {
    if (!data_) return {};
    std::optional<StreamingBoundaryHit> closest = data_->composition
        ? data_->composition->raycast(origin, direction, maxDistance, cullBackFaces)
        : std::optional<StreamingBoundaryHit>{};
    for (const auto& source : data_->sources) {
        if (data_->composition && std::any_of(data_->composition->sources().begin(),
            data_->composition->sources().end(), [&](const auto& dependency) {
                return dependency.key == source.key;
            })) continue;
        const auto hit = raycastTerrainMesh(*source.geometry, origin, direction, maxDistance,
                                             source.origin, cullBackFaces);
        if (!hit) continue;
        const auto keyTuple = [](const auto& key) {
            return std::tuple{key.policy, key.coord.x, key.coord.y, key.coord.z};
        };
        if (!closest || hit->distance < closest->source.surface.distance ||
            (hit->distance == closest->source.surface.distance &&
             keyTuple(source.key) < keyTuple(closest->source.key))) {
            closest = StreamingBoundaryHit{
                {source.key, source.handle, source.meshVersion, *hit, source.regionRevision}, 0, {}};
        }
    }
    return closest;
}

class WorldStreamer::Impl {
public:
    explicit Impl(const WorldStreamingConfig& config)
        : config_(config),
          regions_(config.regionExtentMeters, config.defaultPolicy),
          voxelStore_(std::max<std::size_t>(config.chunkCapacityPerPolicy, 1)),
          heightStore_(std::max<std::size_t>(config.chunkCapacityPerPolicy, 1)),
          hexStore_(std::max<std::size_t>(config.chunkCapacityPerPolicy, 1)),
          jobs_(config.workerCount == 0 ? std::thread::hardware_concurrency()
                                        : config.workerCount) {
        config_.chunkCapacityPerPolicy =
            std::max<std::size_t>(config_.chunkCapacityPerPolicy, 1);
    }

    ~Impl() {
        if (planning_) planning_->cancelled.store(true);
        jobs_.shutdown();
    }

    struct Entry {
        ChunkHandle handle;
        std::uint64_t distanceSquared = 0;
        bool generationInFlight = false;
        bool meshInFlight = false;
        std::optional<std::uint64_t> failedMeshVersion;
        std::optional<MeshData> mesh;
        std::uint64_t meshVersion = 0;
        std::optional<std::uint64_t> meshRegionRevision{};
    };

    struct Candidate {
        StreamingChunkKey key;
        std::uint64_t distanceSquared = 0;
    };

    struct PlanningWork {
        PlanningWork(const WorldStreamingConfig& settings, const RegionMap& map,
                     math::Vec3 eye) : config(settings), regions(map), camera(eye) {}
        WorldStreamingConfig config;
        RegionMap regions;
        math::Vec3 camera;
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::optional<std::vector<Candidate>> result;
        bool failed = false;
    };

    [[nodiscard]] static auto nearer(const Candidate& a, const Candidate& b) -> bool {
        if (a.distanceSquared != b.distanceSquared) return a.distanceSquared < b.distanceSquared;
        return keyLess(a.key, b.key);
    }

    enum class CompletionKind : std::uint8_t { Cells, Mesh };

    struct Completion {
        CompletionKind kind = CompletionKind::Cells;
        StreamingChunkKey key;
        ChunkHandle handle;
        jobs::JobPriority priority;
        std::uint64_t sourceVersion = 0;
        ChunkGenerationRequest generation;
        std::optional<ChunkBase> base;
        std::optional<TerrainMeshBuild> mesh;
        std::optional<std::uint64_t> regionRevision;
    };

    struct MeshWork {
        StreamingChunkKey key;
        ChunkHandle handle;
        std::uint64_t distanceSquared = 0;
        ChunkSnapshot center;
        std::vector<ChunkSnapshot> neighborhood;
        BlockRegistry blocks;
        float heightSkirtDepth = 4.0f;
        std::optional<float> heightTransitionFloor;
        float hexRadius = 1.0f;
        float hexFloorHeight = -8.0f;
        std::optional<RegionMap> regions;
        math::Vec3 origin;
        bool hexCutWalls = false;
        bool voxelCutFaces = false;
    };

    [[nodiscard]] auto storeFor(PolicyKind policy) -> ChunkStore& {
        switch (policy) {
        case PolicyKind::Voxel: return voxelStore_;
        case PolicyKind::Height: return heightStore_;
        case PolicyKind::Hex: return hexStore_;
        }
        return voxelStore_;
    }

    [[nodiscard]] auto storeFor(PolicyKind policy) const -> const ChunkStore& {
        switch (policy) {
        case PolicyKind::Voxel: return voxelStore_;
        case PolicyKind::Height: return heightStore_;
        case PolicyKind::Hex: return hexStore_;
        }
        return voxelStore_;
    }

    [[nodiscard]] auto withinBudget(std::chrono::steady_clock::time_point start) const -> bool {
        if (config_.frameBudget.count() <= 0) return false;
        // Count caps are the admission limit. The clock is not a deadline.
        // A hard StreamingDeadline does not use this function.
        if (config_.deterministicAdmission) return true;
        return std::chrono::steady_clock::now() - start < config_.frameBudget;
    }

    [[nodiscard]] static auto desiredChunks(const WorldStreamingConfig& config,
                                            const RegionMap& regions, math::Vec3 camera,
                                            const std::atomic<bool>* cancelled = nullptr)
        -> std::vector<Candidate> {
        std::vector<Candidate> output;
        if (!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z)) {
            return output;
        }
        constexpr PolicyKind policies[]{PolicyKind::Voxel, PolicyKind::Height, PolicyKind::Hex};
        for (PolicyKind policy : policies) {
            std::vector<Candidate> nearest;
            const auto bounds = horizontalBounds(policy, camera, config);
            const auto centerY = policy == PolicyKind::Voxel ? cameraChunk(camera.y, kVoxelEdge)
                                                             : std::optional<std::int32_t>(0);
            if (!bounds || !centerY) continue;
            const std::int64_t vertical = policy == PolicyKind::Voxel
                ? static_cast<std::int64_t>(config.voxelVerticalRadius)
                : 0;
            for (std::int64_t z = bounds->minZ; z <= bounds->maxZ; ++z) {
                for (std::int64_t x = bounds->minX; x <= bounds->maxX; ++x) {
                    if (cancelled != nullptr && cancelled->load()) return {};
                    const auto cx = static_cast<std::int32_t>(x), cz = static_cast<std::int32_t>(z);
                    const auto position = chunkCenter({policy, {cx, 0, cz}}, config);
                    if (!position) continue;
                    if (config.horizontalHalfExtentMeters &&
                        (std::abs((*position)[0] - camera.x) > *config.horizontalHalfExtentMeters ||
                         std::abs((*position)[2] - camera.z) > *config.horizontalHalfExtentMeters)) continue;
                    const auto px = std::floor((*position)[0]);
                    const auto pz = std::floor((*position)[2]);
                    if (config.regionalSurfaceOwnership) {
                        const auto mask = terrainRegionMask(chunkFootprint({policy, {cx, 0, cz}}, config), regions, policy);
                        if (!mask) {
                            if (mask.error() == TerrainMeshError::CoordinateOverflow) continue;
                            throw std::runtime_error("Regional chunk footprint invalid or exceeds 4096 region tiles");
                        }
                        if (mask.value().rectangles.isEmpty()) continue;
                    } else if (px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
                               pz < std::numeric_limits<std::int32_t>::min() || pz > std::numeric_limits<std::int32_t>::max() ||
                               regions.policyAt(static_cast<std::int32_t>(px), static_cast<std::int32_t>(pz)) != policy) continue;
                    for (std::int64_t dy = -vertical; dy <= vertical; ++dy) {
                        if (cancelled != nullptr && cancelled->load()) return {};
                        const std::optional<std::int32_t> y = addChunkOffset(*centerY, dy);
                        if (!y.has_value()) continue;
                        const StreamingChunkKey key{policy, {cx, *y, cz}};
                        const Candidate candidate{key, squaredDistance(key, camera, config)};
                        // Retain only the nearest resident-capacity candidates,
                        // rather than allocating the entire requested volume.
                        if (nearest.size() == config.chunkCapacityPerPolicy) {
                            if (!nearer(candidate, nearest.front())) continue;
                            std::pop_heap(nearest.begin(), nearest.end(), nearer);
                            nearest.pop_back();
                        }
                        nearest.push_back(candidate);
                        std::push_heap(nearest.begin(), nearest.end(), nearer);
                    }
                }
            }
            output.insert(output.end(), nearest.begin(), nearest.end());
        }
        std::sort(output.begin(), output.end(), nearer);
        output.erase(std::unique(output.begin(), output.end(), [](const Candidate& a,
                                                                 const Candidate& b) {
            return a.key == b.key;
        }), output.end());
        return output;
    }

    auto refreshDesired(math::Vec3 camera) -> bool {
        if (config_.regionalSurfaceOwnership && (!planValid_ || planRevision_ != regions_.revision())) {
            // Region changes can alter triangles without changing resident keys.
            // Bump mesh versions before accepting any in-flight completion.
            for (const auto& [key, entry] : entries_) {
                storeFor(key.policy).markDirty(entry.handle, {false, true, true});
            }
        }
        std::array<std::optional<std::int32_t>, 9> centers;
        constexpr PolicyKind policies[]{PolicyKind::Voxel, PolicyKind::Height, PolicyKind::Hex};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto center = cameraCenter(policies[i], camera, config_);
            if (!center) continue;
            centers[i * 3] = center->x;
            centers[i * 3 + 1] = center->y;
            centers[i * 3 + 2] = center->z;
        }
        const std::array<float, 3> exactCamera{camera.x, camera.y, camera.z};
        if (!planValid_ || centers != planCenters_ || planRevision_ != regions_.revision() ||
            (config_.horizontalHalfExtentMeters && exactCamera != planCamera_)) {
            if (planning_) planning_->cancelled.store(true);
            planning_.reset();
            planCenters_ = centers;
            planCamera_ = exactCamera;
            planRevision_ = regions_.revision();
            planValid_ = true;
            long double count = 0.L;
            for (const auto policy : policies) {
                if (const auto bounds = horizontalBounds(policy, camera, config_)) {
                    const long double vertical = policy == PolicyKind::Voxel ? 2.0L * config_.voxelVerticalRadius + 1.L : 1.L;
                    count += static_cast<long double>(bounds->maxX - bounds->minX + 1) *
                             static_cast<long double>(bounds->maxZ - bounds->minZ + 1) * vertical;
                }
            }
            const bool finite = std::isfinite(camera.x) && std::isfinite(camera.y) &&
                                std::isfinite(camera.z);
            // Fixed work-unit admission plans on this thread. An async plan can
            // finish before or after this update checks it, which changes tick
            // counts. This is not a realtime deadline.
            if (count <= 256.0L || !finite || config_.deterministicAdmission) {
                desired_ = desiredChunks(config_, regions_, camera);
            } else {
                auto work = std::make_shared<PlanningWork>(config_, regions_, camera);
                planning_ = work;
                if (!jobs_.submit({0, 0}, [work] {
                    try {
                        auto result = desiredChunks(work->config, work->regions, work->camera,
                                                    &work->cancelled);
                        std::lock_guard lock(work->mutex);
                        work->result = std::move(result);
                    } catch (...) {
                        std::lock_guard lock(work->mutex);
                        work->failed = true;
                    }
                })) {
                    planning_.reset();
                    planValid_ = false;
                    return false;
                }
            }
        }
        if (planning_) {
            const auto work = planning_;
            std::lock_guard lock(work->mutex);
            if (work->failed) {
                ++stats_.failedCompletions;
                planning_.reset();
                planValid_ = false;
                return false;
            }
            if (!work->result.has_value()) return false;
            desired_ = std::move(*work->result);
            planning_.reset();
        }
        // Legacy rings replan at chunk boundaries; meter footprints replan on
        // any camera movement, including changes to nearest-capacity selection.
        for (Candidate& candidate : desired_) {
            candidate.distanceSquared = squaredDistance(candidate.key, camera, config_);
        }
        std::sort(desired_.begin(), desired_.end(), nearer);
        return true;
    }

    void post(Completion completion) {
        std::lock_guard lock(completionMutex_);
        completed_.push_back(std::move(completion));
    }

    auto submitGeneration(StreamingChunkKey key, Entry& entry,
                          const ChunkGenerationRequest& request) -> bool {
        const jobs::JobPriority priority{entry.distanceSquared, kCellStage};
        const ChunkHandle handle = entry.handle;
        const bool submitted = jobs_.submit(priority, [this, key, handle, priority, request] {
            Completion completion;
            completion.kind = CompletionKind::Cells;
            completion.key = key;
            completion.handle = handle;
            completion.priority = priority;
            completion.sourceVersion = request.sourceVersion;
            completion.generation = request;
            try {
                completion.base = generateChunkBase(request);
            } catch (...) {
                completion.base = std::nullopt;
            }
            post(std::move(completion));
        });
        if (submitted) {
            entry.generationInFlight = true;
            ++stats_.submittedCellJobs;
        }
        return submitted;
    }

    [[nodiscard]] auto neighborSnapshots(StreamingChunkKey key)
        -> std::vector<ChunkSnapshot> {
        std::vector<ChunkSnapshot> output;
        const std::int32_t minY = key.policy == PolicyKind::Voxel ? -1 : 0;
        const std::int32_t maxY = key.policy == PolicyKind::Voxel ? 1 : 0;
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            for (std::int32_t dy = minY; dy <= maxY; ++dy) {
                for (std::int32_t dz = -1; dz <= 1; ++dz) {
                    const auto x = addChunkOffset(key.coord.x, dx);
                    const auto y = addChunkOffset(key.coord.y, dy);
                    const auto z = addChunkOffset(key.coord.z, dz);
                    if (!x || !y || !z) continue;
                    ++stats_.neighborQueries;
                    const auto found = entries_.find({key.policy, {*x, *y, *z}});
                    if (found == entries_.end()) continue;
                    auto copy = storeFor(key.policy).snapshot(found->second.handle);
                    if (copy) output.push_back(std::move(*copy));
                }
            }
        }
        // x/y/z loop order is the same key order used by meshing before this
        // bounded lookup path, including overlapping height-border precedence.
        return output;
    }

    void submitMesh(StreamingChunkKey key, Entry& entry, ChunkSnapshot snapshot) {
        auto work = std::make_shared<MeshWork>();
        work->key = key;
        work->handle = entry.handle;
        work->distanceSquared = entry.distanceSquared;
        work->center = std::move(snapshot);
        work->neighborhood = neighborSnapshots(key);
        work->blocks = blocks_;
        work->heightSkirtDepth = config_.heightSkirtDepth;
        work->heightTransitionFloor = config_.heightTransitionFloor;
        const bool cullHeightSkirts = config_.cullSharedHeightSkirts;
        work->hexRadius = config_.hexRadius;
        work->hexFloorHeight = config_.hexFloorHeight;
        if (config_.regionalSurfaceOwnership) work->regions = regions_;
        work->origin = terrainOrigin(key, config_);
        if (work->regions && key.policy == PolicyKind::Hex) work->origin = {};
        work->hexCutWalls = config_.regionalHexCutWalls;
        work->voxelCutFaces = config_.regionalVoxelCutFaces;
        const jobs::JobPriority priority{entry.distanceSquared, kMeshStage};
        const bool submitted = jobs_.submit(priority, [this, work, priority, cullHeightSkirts] {
            TerrainMeshingContext context;
            context.blocks = &work->blocks;
            context.voxel.borderFetch = [work](std::int32_t x, std::int32_t y,
                                                std::int32_t z) -> std::uint16_t {
                // Voxel border requests are local; offset their chunk quotient
                // from the meshed center, not from the world origin.
                const auto chunkX = addChunkOffset(work->key.coord.x, floorDiv(x, static_cast<std::int32_t>(kVoxelEdge)));
                const auto chunkY = addChunkOffset(work->key.coord.y, floorDiv(y, static_cast<std::int32_t>(kVoxelEdge)));
                const auto chunkZ = addChunkOffset(work->key.coord.z, floorDiv(z, static_cast<std::int32_t>(kVoxelEdge)));
                if (!chunkX || !chunkY || !chunkZ) return 0;
                const ChunkCoord coord{*chunkX, *chunkY, *chunkZ};
                for (const ChunkSnapshot& neighbor : work->neighborhood) {
                    if (neighbor.coord != coord) continue;
                    const std::uint32_t localX = static_cast<std::uint32_t>(
                        floorMod(x, static_cast<std::int32_t>(kVoxelEdge)));
                    const std::uint32_t localY = static_cast<std::uint32_t>(
                        floorMod(y, static_cast<std::int32_t>(kVoxelEdge)));
                    const std::uint32_t localZ = static_cast<std::uint32_t>(
                        floorMod(z, static_cast<std::int32_t>(kVoxelEdge)));
                    return snapshotVoxel(neighbor, voxelIndex(localX, localY, localZ)).value_or(0);
                }
                return 0;
            };
            if (work->voxelCutFaces && work->key.policy == PolicyKind::Voxel) {
                context.voxel.cellOwned = [work](std::int32_t x, std::int32_t, std::int32_t z) {
                    const std::int64_t wx = std::int64_t(work->key.coord.x) * kVoxelEdge + x;
                    const std::int64_t wz = std::int64_t(work->key.coord.z) * kVoxelEdge + z;
                    return wx >= std::numeric_limits<std::int32_t>::min() && wx <= std::numeric_limits<std::int32_t>::max() &&
                        wz >= std::numeric_limits<std::int32_t>::min() && wz <= std::numeric_limits<std::int32_t>::max() &&
                        work->regions->policyAt(static_cast<std::int32_t>(wx), static_cast<std::int32_t>(wz)) == PolicyKind::Voxel;
                };
            }
            context.height.skirtDepth = work->heightSkirtDepth;
            context.height.transitionFloor = work->heightTransitionFloor;
            if (cullHeightSkirts && work->center.policy == PolicyKind::Height) {
                context.height.skirtEdges = heightSkirtEdges(work->center, work->neighborhood);
            }
            context.height.borderFetch = [work](std::int64_t x, std::int64_t z) -> float {
                for (const ChunkSnapshot& neighbor : work->neighborhood) {
                    const std::int64_t originX =
                        static_cast<std::int64_t>(neighbor.coord.x) * kHeightQuads;
                    const std::int64_t originZ =
                        static_cast<std::int64_t>(neighbor.coord.z) * kHeightQuads;
                    if (x < originX || x > originX + kHeightQuads ||
                        z < originZ || z > originZ + kHeightQuads) {
                        continue;
                    }
                    const auto localX = static_cast<std::uint32_t>(x - originX);
                    const auto localZ = static_cast<std::uint32_t>(z - originZ);
                    const auto sample = snapshotHeight(
                        neighbor, heightSampleIndex(localX, localZ));
                    if (sample.has_value()) return sample->height;
                }
                const std::int64_t centerX =
                    static_cast<std::int64_t>(work->center.coord.x) * kHeightQuads;
                const std::int64_t centerZ =
                    static_cast<std::int64_t>(work->center.coord.z) * kHeightQuads;
                const auto localX = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    x - centerX, 0, kHeightQuads));
                const auto localZ = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    z - centerZ, 0, kHeightQuads));
                return snapshotHeight(work->center, heightSampleIndex(localX, localZ))
                    .value_or(HeightSample{}).height;
            };
            context.hex.radius = work->hexRadius;
            context.hex.floorHeight = work->hexFloorHeight;
            context.hex.worldSpace = work->regions.has_value();
            context.hex.hasNeighbor = [work](AxialCoord coordinate) {
                for (const ChunkSnapshot& neighbor : work->neighborhood) {
                    if (snapshotHexPresent(neighbor, coordinate)) return true;
                }
                return false;
            };

            Completion completion;
            completion.kind = CompletionKind::Mesh;
            completion.key = work->key;
            completion.handle = work->handle;
            completion.priority = priority;
            completion.sourceVersion = work->center.version;
            if (work->regions) completion.regionRevision = work->regions->revision();
            try {
                auto result = meshChunkSnapshot(work->center, context);
                if (result.hasValue()) {
                    if (work->regions && !(work->voxelCutFaces && work->key.policy == PolicyKind::Voxel)) {
                        const auto floor = work->hexCutWalls && work->key.policy == PolicyKind::Hex
                            ? std::optional<float>(work->hexFloorHeight) : std::nullopt;
                        auto clipped = clipTerrainRegions(result.value().mesh, work->origin, *work->regions, work->key.policy, floor);
                        if (clipped) {
                            result.value().mesh = std::move(clipped).value();
                            completion.mesh = std::move(result).value();
                        }
                    } else completion.mesh = std::move(result).value();
                }
            } catch (...) {
                completion.mesh = std::nullopt;
            }
            post(std::move(completion));
        });
        if (submitted) {
            entry.meshInFlight = true;
            ++stats_.submittedMeshJobs;
        }
    }

    void dirtyNeighbors(StreamingChunkKey key) {
        const std::int32_t minY = key.policy == PolicyKind::Voxel ? -1 : 0;
        const std::int32_t maxY = key.policy == PolicyKind::Voxel ? 1 : 0;
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dy = minY; dy <= maxY; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    if (key.policy == PolicyKind::Hex &&
                        std::find(kHexDirections.begin(), kHexDirections.end(),
                                  AxialCoord{dx, dz}) == kHexDirections.end()) {
                        continue;
                    }
                    const std::optional<std::int32_t> x = addChunkOffset(key.coord.x, dx);
                    const std::optional<std::int32_t> y = addChunkOffset(key.coord.y, dy);
                    const std::optional<std::int32_t> z = addChunkOffset(key.coord.z, dz);
                    if (!x.has_value() || !y.has_value() || !z.has_value()) continue;
                    const StreamingChunkKey neighborKey{key.policy, {*x, *y, *z}};
                    const auto found = entries_.find(neighborKey);
                    if (found != entries_.end()) {
                        storeFor(key.policy).markDirty(found->second.handle, {false, true, true});
                    }
                }
            }
        }
    }

    void persist(const StreamingChunkKey& key, const containers::Array<ChunkDelta>& deltas) {
        if (deltas.isEmpty()) {
            persistence_.erase(key);
            return;
        }
        auto& values = persistence_[key];
        values.clear();
        for (const ChunkDelta& delta : deltas) values[delta.index] = delta.value;
    }

    auto applyPersistence(StreamingChunkKey key, ChunkStore& store, ChunkHandle handle) -> bool {
        const auto found = persistence_.find(key);
        if (found == persistence_.end()) return true;
        for (const auto& [index, value] : found->second) {
            if (!store.edit(handle, index, value)) return false;
        }
        return true;
    }

    void collectCompletions() {
        std::vector<Completion> incoming;
        {
            std::lock_guard lock(completionMutex_);
            incoming.swap(completed_);
        }
        for (Completion& completion : incoming) {
            pendingCompletions_.push_back(std::move(completion));
        }
        std::sort(pendingCompletions_.begin(), pendingCompletions_.end(),
                  [](const Completion& a, const Completion& b) {
            if (a.priority.distanceSquared != b.priority.distanceSquared) {
                return a.priority.distanceSquared < b.priority.distanceSquared;
            }
            if (a.priority.stage != b.priority.stage) {
                return a.priority.stage < b.priority.stage;
            }
            return keyLess(a.key, b.key);
        });
    }

    auto processCompletion(Completion completion) -> bool {
        auto found = entries_.find(completion.key);
        if (found == entries_.end() || found->second.handle != completion.handle) {
            ++stats_.staleCompletions;
            return false;
        }
        Entry& entry = found->second;
        ChunkStore& store = storeFor(completion.key.policy);
        if (completion.kind == CompletionKind::Cells) {
            entry.generationInFlight = false;
            if (!completion.base.has_value()) {
                ++stats_.failedCompletions;
                dirtyNeighbors(completion.key);
                store.destroy(entry.handle);
                entries_.erase(found);
                return false;
            }
            if (!store.publishGenerated(entry.handle, completion.generation,
                                        std::move(*completion.base))) {
                ++stats_.staleCompletions;
                return false;
            }
            if (!applyPersistence(completion.key, store, entry.handle)) {
                ++stats_.failedCompletions;
            }
            dirtyNeighbors(completion.key);
            ++stats_.publishedCells;
            return true;
        }

        entry.meshInFlight = false;
        if (completion.regionRevision && *completion.regionRevision != regions_.revision()) {
            ++stats_.staleCompletions;
            return false;
        }
        if (!completion.mesh.has_value()) {
            if (store.version(entry.handle) != completion.sourceVersion) {
                ++stats_.staleCompletions;
                return false;
            }
            entry.failedMeshVersion = completion.sourceVersion;
            ++stats_.failedCompletions;
            return false;
        }
        if (!commitTerrainMesh(store, entry.handle, *completion.mesh)) {
            ++stats_.staleCompletions;
            return false;
        }
        entry.failedMeshVersion.reset();
        entry.meshVersion = completion.mesh->sourceVersion;
        entry.meshRegionRevision = completion.regionRevision;
        entry.mesh = std::move(completion.mesh->mesh);
        ++stats_.publishedMeshes;
        return true;
    }

    void recordHeightPersistence(StreamingChunkKey key, std::uint32_t index,
                                 const CellValue& value) {
        const std::uint32_t x = index % kHeightSamples;
        const std::uint32_t z = index / kHeightSamples;
        const auto record = [this, &key, &value](std::int32_t dx, std::int32_t dz,
                                                 std::uint32_t mirroredIndex) {
            const std::optional<std::int32_t> neighborX = addChunkOffset(key.coord.x, dx);
            const std::optional<std::int32_t> neighborZ = addChunkOffset(key.coord.z, dz);
            if (neighborX.has_value() && neighborZ.has_value()) {
                persistence_[{PolicyKind::Height, {*neighborX, 0, *neighborZ}}][mirroredIndex] =
                    value;
            }
        };
        if (x == 0) record(-1, 0, heightSampleIndex(kHeightQuads, z));
        if (x == kHeightQuads) record(1, 0, heightSampleIndex(0, z));
        if (z == 0) record(0, -1, heightSampleIndex(x, kHeightQuads));
        if (z == kHeightQuads) record(0, 1, heightSampleIndex(x, 0));
        if (x == 0 && z == 0) {
            record(-1, -1, heightSampleIndex(kHeightQuads, kHeightQuads));
        } else if (x == 0 && z == kHeightQuads) {
            record(-1, 1, heightSampleIndex(kHeightQuads, 0));
        } else if (x == kHeightQuads && z == 0) {
            record(1, -1, heightSampleIndex(0, kHeightQuads));
        } else if (x == kHeightQuads && z == kHeightQuads) {
            record(1, 1, heightSampleIndex(0, 0));
        }
    }

    auto update(math::Vec3 camera, StreamingDeadline deadline) -> StreamingFrameStats {
        const bool hard = deadline.hard && !config_.deterministicAdmission;
        const auto sample = [&]() -> std::chrono::steady_clock::time_point {
            if (hard && deadline.now != nullptr) return deadline.now(deadline.clock);
            return std::chrono::steady_clock::now();
        };
        const auto start = sample();
        StreamingFrameStats frame;
        if (config_.frameBudget.count() <= 0) {
            frame.budgetClosed = true;
            return frame;
        }
        auto stageStart = start;
        const auto past = [&] { return sample() - start >= config_.frameBudget; };
        const auto finishStage = [&](std::chrono::nanoseconds& duration) {
            if (!config_.profileTimings) return;
            const auto now = std::chrono::steady_clock::now();
            duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - stageStart);
            stageStart = now;
        };
        const bool ready = refreshDesired(camera);
        finishStage(frame.timings.planning);
        frame.planningPending = !ready;
        const auto& desired = desired_;
        std::unordered_map<StreamingChunkKey, std::uint64_t> desiredDistances;
        desiredDistances.reserve(desired.size());
        for (const Candidate& candidate : desired) {
            desiredDistances.emplace(candidate.key, candidate.distanceSquared);
            const auto found = entries_.find(candidate.key);
            if (found != entries_.end()) found->second.distanceSquared = candidate.distanceSquared;
        }

        std::vector<Candidate> unloads;
        for (const auto& [key, entry] : entries_) {
            if (desiredDistances.find(key) == desiredDistances.end()) {
                unloads.push_back({key, entry.distanceSquared});
            }
        }
        std::sort(unloads.begin(), unloads.end(), [](const Candidate& a, const Candidate& b) {
            if (a.distanceSquared != b.distanceSquared) {
                return a.distanceSquared > b.distanceSquared;
            }
            return keyLess(b.key, a.key);
        });
        finishStage(frame.timings.residency);
        for (const Candidate& candidate : unloads) {
            if (!ready || frame.unloaded >= config_.maxUnloadsPerFrame) break;
            if (hard) {
                if (past()) {
                    frame.budgetDeferred = true;
                    break;
                }
            } else if (frame.unloaded != 0 && !withinBudget(start)) {
                frame.budgetDeferred = true;
                break;
            }
            const auto found = entries_.find(candidate.key);
            if (found == entries_.end()) continue;
            ChunkStore& store = storeFor(candidate.key.policy);
            const auto copy = store.snapshotDeltas(found->second.handle);
            if (copy.has_value()) persist(candidate.key, *copy);
            dirtyNeighbors(candidate.key);
            store.destroy(found->second.handle);
            entries_.erase(found);
            ++frame.unloaded;
            ++stats_.unloadedChunks;
            if (hard && past()) {
                frame.budgetOverrun = true;
                frame.budgetDeferred = true;
                break;
            }
        }
        finishStage(frame.timings.unload);

        for (const Candidate& candidate : desired) {
            if (!ready || frame.loaded >= config_.maxLoadsPerFrame) break;
            if (hard) {
                if (past()) {
                    frame.budgetDeferred = true;
                    break;
                }
            } else if (frame.loaded != 0 && !withinBudget(start)) {
                frame.budgetDeferred = true;
                break;
            }
            if (entries_.find(candidate.key) != entries_.end()) continue;
            ChunkStore& store = storeFor(candidate.key.policy);
            if (store.liveCount() >= config_.chunkCapacityPerPolicy) continue;
            const ChunkHandle handle = store.create(
                candidate.key.coord, config_.worldSeed, candidate.key.policy, config_.policyParams);
            if (!store.queue(handle)) continue;
            const std::optional<ChunkGenerationRequest> request = store.beginGeneration(handle);
            if (!request.has_value()) {
                store.destroy(handle);
                continue;
            }
            const auto [found, inserted] = entries_.emplace(
                candidate.key,
                Entry{handle, candidate.distanceSquared, false, false, std::nullopt,
                      std::nullopt});
            if (!inserted || !submitGeneration(candidate.key, found->second, *request)) {
                entries_.erase(candidate.key);
                store.destroy(handle);
                continue;
            }
            ++frame.loaded;
            if (hard && past()) {
                frame.budgetOverrun = true;
                frame.budgetDeferred = true;
                break;
            }
        }
        finishStage(frame.timings.load);

        const auto submitReadyMeshes = [&] {
            std::vector<Candidate> meshCandidates;
            for (const auto& [key, entry] : entries_) {
                const ChunkStore& store = storeFor(key.policy);
                if (entry.meshInFlight || entry.generationInFlight ||
                    store.state(entry.handle) != ChunkState::CellsReady) {
                    continue;
                }
                const DirtyFlags dirty = store.dirty(entry.handle);
                if (!dirty.meshDirty && !dirty.bordersDirty) continue;
                if (entry.failedMeshVersion.has_value() &&
                    *entry.failedMeshVersion == store.version(entry.handle)) {
                    continue;
                }
                meshCandidates.push_back({key, entry.distanceSquared});
            }
            std::sort(meshCandidates.begin(), meshCandidates.end(),
                      [](const Candidate& a, const Candidate& b) {
                if (a.distanceSquared != b.distanceSquared) {
                    return a.distanceSquared < b.distanceSquared;
                }
                return keyLess(a.key, b.key);
            });
            for (const Candidate& candidate : meshCandidates) {
                if (frame.meshJobs >= config_.maxMeshJobsPerFrame) break;
                if (hard) {
                    if (past()) {
                        frame.budgetDeferred = true;
                        break;
                    }
                } else if (frame.meshJobs != 0 && !withinBudget(start)) {
                    frame.budgetDeferred = true;
                    break;
                }
                const auto found = entries_.find(candidate.key);
                if (found == entries_.end()) continue;
                std::optional<ChunkSnapshot> copy =
                    storeFor(candidate.key.policy).snapshot(found->second.handle);
                if (!copy.has_value()) continue;
                submitMesh(candidate.key, found->second, std::move(*copy));
                if (found->second.meshInFlight) ++frame.meshJobs;
                if (hard && past()) {
                    frame.budgetOverrun = true;
                    frame.budgetDeferred = true;
                    break;
                }
            }
        };
        const auto publishReady = [&] {
            collectCompletions();
            while (!pendingCompletions_.empty() &&
                   frame.completionsProcessed < config_.maxPublicationsPerFrame) {
                if (hard) {
                    if (past()) {
                        frame.budgetDeferred = true;
                        break;
                    }
                } else if (frame.completionsProcessed != 0 && !withinBudget(start)) {
                    break;
                }
                Completion completion = std::move(pendingCompletions_.front());
                pendingCompletions_.erase(pendingCompletions_.begin());
                if (processCompletion(std::move(completion))) ++frame.published;
                ++frame.completionsProcessed;
                if (hard && past()) {
                    frame.budgetOverrun = true;
                    frame.budgetDeferred = true;
                    break;
                }
            }
            if (!hard && !pendingCompletions_.empty() && frame.completionsProcessed > 0 &&
                frame.completionsProcessed < config_.maxPublicationsPerFrame && !withinBudget(start)) {
                frame.budgetDeferred = true;
            }
        };
        // The gate path finishes jobs already in flight, including this tick's
        // loads, before it chooses a publication prefix. Count caps still stop
        // the tick. Interactive pacing does not wait here.
        if (config_.deterministicAdmission) jobs_.waitIdle();
        publishReady();
        finishStage(frame.timings.publication);
        submitReadyMeshes();
        if (config_.deterministicAdmission) {
            for (unsigned pass = 0; pass < 8; ++pass) {
                const auto meshesBefore = frame.meshJobs;
                jobs_.waitIdle();
                publishReady();
                if (!pendingCompletions_.empty()) break;
                submitReadyMeshes();
                if (frame.meshJobs == meshesBefore) break;
            }
        }
        finishStage(frame.timings.mesh);

        frame.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(sample() - start);
        return frame;
    }

    [[nodiscard]] auto info(StreamingChunkKey key, const Entry& entry) const
        -> StreamingChunkInfo {
        const ChunkStore& store = storeFor(key.policy);
        return {key, entry.handle, store.state(entry.handle), store.dirty(entry.handle),
                store.version(entry.handle), entry.distanceSquared,
                entry.generationInFlight, entry.meshInFlight, entry.mesh.has_value(), entry.meshVersion, entry.meshRegionRevision};
    }

    auto boundaryKeys(const StreamingBoundaryRequest& request) const -> std::vector<StreamingChunkKey> {
        std::vector<StreamingChunkKey> keys;
        for (const auto& [key, entry] : entries_) {
            (void)entry;
            if (key.policy != request.negative && key.policy != request.positive) continue;
            const auto f = chunkFootprint(key, config_);
            const bool x = request.line.axis == TerrainBoundaryAxis::X;
            const double low = x ? f.minX : f.minZ, high = x ? f.maxX : f.maxZ;
            const double begin = x ? f.minZ : f.minX, end = x ? f.maxZ : f.maxX;
            const bool positive = key.policy == request.positive;
            if (begin >= request.line.end || end <= request.line.begin) continue;
            if (positive ? low <= request.line.plane && high > request.line.plane
                         : low < request.line.plane && high >= request.line.plane) keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end(), keyLess);
        return keys;
    }

    [[nodiscard]] auto classifyPublishedSolid(double x, double y, double z) const
        -> Result<TerrainSolidSample, StreamingSolidError>;

    auto boundarySourceReady(StreamingChunkKey key, const Entry& entry) const -> bool {
        const auto& store = storeFor(key.policy);
        return entry.mesh && !entry.generationInFlight && !entry.meshInFlight &&
            store.state(entry.handle) == ChunkState::Meshed && store.version(entry.handle) == entry.meshVersion &&
            entry.meshRegionRevision == regions_.revision();
    }

    WorldStreamingConfig config_;
    std::shared_ptr<const std::byte> boundaryOwner_ = std::make_shared<const std::byte>();
    std::vector<Candidate> desired_;
    std::array<std::optional<std::int32_t>, 9> planCenters_;
    std::array<float, 3> planCamera_{};
    std::uint64_t planRevision_ = 0;
    bool planValid_ = false;
    std::shared_ptr<PlanningWork> planning_;
    RegionMap regions_;
    BlockRegistry blocks_;
    ChunkStore voxelStore_;
    ChunkStore heightStore_;
    ChunkStore hexStore_;
    std::unordered_map<StreamingChunkKey, Entry> entries_;
    std::optional<StreamingSurfaceView> acceptedSurfaceView_;
    std::map<StreamingChunkKey, std::map<std::uint32_t, CellValue>, StreamingKeyLess>
        persistence_;
    StreamingStats stats_;
    mutable std::mutex completionMutex_;
    std::vector<Completion> completed_;
    std::vector<Completion> pendingCompletions_;
    jobs::JobSystem jobs_;
};

WorldStreamer::WorldStreamer(const WorldStreamingConfig& config) {
    if (config.regionalVoxelCutFaces && !config.regionalSurfaceOwnership) {
        throw std::invalid_argument("Regional voxel cut faces require regional surface ownership");
    }
    if (config.regionalHexCutWalls && !config.regionalSurfaceOwnership) {
        throw std::invalid_argument("Regional hex cut walls require regional surface ownership");
    }
    if (config.horizontalHalfExtentMeters &&
        (!std::isfinite(*config.horizontalHalfExtentMeters) || *config.horizontalHalfExtentMeters < 0.f)) {
        throw std::invalid_argument("Streaming horizontalHalfExtentMeters must be finite and nonnegative");
    }
    if (config.regionalSurfaceOwnership) {
        if (config.regionExtentMeters <= 0 || !std::isfinite(config.hexRadius) || config.hexRadius <= 0.f ||
            !hexChunkOrigin({}, config.policyParams.hex.radius, config.policyParams.hex.layout)) {
            throw std::invalid_argument("Regional surfaces require valid region extent and hex layout/scale");
        }
        // A translated footprint can touch one more tile per axis than the
        // origin fixture. Reserve that worst-case margin before starting workers.
        for (const auto policy : {PolicyKind::Height, PolicyKind::Voxel, PolicyKind::Hex}) {
            const auto bounds = chunkFootprint({policy, {}}, config);
            const double nx = std::ceil((bounds.maxX - bounds.minX) / config.regionExtentMeters) + 1;
            const double nz = std::ceil((bounds.maxZ - bounds.minZ) / config.regionExtentMeters) + 1;
            if (!std::isfinite(nx * nz) || nx * nz > 4096) {
                throw std::invalid_argument("Regional surfaces exceed the 4096-tile per-chunk work limit");
            }
        }
    }
    impl_ = std::make_unique<Impl>(config);
}

WorldStreamer::~WorldStreamer() = default;

auto WorldStreamer::regions() -> RegionMap& { return impl_->regions_; }
auto WorldStreamer::regions() const -> const RegionMap& { return impl_->regions_; }
auto WorldStreamer::identityToken() const -> std::shared_ptr<const std::byte> { return impl_->boundaryOwner_; }

auto WorldStreamer::blocks() -> BlockRegistry& { return impl_->blocks_; }

auto WorldStreamer::update(math::Vec3 cameraPosition, StreamingDeadline deadline) -> StreamingFrameStats {
    return impl_->update(cameraPosition, deadline);
}

auto WorldStreamer::deterministicAdmission() const -> bool {
    return impl_->config_.deterministicAdmission;
}

void WorldStreamer::waitForIdle() { impl_->jobs_.waitIdle(); }

auto WorldStreamer::hasPendingWork() const -> bool {
    if (impl_->planning_ || !impl_->planValid_) return true;
    if (impl_->jobs_.pendingCount() != 0 || !impl_->pendingCompletions_.empty()) return true;
    {
        std::lock_guard lock(impl_->completionMutex_);
        if (!impl_->completed_.empty()) return true;
    }
    for (const auto& [key, entry] : impl_->entries_) {
        if (entry.generationInFlight || entry.meshInFlight) return true;
        const ChunkStore& store = impl_->storeFor(key.policy);
        const ChunkState state = store.state(entry.handle);
        if (state == ChunkState::Queued || state == ChunkState::LayoutDone) return true;
        if (state == ChunkState::CellsReady) {
            const DirtyFlags dirty = store.dirty(entry.handle);
            const bool failedAtCurrentVersion = entry.failedMeshVersion.has_value() &&
                *entry.failedMeshVersion == store.version(entry.handle);
            if ((dirty.meshDirty || dirty.bordersDirty) && !failedAtCurrentVersion) return true;
        }
    }
    return false;
}

auto WorldStreamer::residentCount() const -> std::size_t { return impl_->entries_.size(); }

auto WorldStreamer::residentChunks() const -> containers::Array<StreamingChunkInfo> {
    containers::Array<StreamingChunkInfo> output;
    output.reserve(impl_->entries_.size());
    for (const auto& [key, entry] : impl_->entries_) output.push(impl_->info(key, entry));
    std::sort(output.begin(), output.end(), [](const StreamingChunkInfo& a,
                                               const StreamingChunkInfo& b) {
        if (a.distanceSquared != b.distanceSquared) return a.distanceSquared < b.distanceSquared;
        return keyLess(a.key, b.key);
    });
    return output;
}

auto WorldStreamer::residentTerrainFootprint() const -> std::optional<TerrainFootprint> {
    TerrainFootprint result{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& [key, entry] : impl_->entries_) {
        const auto footprint = chunkFootprint(key, impl_->config_);
        if (!std::isfinite(footprint.minX) || !std::isfinite(footprint.maxX) ||
            !std::isfinite(footprint.minZ) || !std::isfinite(footprint.maxZ) ||
            footprint.minX >= footprint.maxX || footprint.minZ >= footprint.maxZ) continue;
        result.minX = std::min(result.minX, footprint.minX); result.maxX = std::max(result.maxX, footprint.maxX);
        result.minZ = std::min(result.minZ, footprint.minZ); result.maxZ = std::max(result.maxZ, footprint.maxZ);
    }
    return std::isfinite(result.minX) ? std::optional<TerrainFootprint>(result) : std::nullopt;
}

auto WorldStreamer::residentTerrainFootprint(PolicyKind first, PolicyKind second) const
    -> std::optional<TerrainFootprint> {
    TerrainFootprint result{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& [key, entry] : impl_->entries_) {
        if (key.policy != first && key.policy != second) continue;
        if (!entry.mesh || entry.mesh->indices.isEmpty()) continue;
        const auto* mesh = &*entry.mesh;
        const auto origin = meshOrigin(key);
        for (const auto index : mesh->indices) {
            if (index >= mesh->vertices.size()) continue;
            const auto& position = mesh->vertices[index].position;
            const double x = double(position.x) + origin.x, z = double(position.z) + origin.z;
            if (!std::isfinite(x) || !std::isfinite(z)) continue;
            result.minX = std::min(result.minX, x); result.maxX = std::max(result.maxX, x);
            result.minZ = std::min(result.minZ, z); result.maxZ = std::max(result.maxZ, z);
        }
    }
    return std::isfinite(result.minX) && result.minX < result.maxX && result.minZ < result.maxZ
        ? std::optional<TerrainFootprint>(result) : std::nullopt;
}

auto WorldStreamer::publishSurfaceView(const StreamingSurfaceView& view) -> bool {
    if (!view.current(*this)) return false;
    impl_->acceptedSurfaceView_ = view;
    return true;
}

void WorldStreamer::clearSurfaceView() {
    impl_->acceptedSurfaceView_.reset();
}

auto WorldStreamer::acceptedSurfaceView() const -> const StreamingSurfaceView* {
    return impl_->acceptedSurfaceView_ ? &*impl_->acceptedSurfaceView_ : nullptr;
}

auto WorldStreamer::raycastAcceptedTerrain(math::Vec3 origin, math::Vec3 direction,
                                           float maxDistance, bool cullBackFaces) const
    -> std::optional<StreamingBoundaryHit> {
    return impl_->acceptedSurfaceView_
        ? impl_->acceptedSurfaceView_->raycast(origin, direction, maxDistance, cullBackFaces)
        : std::optional<StreamingBoundaryHit>{};
}

auto WorldStreamer::chunk(StreamingChunkKey key) const
    -> std::optional<StreamingChunkInfo> {
    const auto found = impl_->entries_.find(key);
    return found != impl_->entries_.end()
        ? std::optional<StreamingChunkInfo>(impl_->info(key, found->second))
        : std::nullopt;
}

auto WorldStreamer::mesh(StreamingChunkKey key) const -> const MeshData* {
    const auto found = impl_->entries_.find(key);
    return found != impl_->entries_.end() && found->second.mesh.has_value()
        ? &*found->second.mesh
        : nullptr;
}

auto WorldStreamer::meshOrigin(StreamingChunkKey key) const -> math::Vec3 {
    if (impl_->config_.regionalSurfaceOwnership && key.policy == PolicyKind::Hex) return {};
    return terrainOrigin(key, impl_->config_);
}

auto WorldStreamer::captureBoundary(StreamingBoundaryRequest request, StreamingBoundaryLimits limits) const
    -> Result<StreamingBoundarySnapshot, StreamingBoundaryError> {
    if (!impl_->config_.regionalSurfaceOwnership) return StreamingBoundaryError::Unsupported;
    const auto& line = request.line;
    constexpr double low = std::numeric_limits<std::int32_t>::min();
    constexpr double high = double(std::numeric_limits<std::int32_t>::max()) + 1;
    const auto extent = impl_->regions_.tileExtentMeters();
    if ((request.negative != PolicyKind::Height || request.positive != PolicyKind::Hex) &&
        (request.negative != PolicyKind::Hex || request.positive != PolicyKind::Height)) return StreamingBoundaryError::InvalidBoundary;
    if ((line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !std::isfinite(line.plane) || !std::isfinite(line.begin) || !std::isfinite(line.end) ||
        line.plane <= low || line.plane >= high || line.begin < low || line.end > high ||
        line.begin >= line.end || std::fmod(line.plane, extent) != 0) return StreamingBoundaryError::InvalidBoundary;
    const auto regionAt = [extent](double coordinate) {
        auto index = static_cast<std::int64_t>(std::floor(coordinate / extent));
        if (double(index * extent) > coordinate) --index;
        return index;
    };
    const auto first = regionAt(line.begin);
    auto last = regionAt(line.end);
    if (double(last * extent) == line.end) --last;
    if (last - first + 1 > 4096) return StreamingBoundaryError::LimitExceeded;
    for (auto tile = first; tile <= last; ++tile) {
        const auto tangent = static_cast<std::int32_t>(std::max(low, double(tile * extent)));
        const auto plane = static_cast<std::int32_t>(line.plane);
        const auto policyAt = [&](std::int32_t p) {
            return line.axis == TerrainBoundaryAxis::X ? impl_->regions_.policyAt(p, tangent) : impl_->regions_.policyAt(tangent, p);
        };
        if (policyAt(plane - 1) != request.negative || policyAt(plane) != request.positive)
            return StreamingBoundaryError::InvalidBoundary;
    }
    const auto keys = impl_->boundaryKeys(request);
    if (keys.size() > limits.maxSources) return StreamingBoundaryError::LimitExceeded;
    std::size_t bytes = 0;
    for (const auto& key : keys) {
        const auto& entry = impl_->entries_.at(key);
        if (!impl_->boundarySourceReady(key, entry)) return StreamingBoundaryError::SourcesPending;
        const auto account = [&](std::size_t count, std::size_t size) {
            if (count > (limits.maxMeshBytes - bytes) / size) return false;
            bytes += count * size;
            return true;
        };
        if (!account(entry.mesh->vertices.size(), sizeof(TerrainVertex)) ||
            !account(entry.mesh->indices.size(), sizeof(std::uint32_t)) ||
            !account(entry.mesh->surfaces.size(), sizeof(TerrainSurface))) return StreamingBoundaryError::LimitExceeded;
    }
    auto data = std::make_shared<StreamingBoundarySnapshot::Data>();
    data->meshBytes = bytes;
    data->request = request; data->revision = impl_->regions_.revision(); data->owner = impl_->boundaryOwner_;
    for (const auto& key : keys) {
        const auto& entry = impl_->entries_.at(key);
        const unsigned side = key.policy == request.positive ? 1 : 0;
        data->sources[side].push_back({key, entry.handle, impl_->storeFor(key.policy).version(entry.handle), entry.meshVersion});
        data->meshes[side].push_back(*entry.mesh);
        data->origins[side].push_back(meshOrigin(key));
    }
    StreamingBoundarySnapshot snapshot;
    snapshot.data_ = std::move(data);
    return snapshot;
}

auto WorldStreamer::boundaryCurrent(const StreamingBoundarySnapshot& snapshot) const -> bool {
    if (!snapshot.data_ || snapshot.data_->owner != impl_->boundaryOwner_ ||
        snapshot.data_->revision != impl_->regions_.revision()) return false;
    const auto& data = *snapshot.data_;
    const auto keys = impl_->boundaryKeys(data.request);
    if (keys.size() != data.sources[0].size() + data.sources[1].size()) return false;
    std::array<std::size_t, 2> indices{};
    for (const auto& key : keys) {
        const unsigned side = key.policy == data.request.positive ? 1 : 0;
        if (indices[side] >= data.sources[side].size()) return false;
        const auto& source = data.sources[side][indices[side]++];
        const auto& entry = impl_->entries_.at(key);
        if (source.key != key || source.handle != entry.handle || !impl_->boundarySourceReady(key, entry) ||
            source.cellVersion != impl_->storeFor(key.policy).version(entry.handle) || source.meshVersion != entry.meshVersion) return false;
    }
    return true;
}

auto WorldStreamer::boundaryCurrent(const StreamingBoundaryReplacement& replacement) const -> bool {
    return replacement.data_ && boundaryCurrent(replacement.data_->snapshot);
}

auto WorldStreamer::boundaryCurrent(const StreamingBoundaryComposition& composition) const -> bool {
    if (!composition.data_) return false;
    for (const auto& snapshot : composition.data_->snapshots) if (!boundaryCurrent(snapshot)) return false;
    return true;
}

auto StreamingSurfaceView::current(const WorldStreamer& world) const -> bool {
    if (!data_) return false;
    if (data_->composition && !world.boundaryCurrent(*data_->composition)) return false;
    for (const auto& source : data_->sources) {
        const auto current = world.chunk(source.key);
        if (!current || !current->hasMesh || current->state != ChunkState::Meshed ||
            current->handle != source.handle || current->version != source.meshVersion ||
            current->meshVersion != source.meshVersion ||
            current->meshRegionRevision != source.regionRevision ||
            world.identityToken() != source.owner)
            return false;
    }
    return true;
}

auto WorldStreamer::snapshot(StreamingChunkKey key) const
    -> std::optional<ChunkSnapshot> {
    const auto found = impl_->entries_.find(key);
    return found != impl_->entries_.end()
        ? impl_->storeFor(key.policy).snapshot(found->second.handle)
        : std::nullopt;
}

auto WorldStreamer::stats() const -> StreamingStats { return impl_->stats_; }

auto WorldStreamer::edit(StreamingChunkKey key, std::uint32_t index,
                         const CellValue& value) -> bool {
    const auto found = impl_->entries_.find(key);
    if (found == impl_->entries_.end()) return false;
    ChunkStore& store = impl_->storeFor(key.policy);
    if (!store.edit(found->second.handle, index, value)) return false;
    impl_->persistence_[key][index] = value;
    if (key.policy == PolicyKind::Height) {
        impl_->recordHeightPersistence(key, index, value);
    }
    return true;
}

auto WorldStreamer::raycastPublishedTerrain(math::Vec3 origin, math::Vec3 direction,
                                            float maxDistance, bool cullBackFaces) const
    -> std::optional<PublishedTerrainHit> {
    std::optional<PublishedTerrainHit> closest;
    for (const auto& [key, entry] : impl_->entries_) {
        if (!entry.mesh) continue;
        const auto hit = raycastTerrainMesh(*entry.mesh, origin, direction, maxDistance,
                                           meshOrigin(key), cullBackFaces);
        if (!hit) continue;
        if (!closest || hit->distance < closest->surface.distance ||
            (hit->distance == closest->surface.distance && keyLess(key, closest->key))) {
            closest = PublishedTerrainHit{key, entry.handle, entry.meshVersion, *hit, entry.meshRegionRevision};
        }
    }
    return closest;
}

auto WorldStreamer::raycastVoxelTerrain(math::Vec3 origin, math::Vec3 direction,
                                        float maxDistance) const
    -> std::optional<VoxelRayHit> {
    return raycastVoxelWorld(
        origin, direction, maxDistance,
        [this](std::int32_t x, std::int32_t y, std::int32_t z) -> std::uint16_t {
            const ChunkCoord coord{floorDiv(x, static_cast<std::int32_t>(kVoxelEdge)),
                                   floorDiv(y, static_cast<std::int32_t>(kVoxelEdge)),
                                   floorDiv(z, static_cast<std::int32_t>(kVoxelEdge))};
            const StreamingChunkKey key{PolicyKind::Voxel, coord};
            const auto found = impl_->entries_.find(key);
            if (found == impl_->entries_.end()) return 0;
            const std::uint32_t localX = static_cast<std::uint32_t>(
                floorMod(x, static_cast<std::int32_t>(kVoxelEdge)));
            const std::uint32_t localY = static_cast<std::uint32_t>(
                floorMod(y, static_cast<std::int32_t>(kVoxelEdge)));
            const std::uint32_t localZ = static_cast<std::uint32_t>(
                floorMod(z, static_cast<std::int32_t>(kVoxelEdge)));
            const auto value = impl_->voxelStore_.cell(
                found->second.handle, voxelIndex(localX, localY, localZ));
            if (!value.has_value()) return 0;
            const BlockDesc* block = impl_->blocks_.find(value->voxel);
            return block != nullptr && block->solid ? value->voxel : 0;
        });
}

auto WorldStreamer::Impl::classifyPublishedSolid(double x, double y, double z) const
    -> Result<TerrainSolidSample, StreamingSolidError> {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return StreamingSolidError::InvalidSample;
    const double extent = regions_.tileExtentMeters();
    if (!(extent > 0)) return StreamingSolidError::InvalidSample;
    const bool onX = onRegionPlane(x, extent);
    const bool onZ = onRegionPlane(z, extent);

    const auto policyOf = [&](std::int32_t wx, std::int32_t wz) { return regions_.policyAt(wx, wz); };
    std::array<PolicyKind, 4> cornerPolicies{};
    PolicyKind interior = PolicyKind::Height;
    PolicyKind negative = PolicyKind::Height;
    PolicyKind positive = PolicyKind::Height;
    std::optional<std::int32_t> planeX;
    std::optional<std::int32_t> planeZ;
    if (onX && onZ) {
        planeX = integralPlane(x);
        planeZ = integralPlane(z);
        if (!planeX || !planeZ || *planeX == std::numeric_limits<std::int32_t>::min() ||
            *planeZ == std::numeric_limits<std::int32_t>::min()) {
            return StreamingSolidError::InvalidSample;
        }
        cornerPolicies = {policyOf(*planeX, *planeZ), policyOf(*planeX - 1, *planeZ),
                          policyOf(*planeX - 1, *planeZ - 1), policyOf(*planeX, *planeZ - 1)};
        for (const auto policy : cornerPolicies) {
            if (policy == PolicyKind::Voxel) return StreamingSolidError::UnsupportedVoxel;
        }
    } else if (onX || onZ) {
        const auto plane = integralPlane(onX ? x : z);
        const auto tangent = narrowFloor(onX ? z : x);
        if (!plane || !tangent || *plane == std::numeric_limits<std::int32_t>::min()) {
            return StreamingSolidError::InvalidSample;
        }
        if (onX) {
            planeX = plane;
            negative = policyOf(*plane - 1, *tangent);
            positive = policyOf(*plane, *tangent);
        } else {
            planeZ = plane;
            negative = policyOf(*tangent, *plane - 1);
            positive = policyOf(*tangent, *plane);
        }
        if (negative == PolicyKind::Voxel || positive == PolicyKind::Voxel) {
            return StreamingSolidError::UnsupportedVoxel;
        }
    } else {
        const auto ix = narrowFloor(x);
        const auto iz = narrowFloor(z);
        if (!ix || !iz) return StreamingSolidError::InvalidSample;
        interior = policyOf(*ix, *iz);
        if (interior == PolicyKind::Voxel) return StreamingSolidError::UnsupportedVoxel;
    }

    struct Hit { PolicyKind policy; double height, cx, cz; };
    std::vector<Hit> hits;
    for (const auto& [key, entry] : entries_) {
        if (!entry.mesh) continue;
        const auto& mesh = *entry.mesh;
        if (mesh.indices.size() % 3 != 0) return StreamingSolidError::InvalidData;
        const math::Vec3 origin = config_.regionalSurfaceOwnership && key.policy == PolicyKind::Hex
            ? math::Vec3{} : terrainOrigin(key, config_);
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
            std::array<TerrainTopSample, 3> samples;
            for (int corner = 0; corner < 3; ++corner) {
                const auto vertex = mesh.indices[index + static_cast<std::size_t>(corner)];
                if (vertex >= mesh.vertices.size()) return StreamingSolidError::InvalidData;
                const auto& position = mesh.vertices[vertex].position;
                samples[static_cast<std::size_t>(corner)] = {
                    double(position.x) + origin.x, double(position.y) + origin.y, double(position.z) + origin.z};
            }
            const auto& a = samples[0];
            const auto& b = samples[1];
            const auto& c = samples[2];
            const double area = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            if (!(area > 0)) continue;
            const auto signedArea = [](double px, double pz, const TerrainTopSample& q, const TerrainTopSample& r) {
                return (q.z - pz) * (r.x - px) - (q.x - px) * (r.z - pz);
            };
            if (signedArea(x, z, b, c) < 0 || signedArea(x, z, c, a) < 0 || signedArea(x, z, a, b) < 0) continue;
            const auto sampled = sampleUpwardTriangleHeight(a, b, c, x, z);
            if (!sampled) return mapSolidError(sampled.error());
            hits.push_back({key.policy, sampled.value(), (a.x + b.x + c.x) / 3.0, (a.z + b.z + c.z) / 3.0});
        }
    }

    const auto columnFor = [&](PolicyKind policy, double top) -> Result<TerrainSolidColumn, StreamingSolidError> {
        if (policy == PolicyKind::Hex) {
            if (!std::isfinite(config_.hexFloorHeight)) return StreamingSolidError::InvalidData;
            return TerrainSolidColumn{policy, top, double(config_.hexFloorHeight)};
        }
        return TerrainSolidColumn{policy, top, std::nullopt};
    };
    const auto uniqueTop = [&](PolicyKind policy) -> Result<double, StreamingSolidError> {
        std::optional<double> top;
        for (const auto& hit : hits) {
            if (hit.policy != policy) return StreamingSolidError::AmbiguousSurface;
            if (!top) top = hit.height;
            else if (*top != hit.height) return StreamingSolidError::AmbiguousSurface;
        }
        if (!top) return StreamingSolidError::NoPublishedSurface;
        return *top;
    };

    if (!onX && !onZ) {
        const auto top = uniqueTop(interior);
        if (!top) return top.error();
        const auto column = columnFor(interior, top.value());
        if (!column) return column.error();
        return solidResult(classifyTerrainColumn(column.value(), y));
    }

    if (onX && onZ) {
        std::array<std::optional<double>, 4> tops;
        std::optional<double> unassigned;
        for (const auto& hit : hits) {
            const bool east = hit.cx > double(*planeX);
            const bool west = hit.cx < double(*planeX);
            const bool north = hit.cz > double(*planeZ);
            const bool south = hit.cz < double(*planeZ);
            std::optional<unsigned> sector;
            if (east && north) sector = 0;
            else if (west && north) sector = 1;
            else if (west && south) sector = 2;
            else if (east && south) sector = 3;
            if (!sector) {
                if (unassigned && *unassigned != hit.height) return StreamingSolidError::AmbiguousSurface;
                unassigned = hit.height;
                continue;
            }
            if (hit.policy != cornerPolicies[*sector]) return StreamingSolidError::AmbiguousSurface;
            if (tops[*sector] && *tops[*sector] != hit.height) return StreamingSolidError::AmbiguousSurface;
            tops[*sector] = hit.height;
        }
        std::array<TerrainSolidColumn, 4> columns;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!tops[i]) return StreamingSolidError::NoPublishedSurface;
            const auto column = columnFor(cornerPolicies[i], *tops[i]);
            if (!column) return column.error();
            columns[i] = column.value();
        }
        if (unassigned) {
            const bool matched = std::any_of(tops.begin(), tops.end(), [&](const auto& top) {
                return top && *top == *unassigned;
            });
            if (!matched) return StreamingSolidError::AmbiguousSurface;
        }
        return solidResult(classifyTerrainCornerSolid(columns, y));
    }

    for (const auto& hit : hits) {
        if (hit.policy != negative && hit.policy != positive) return StreamingSolidError::AmbiguousSurface;
    }
    const double tangent = onX ? z : x;
    const auto begin = segmentBegin(tangent, extent);
    if (!begin) return StreamingSolidError::InvalidSample;
    const double end = *begin + extent;
    if (!std::isfinite(end) || !(end > *begin)) return StreamingSolidError::InvalidSample;
    const TerrainBoundaryLine line{onX ? TerrainBoundaryAxis::X : TerrainBoundaryAxis::Z, onX ? x : z, *begin, end};
    std::vector<std::pair<StreamingChunkKey, TerrainBoundaryMesh>> negativeMeshes;
    std::vector<std::pair<StreamingChunkKey, TerrainBoundaryMesh>> positiveMeshes;
    for (const auto& [key, entry] : entries_) {
        if (!entry.mesh || (key.policy != negative && key.policy != positive)) continue;
        const auto footprint = chunkFootprint(key, config_);
        if (!std::isfinite(footprint.minX) || !std::isfinite(footprint.maxX) ||
            !std::isfinite(footprint.minZ) || !std::isfinite(footprint.maxZ)) {
            continue;
        }
        const double low = onX ? footprint.minX : footprint.minZ;
        const double high = onX ? footprint.maxX : footprint.maxZ;
        const double tangentLow = onX ? footprint.minZ : footprint.minX;
        const double tangentHigh = onX ? footprint.maxZ : footprint.maxX;
        if (!(tangentLow < line.end && tangentHigh > line.begin)) continue;
        const math::Vec3 origin = config_.regionalSurfaceOwnership && key.policy == PolicyKind::Hex
            ? math::Vec3{} : terrainOrigin(key, config_);
        const TerrainBoundaryMesh view{&*entry.mesh, origin};
        if (key.policy == negative && low < line.plane && high >= line.plane) negativeMeshes.push_back({key, view});
        if (key.policy == positive && low <= line.plane && high > line.plane) positiveMeshes.push_back({key, view});
    }
    const auto byKey = [](const auto& a, const auto& b) { return keyLess(a.first, b.first); };
    std::sort(negativeMeshes.begin(), negativeMeshes.end(), byKey);
    std::sort(positiveMeshes.begin(), positiveMeshes.end(), byKey);
    std::vector<TerrainBoundaryMesh> negativeViews;
    std::vector<TerrainBoundaryMesh> positiveViews;
    negativeViews.reserve(negativeMeshes.size());
    positiveViews.reserve(positiveMeshes.size());
    for (const auto& mesh : negativeMeshes) negativeViews.push_back(mesh.second);
    for (const auto& mesh : positiveMeshes) positiveViews.push_back(mesh.second);
    const auto profile = compareTerrainBoundaryTops(negativeViews, positiveViews, line);
    if (!profile) {
        return profile.error() == TerrainBoundaryError::SegmentLimitExceeded
            ? StreamingSolidError::IncompleteProfile : StreamingSolidError::InvalidData;
    }
    const auto floorFor = [&](PolicyKind policy) -> Result<std::optional<double>, StreamingSolidError> {
        if (policy != PolicyKind::Hex) return std::optional<double>{};
        if (!std::isfinite(config_.hexFloorHeight)) return StreamingSolidError::InvalidData;
        return std::optional<double>{double(config_.hexFloorHeight)};
    };
    const auto negativeFloor = floorFor(negative);
    const auto positiveFloor = floorFor(positive);
    if (!negativeFloor) return negativeFloor.error();
    if (!positiveFloor) return positiveFloor.error();
    return solidResult(classifyTerrainBoundarySolid(TerrainBoundarySolidQuery{
        line, std::span<const TerrainBoundarySpan>(profile.value().data(), profile.value().size()),
        negative, positive, negativeFloor.value(), positiveFloor.value(), tangent, y}));
}

auto WorldStreamer::classifyPublishedSolid(double x, double y, double z) const
    -> Result<TerrainSolidSample, StreamingSolidError> {
    return impl_->classifyPublishedSolid(x, y, z);
}

auto WorldStreamer::sampleHeightTerrain(float worldX, float worldZ) const
    -> std::optional<HeightSample> {
    const std::optional<std::int32_t> chunkX =
        cameraChunk(worldX, static_cast<std::int32_t>(kHeightQuads));
    const std::optional<std::int32_t> chunkZ =
        cameraChunk(worldZ, static_cast<std::int32_t>(kHeightQuads));
    if (!chunkX.has_value() || !chunkZ.has_value()) return std::nullopt;
    const StreamingChunkKey key{PolicyKind::Height, {*chunkX, 0, *chunkZ}};
    const std::optional<ChunkSnapshot> copy = snapshot(key);
    if (!copy.has_value()) return std::nullopt;
    const HeightTile* base = std::get_if<HeightTile>(&copy->base);
    if (base == nullptr) return std::nullopt;
    HeightTile tile = *base;
    for (const ChunkDelta& delta : copy->deltas) {
        if (delta.value.kind != PolicyKind::Height || delta.index >= tile.samples.size()) {
            return std::nullopt;
        }
        tile.samples[delta.index] = delta.value.height;
    }
    const float localX = worldX - static_cast<float>(*chunkX) * kHeightQuads;
    const float localZ = worldZ - static_cast<float>(*chunkZ) * kHeightQuads;
    return sampleHeight(tile, localX, localZ);
}

} // namespace f4::world
