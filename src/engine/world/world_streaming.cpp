#include "engine/world/world_streaming.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/jobs/job_system.h"

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

[[nodiscard]] auto worldCenter(std::int32_t coordinate, std::int32_t extent)
    -> std::optional<std::int32_t> {
    const std::int64_t center = static_cast<std::int64_t>(coordinate) * extent + extent / 2;
    if (center < std::numeric_limits<std::int32_t>::min() ||
        center > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(center);
}

[[nodiscard]] auto squaredDistance(StreamingChunkKey key, math::Vec3 camera,
                                   const PolicyParams& params) -> std::uint64_t {
    const std::int32_t extent = chunkExtent(key.policy, params);
    const long double centerX = static_cast<long double>(key.coord.x) * extent + extent * 0.5L;
    const long double centerY = key.policy == PolicyKind::Voxel
        ? static_cast<long double>(key.coord.y) * extent + extent * 0.5L
        : 0.0L;
    const long double centerZ = static_cast<long double>(key.coord.z) * extent + extent * 0.5L;
    const long double dx = centerX - static_cast<long double>(camera.x);
    const long double dy = centerY - static_cast<long double>(camera.y);
    const long double dz = centerZ - static_cast<long double>(camera.z);
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
    const std::int64_t localQ = static_cast<std::int64_t>(global.q) -
        static_cast<std::int64_t>(snapshot.coord.x) * tile->width;
    const std::int64_t localR = static_cast<std::int64_t>(global.r) -
        static_cast<std::int64_t>(snapshot.coord.z) * tile->width;
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

} // namespace

auto std::hash<f4::world::StreamingChunkKey>::operator()(
    const f4::world::StreamingChunkKey& value) const noexcept -> std::size_t {
    const std::size_t coordinate = std::hash<f4::world::ChunkCoord>{}(value.coord);
    const std::size_t policy = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(value.policy));
    return coordinate ^ (policy + 0x9e3779b9u + (coordinate << 6u) + (coordinate >> 2u));
}

namespace f4::world {

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

    ~Impl() { jobs_.shutdown(); }

    struct Entry {
        ChunkHandle handle;
        std::uint64_t distanceSquared = 0;
        bool generationInFlight = false;
        bool meshInFlight = false;
        std::optional<std::uint64_t> failedMeshVersion;
        std::optional<MeshData> mesh;
    };

    struct Candidate {
        StreamingChunkKey key;
        std::uint64_t distanceSquared = 0;
    };

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
        return config_.frameBudget.count() > 0 &&
            std::chrono::steady_clock::now() - start < config_.frameBudget;
    }

    [[nodiscard]] auto desiredChunks(math::Vec3 camera) const -> std::vector<Candidate> {
        std::vector<Candidate> output;
        if (!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z)) {
            return output;
        }
        constexpr PolicyKind policies[]{PolicyKind::Voxel, PolicyKind::Height, PolicyKind::Hex};
        for (PolicyKind policy : policies) {
            const std::int32_t extent = chunkExtent(policy, config_.policyParams);
            const std::optional<std::int32_t> centerX = cameraChunk(camera.x, extent);
            const std::optional<std::int32_t> centerZ = cameraChunk(camera.z, extent);
            const std::optional<std::int32_t> centerY = policy == PolicyKind::Voxel
                ? cameraChunk(camera.y, extent)
                : std::optional<std::int32_t>(0);
            if (!centerX.has_value() || !centerY.has_value() || !centerZ.has_value()) continue;
            const std::int64_t horizontal = config_.horizontalRadius;
            const std::int64_t vertical = policy == PolicyKind::Voxel
                ? static_cast<std::int64_t>(config_.voxelVerticalRadius)
                : 0;
            for (std::int64_t dz = -horizontal; dz <= horizontal; ++dz) {
                for (std::int64_t dx = -horizontal; dx <= horizontal; ++dx) {
                    const std::optional<std::int32_t> x = addChunkOffset(*centerX, dx);
                    const std::optional<std::int32_t> z = addChunkOffset(*centerZ, dz);
                    if (!x.has_value() || !z.has_value()) continue;
                    const std::optional<std::int32_t> xCenter = worldCenter(*x, extent);
                    const std::optional<std::int32_t> zCenter = worldCenter(*z, extent);
                    if (!xCenter.has_value() || !zCenter.has_value() ||
                        regions_.policyAt(*xCenter, *zCenter) != policy) {
                        continue;
                    }
                    for (std::int64_t dy = -vertical; dy <= vertical; ++dy) {
                        const std::optional<std::int32_t> y = addChunkOffset(*centerY, dy);
                        if (!y.has_value()) continue;
                        const StreamingChunkKey key{policy, {*x, *y, *z}};
                        output.push_back({key, squaredDistance(key, camera, config_.policyParams)});
                    }
                }
            }
        }
        std::sort(output.begin(), output.end(), [](const Candidate& a, const Candidate& b) {
            if (a.distanceSquared != b.distanceSquared) {
                return a.distanceSquared < b.distanceSquared;
            }
            return keyLess(a.key, b.key);
        });
        output.erase(std::unique(output.begin(), output.end(), [](const Candidate& a,
                                                                 const Candidate& b) {
            return a.key == b.key;
        }), output.end());
        return output;
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

    [[nodiscard]] auto neighborSnapshots(StreamingChunkKey key) const
        -> std::vector<ChunkSnapshot> {
        std::vector<ChunkSnapshot> output;
        for (const auto& [candidateKey, entry] : entries_) {
            if (candidateKey.policy != key.policy ||
                std::abs(static_cast<std::int64_t>(candidateKey.coord.x) - key.coord.x) > 1 ||
                std::abs(static_cast<std::int64_t>(candidateKey.coord.y) - key.coord.y) > 1 ||
                std::abs(static_cast<std::int64_t>(candidateKey.coord.z) - key.coord.z) > 1) {
                continue;
            }
            const std::optional<ChunkSnapshot> copy = storeFor(key.policy).snapshot(entry.handle);
            if (copy.has_value()) output.push_back(*copy);
        }
        std::sort(output.begin(), output.end(), [](const ChunkSnapshot& a,
                                                   const ChunkSnapshot& b) {
            return keyLess({a.policy, a.coord}, {b.policy, b.coord});
        });
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
        work->hexRadius = config_.hexRadius;
        work->hexFloorHeight = config_.hexFloorHeight;
        const jobs::JobPriority priority{entry.distanceSquared, kMeshStage};
        const bool submitted = jobs_.submit(priority, [this, work, priority] {
            TerrainMeshingContext context;
            context.blocks = &work->blocks;
            context.voxel.borderFetch = [work](std::int32_t x, std::int32_t y,
                                                std::int32_t z) -> std::uint16_t {
                const ChunkCoord coord{floorDiv(x, static_cast<std::int32_t>(kVoxelEdge)),
                                       floorDiv(y, static_cast<std::int32_t>(kVoxelEdge)),
                                       floorDiv(z, static_cast<std::int32_t>(kVoxelEdge))};
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
            context.height.skirtDepth = work->heightSkirtDepth;
            context.height.transitionFloor = work->heightTransitionFloor;
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
            try {
                auto result = meshChunkSnapshot(work->center, context);
                if (result.hasValue()) completion.mesh = std::move(result).value();
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

    void persist(const StreamingChunkKey& key, const ChunkSnapshot& snapshot) {
        if (snapshot.deltas.isEmpty()) {
            persistence_.erase(key);
            return;
        }
        auto& values = persistence_[key];
        values.clear();
        for (const ChunkDelta& delta : snapshot.deltas) values[delta.index] = delta.value;
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

    auto update(math::Vec3 camera) -> StreamingFrameStats {
        const auto start = std::chrono::steady_clock::now();
        StreamingFrameStats frame;
        const std::vector<Candidate> desired = desiredChunks(camera);
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
        for (const Candidate& candidate : unloads) {
            if (frame.unloaded >= config_.maxUnloadsPerFrame || !withinBudget(start)) break;
            const auto found = entries_.find(candidate.key);
            if (found == entries_.end()) continue;
            ChunkStore& store = storeFor(candidate.key.policy);
            const std::optional<ChunkSnapshot> copy = store.snapshot(found->second.handle);
            if (copy.has_value()) persist(candidate.key, *copy);
            dirtyNeighbors(candidate.key);
            store.destroy(found->second.handle);
            entries_.erase(found);
            ++frame.unloaded;
            ++stats_.unloadedChunks;
        }

        for (const Candidate& candidate : desired) {
            if (frame.loaded >= config_.maxLoadsPerFrame || !withinBudget(start)) break;
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
        }

        collectCompletions();
        while (!pendingCompletions_.empty() &&
               frame.completionsProcessed < config_.maxPublicationsPerFrame &&
               withinBudget(start)) {
            Completion completion = std::move(pendingCompletions_.front());
            pendingCompletions_.erase(pendingCompletions_.begin());
            if (processCompletion(std::move(completion))) ++frame.published;
            ++frame.completionsProcessed;
        }

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
            if (frame.meshJobs >= config_.maxMeshJobsPerFrame || !withinBudget(start)) break;
            const auto found = entries_.find(candidate.key);
            if (found == entries_.end()) continue;
            std::optional<ChunkSnapshot> copy =
                storeFor(candidate.key.policy).snapshot(found->second.handle);
            if (!copy.has_value()) continue;
            submitMesh(candidate.key, found->second, std::move(*copy));
            if (found->second.meshInFlight) ++frame.meshJobs;
        }

        frame.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        return frame;
    }

    [[nodiscard]] auto info(StreamingChunkKey key, const Entry& entry) const
        -> StreamingChunkInfo {
        const ChunkStore& store = storeFor(key.policy);
        return {key, entry.handle, store.state(entry.handle), store.dirty(entry.handle),
                store.version(entry.handle), entry.distanceSquared,
                entry.generationInFlight, entry.meshInFlight, entry.mesh.has_value()};
    }

    WorldStreamingConfig config_;
    RegionMap regions_;
    BlockRegistry blocks_;
    ChunkStore voxelStore_;
    ChunkStore heightStore_;
    ChunkStore hexStore_;
    std::unordered_map<StreamingChunkKey, Entry> entries_;
    std::map<StreamingChunkKey, std::map<std::uint32_t, CellValue>, StreamingKeyLess>
        persistence_;
    StreamingStats stats_;
    mutable std::mutex completionMutex_;
    std::vector<Completion> completed_;
    std::vector<Completion> pendingCompletions_;
    jobs::JobSystem jobs_;
};

WorldStreamer::WorldStreamer(const WorldStreamingConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

WorldStreamer::~WorldStreamer() = default;

auto WorldStreamer::regions() -> RegionMap& { return impl_->regions_; }

auto WorldStreamer::blocks() -> BlockRegistry& { return impl_->blocks_; }

auto WorldStreamer::update(math::Vec3 cameraPosition) -> StreamingFrameStats {
    return impl_->update(cameraPosition);
}

void WorldStreamer::waitForIdle() { impl_->jobs_.waitIdle(); }

auto WorldStreamer::hasPendingWork() const -> bool {
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
