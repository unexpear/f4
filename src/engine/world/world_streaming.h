#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/containers/array.h"
#include "core/math/vec3.h"
#include "engine/world/terrain_meshing.h"
#include "engine/world/world_data.h"

namespace f4::world {

struct StreamingChunkKey {
    PolicyKind policy = PolicyKind::Voxel;
    ChunkCoord coord;
    friend constexpr auto operator==(const StreamingChunkKey&,
                                     const StreamingChunkKey&) -> bool = default;
};

struct WorldStreamingConfig {
    std::uint64_t worldSeed = 0;
    PolicyParams policyParams;
    PolicyKind defaultPolicy = PolicyKind::Height;
    std::int32_t regionExtentMeters = 512;
    std::uint32_t horizontalRadius = 2;
    std::uint32_t voxelVerticalRadius = 1;
    std::size_t chunkCapacityPerPolicy = 1024;
    std::size_t workerCount = 0;
    std::chrono::microseconds frameBudget{2000};
    std::uint32_t maxLoadsPerFrame = 8;
    std::uint32_t maxUnloadsPerFrame = 16;
    std::uint32_t maxPublicationsPerFrame = 16;
    std::uint32_t maxMeshJobsPerFrame = 8;
    float heightSkirtDepth = 4.0f;
    std::optional<float> heightTransitionFloor;
    float hexRadius = 1.0f;
    float hexFloorHeight = -8.0f;
};

struct StreamingFrameStats {
    std::uint32_t loaded = 0;
    std::uint32_t unloaded = 0;
    std::uint32_t completionsProcessed = 0;
    std::uint32_t published = 0;
    std::uint32_t meshJobs = 0;
    std::chrono::microseconds elapsed{};
};

struct StreamingStats {
    std::uint64_t submittedCellJobs = 0;
    std::uint64_t submittedMeshJobs = 0;
    std::uint64_t publishedCells = 0;
    std::uint64_t publishedMeshes = 0;
    std::uint64_t staleCompletions = 0;
    std::uint64_t failedCompletions = 0;
    std::uint64_t unloadedChunks = 0;
};

struct StreamingChunkInfo {
    StreamingChunkKey key;
    ChunkHandle handle;
    ChunkState state = ChunkState::Invalid;
    DirtyFlags dirty;
    std::uint64_t version = 0;
    std::uint64_t distanceSquared = 0;
    bool generationInFlight = false;
    bool meshInFlight = false;
    bool hasMesh = false;
};

class WorldStreamer {
public:
    explicit WorldStreamer(const WorldStreamingConfig& config = {});
    ~WorldStreamer();

    WorldStreamer(const WorldStreamer&) = delete;
    auto operator=(const WorldStreamer&) -> WorldStreamer& = delete;

    // Configuration and update/publication are main-thread operations. Worker
    // jobs receive only immutable generation requests and chunk snapshots.
    [[nodiscard]] auto regions() -> RegionMap&;
    [[nodiscard]] auto blocks() -> BlockRegistry&;

    auto update(math::Vec3 cameraPosition) -> StreamingFrameStats;
    void waitForIdle();
    [[nodiscard]] auto hasPendingWork() const -> bool;

    [[nodiscard]] auto residentCount() const -> std::size_t;
    [[nodiscard]] auto residentChunks() const -> containers::Array<StreamingChunkInfo>;
    [[nodiscard]] auto chunk(StreamingChunkKey key) const
        -> std::optional<StreamingChunkInfo>;
    [[nodiscard]] auto mesh(StreamingChunkKey key) const -> const MeshData*;
    [[nodiscard]] auto snapshot(StreamingChunkKey key) const
        -> std::optional<ChunkSnapshot>;
    [[nodiscard]] auto stats() const -> StreamingStats;

    auto edit(StreamingChunkKey key, std::uint32_t index, const CellValue& value) -> bool;
    [[nodiscard]] auto raycastVoxelTerrain(math::Vec3 origin, math::Vec3 direction,
                                           float maxDistance) const
        -> std::optional<VoxelRayHit>;
    [[nodiscard]] auto sampleHeightTerrain(float worldX, float worldZ) const
        -> std::optional<HeightSample>;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace f4::world

template <>
struct std::hash<f4::world::StreamingChunkKey> {
    auto operator()(const f4::world::StreamingChunkKey& value) const noexcept -> std::size_t;
};
