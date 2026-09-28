#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "core/containers/array.h"
#include "core/math/vec3.h"
#include "engine/world/terrain_meshing.h"
#include "engine/world/terrain_regions.h"
#include "engine/world/terrain_queries.h"
#include "engine/world/terrain_boundaries.h"
#include "engine/world/boundary_replacement.h"
#include "engine/world/world_data.h"

namespace f4::world {

class WorldStreamer;

struct StreamingChunkKey {
    PolicyKind policy = PolicyKind::Voxel;
    ChunkCoord coord;
    friend constexpr auto operator==(const StreamingChunkKey&,
                                     const StreamingChunkKey&) -> bool = default;
};

struct WorldStreamingConfig {
    std::uint64_t worldSeed = 0;
    // Standalone generation defaults to the legacy recipe; new streaming worlds tile.
    PolicyParams policyParams{.voxel = {}, .height = {}, .hex = {.layout = HexLayout::Tiled}};
    PolicyKind defaultPolicy = PolicyKind::Height;
    std::int32_t regionExtentMeters = 512;
    std::uint32_t horizontalRadius = 2;
    std::uint32_t voxelVerticalRadius = 1;
    std::size_t chunkCapacityPerPolicy = 1024;
    std::size_t workerCount = 0;
    // Cooperative budget: a positive value admits at least one operation per
    // enabled stage. Individual topology/publication operations are not preemptible.
    std::chrono::microseconds frameBudget{2000};
    std::uint32_t maxLoadsPerFrame = 8;
    std::uint32_t maxUnloadsPerFrame = 16;
    std::uint32_t maxPublicationsPerFrame = 16;
    std::uint32_t maxMeshJobsPerFrame = 8;
    float heightSkirtDepth = 4.0f;
    std::optional<float> heightTransitionFloor;
    float hexRadius = 1.0f;
    float hexFloorHeight = -8.0f;
    bool profileTimings = false;
    // Same-LOD height worlds can omit internal skirts only across equal-height edges.
    bool cullSharedHeightSkirts = false;
    // Optional closed XZ square centered on the exact camera position, in meters.
    // Selects geometric chunk centers, not intersecting mesh bounds. Overrides
    // horizontalRadius; voxelVerticalRadius and per-policy capacity still apply.
    // Null retains legacy chunk-count rings. Must be finite and nonnegative.
    std::optional<float> horizontalHalfExtentMeters;
    // Opt-in surface ownership: admit chunk bounds intersecting owned regions,
    // then clip CPU meshes after edit replay. Does not create transition walls
    // or change the legacy cell-DDA/bilinear APIs. Residency caps still apply.
    bool regionalSurfaceOwnership = false;
    // Close regional Hex prism cuts to hexFloorHeight, not to neighboring
    // terrain heights. Requires regionalSurfaceOwnership; default unchanged.
    bool regionalHexCutWalls = false;
    // Cell-aligned voxel cuts via ownership masking before face culling/AO.
    // Preserves cavities; requires regionalSurfaceOwnership. Default unchanged.
    bool regionalVoxelCutFaces = false;
};

struct StreamingStageTimes {
    std::chrono::nanoseconds planning{}, residency{}, unload{}, load{}, publication{}, mesh{};
};

struct StreamingFrameStats {
    bool planningPending = false;
    std::uint32_t loaded = 0;
    std::uint32_t unloaded = 0;
    std::uint32_t completionsProcessed = 0;
    std::uint32_t published = 0;
    std::uint32_t meshJobs = 0;
    std::chrono::microseconds elapsed{};
    StreamingStageTimes timings{}; // Populated only when profileTimings is enabled.
};

struct StreamingStats {
    std::uint64_t submittedCellJobs = 0;
    std::uint64_t submittedMeshJobs = 0;
    std::uint64_t publishedCells = 0;
    std::uint64_t publishedMeshes = 0;
    std::uint64_t staleCompletions = 0;
    std::uint64_t failedCompletions = 0;
    std::uint64_t unloadedChunks = 0;
    std::uint64_t neighborQueries = 0;
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
    std::uint64_t meshVersion = 0; // Published geometry; may precede current cells/version.
    std::optional<std::uint64_t> meshRegionRevision{}; // Null for legacy, unclipped publication.
};

struct PublishedTerrainHit {
    StreamingChunkKey key;
    ChunkHandle handle;
    std::uint64_t meshVersion = 0;
    TerrainMeshRayHit surface;
    std::optional<std::uint64_t> meshRegionRevision{};
};

struct StreamingBoundaryRequest {
    TerrainBoundaryLine line;
    PolicyKind negative = PolicyKind::Hex;
    PolicyKind positive = PolicyKind::Height;
};
struct StreamingBoundaryLimits {
    std::size_t maxSources = 256;
    std::size_t maxMeshBytes = 64u * 1024u * 1024u;
};
enum class StreamingBoundaryError { Unsupported, InvalidBoundary, SourcesPending, LimitExceeded };
struct StreamingBoundarySource {
    StreamingChunkKey key;
    ChunkHandle handle;
    std::uint64_t cellVersion = 0, meshVersion = 0;
};

// Combined immutable geometry with the original snapshot dependency stamps.
class StreamingBoundaryReplacement {
public:
    // Immutable common-world-frame geometry and provenance; lifetime is shared.
    // These accessors do NOT imply freshness. Check boundaryCurrent before use.
    [[nodiscard]] auto geometry() const -> const TerrainBoundaryReplacement*;
    [[nodiscard]] auto sources(bool positive) const -> std::span<const StreamingBoundarySource>;
private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend class StreamingBoundarySnapshot;
    friend class WorldStreamer;
};

// Owns immutable CPU geometry; copy this value to a worker, not WorldStreamer.
// Profiles remain readable after source edits/unloads/destruction. Capture
// freshness and profile completeness must be checked before publication.
class StreamingBoundarySnapshot {
public:
    StreamingBoundarySnapshot() = default;
    // Logical copied mesh payload, excluding container/ownership overhead.
    [[nodiscard]] auto meshBytes() const -> std::size_t;
    [[nodiscard]] auto profile(std::size_t segmentLimit = 65536) const
        -> Result<containers::Array<TerrainBoundarySpan>, TerrainBoundaryError>;
    [[nodiscard]] auto sources(bool positive) const -> std::span<const StreamingBoundarySource>;
    [[nodiscard]] auto regionRevision() const -> std::optional<std::uint64_t>;
    // Worker-safe immutable ONE-boundary package. No automatic scheduling or
    // publication; caller must establish replaceable-wall/solid-column semantics.
    [[nodiscard]] auto buildReplacement(BoundaryReplacementLimits limits = {},
                                        std::uint16_t negativeMaterial = 0, std::uint16_t positiveMaterial = 0) const
        -> Result<StreamingBoundaryReplacement, BoundaryReplacementError>;
private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend class WorldStreamer;
    friend class StreamingBoundaryComposition;
};

struct StreamingBoundaryHit {
    PublishedTerrainHit source; // surface.triangleIndex is the original source top/face id.
    std::size_t composedTriangle = 0;
    std::optional<std::size_t> boundary;
};

// Immutable union of single-boundary captures plus composed geometry. Build on
// workers; validate against WorldStreamer on the main thread before installation.
// Source identities are deduplicated; retained captures can contain duplicate
// mesh copies. snapshotBytes bounds their aggregate logical mesh payload.
class StreamingBoundaryComposition {
public:
    [[nodiscard]] static auto build(std::span<const StreamingBoundarySnapshot> snapshots,
                                    BoundaryCompositionLimits limits = {},
                                    std::size_t snapshotBytes = 128u * 1024u * 1024u)
        -> Result<StreamingBoundaryComposition, BoundaryReplacementError>;
    [[nodiscard]] auto geometry() const -> const TerrainBoundaryComposition*;
    [[nodiscard]] auto sources() const -> std::span<const StreamingBoundarySource>;
    [[nodiscard]] auto raycast(math::Vec3 origin, math::Vec3 direction, float maxDistance,
                               bool cullBackFaces = false) const -> std::optional<StreamingBoundaryHit>;
private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend class WorldStreamer;
};

enum class StreamingSurfaceViewError {
    InvalidData,
    DuplicateSource,
    MissingCompositionSource,
};

// Immutable accepted render/query input. A view retains the exact published
// resident mesh packages used by a renderer and, optionally, a transition
// composition that replaces its represented source surfaces. It is safe to
// copy between the main thread and query callers; current() is the explicit
// freshness check against a WorldStreamer.
struct StreamingSurfaceSource {
    StreamingChunkKey key;
    ChunkHandle handle;
    std::uint64_t meshVersion = 0;
    std::optional<std::uint64_t> regionRevision{};
    math::Vec3 origin;
    std::shared_ptr<const std::byte> owner;
    std::shared_ptr<const MeshData> geometry;
};

class StreamingSurfaceView {
public:
    StreamingSurfaceView() = default;

    [[nodiscard]] static auto build(std::span<const StreamingSurfaceSource> sources,
                                    std::optional<StreamingBoundaryComposition> composition = {})
        -> Result<StreamingSurfaceView, StreamingSurfaceViewError>;
    [[nodiscard]] auto sources() const -> std::span<const StreamingSurfaceSource>;
    [[nodiscard]] auto composition() const -> const StreamingBoundaryComposition*;
    // Accepted views may be retained while the world advances. This reports
    // whether all source stamps and the optional transition composition still
    // describe the world's current published data.
    [[nodiscard]] auto current(const WorldStreamer& world) const -> bool;
    // Queries the same immutable surfaces that a renderer consuming this view
    // should draw. Represented source surfaces are suppressed when a composed
    // transition is present; returned provenance identifies the source mesh or
    // composed boundary triangle.
    [[nodiscard]] auto raycast(math::Vec3 origin, math::Vec3 direction,
                               float maxDistance, bool cullBackFaces = false) const
        -> std::optional<StreamingBoundaryHit>;

private:
    struct Data;
    std::shared_ptr<const Data> data_;
};

class WorldStreamer {
public:
    // Throws std::invalid_argument for an invalid horizontalHalfExtentMeters.
    explicit WorldStreamer(const WorldStreamingConfig& config = {});
    ~WorldStreamer();

    WorldStreamer(const WorldStreamer&) = delete;
    auto operator=(const WorldStreamer&) -> WorldStreamer& = delete;

    // Configuration and update/publication are main-thread operations. Worker
    // jobs receive only immutable generation requests and chunk snapshots.
    [[nodiscard]] auto regions() -> RegionMap&;
    [[nodiscard]] auto regions() const -> const RegionMap&;
    // Opaque lifetime identity for immutable publication transactions.
    [[nodiscard]] auto identityToken() const -> std::shared_ptr<const std::byte>;
    [[nodiscard]] auto blocks() -> BlockRegistry&;

    auto update(math::Vec3 cameraPosition) -> StreamingFrameStats;
    void waitForIdle();
    [[nodiscard]] auto hasPendingWork() const -> bool;

    [[nodiscard]] auto residentCount() const -> std::size_t;
    [[nodiscard]] auto residentChunks() const -> containers::Array<StreamingChunkInfo>;
    // Conservative geographic union of the current camera-selected resident
    // chunk footprints. Empty before the first successful residency plan.
    [[nodiscard]] auto residentTerrainFootprint() const -> std::optional<TerrainFootprint>;
    // Same union restricted to the two policies that a transition composer
    // can consume, using their published mesh bounds after ownership clipping.
    // This is a caller-selected composition scope, not full-world coverage.
    [[nodiscard]] auto residentTerrainFootprint(PolicyKind first, PolicyKind second) const
        -> std::optional<TerrainFootprint>;
    // Accepted render/query publication. Publishing validates the complete
    // source stamp set first; an already accepted view remains queryable until
    // explicitly replaced or cleared, even if the world advances afterward.
    auto publishSurfaceView(const StreamingSurfaceView& view) -> bool;
    void clearSurfaceView();
    [[nodiscard]] auto acceptedSurfaceView() const -> const StreamingSurfaceView*;
    [[nodiscard]] auto raycastAcceptedTerrain(math::Vec3 origin, math::Vec3 direction,
                                              float maxDistance, bool cullBackFaces = false) const
        -> std::optional<StreamingBoundaryHit>;
    [[nodiscard]] auto chunk(StreamingChunkKey key) const
        -> std::optional<StreamingChunkInfo>;
    [[nodiscard]] auto mesh(StreamingChunkKey key) const -> const MeshData*;
    // Translation of mesher vertices, including axial hex spacing/scale.
    // Regional Hex meshes use canonical world-space positions and return zero;
    // do not use this mesh transform as a geographic chunk-center query.
    [[nodiscard]] auto meshOrigin(StreamingChunkKey key) const -> math::Vec3;
    [[nodiscard]] auto snapshot(StreamingChunkKey key) const
        -> std::optional<ChunkSnapshot>;
    [[nodiscard]] auto stats() const -> StreamingStats;

    // Main-thread reference capture of a uniform Height/Hex region boundary.
    // Requires regionalSurfaceOwnership, an integer region plane, <=4096 tiles,
    // and current published meshes for all resident footprint contributors.
    // Missing residents are recorded by the membership set/profile gaps, not
    // invented or loaded. Copies geometry within explicit source/byte limits.
    [[nodiscard]] auto captureBoundary(StreamingBoundaryRequest request,
                                       StreamingBoundaryLimits limits = {}) const
        -> Result<StreamingBoundarySnapshot, StreamingBoundaryError>;
    // Main-thread pull-based dependency gate, not a subscription/rescheduler.
    // False on a different owner, source change/arrival/unload, pending source,
    // or ANY region revision change, including before update() processes it.
    [[nodiscard]] auto boundaryCurrent(const StreamingBoundarySnapshot& snapshot) const -> bool;
    [[nodiscard]] auto boundaryCurrent(const StreamingBoundaryReplacement& replacement) const -> bool;
    [[nodiscard]] auto boundaryCurrent(const StreamingBoundaryComposition& composition) const -> bool;

    auto edit(StreamingChunkKey key, std::uint32_t index, const CellValue& value) -> bool;
    // Main-thread reference query of published CPU triangles for ALL policies,
    // including retained old meshes while remeshing. GPU upload may still lag.
    // Uses clipped geometry when regionalSurfaceOwnership is enabled, but has
    // no solid-volume semantics. Exact distance ties use
    // policy/X/Y/Z key, then triangle index, not worker/publication order.
    [[nodiscard]] auto raycastPublishedTerrain(math::Vec3 origin, math::Vec3 direction,
                                               float maxDistance, bool cullBackFaces = false) const
        -> std::optional<PublishedTerrainHit>;
    // Legacy resident-cell queries, NOT masked published surfaces. These retain
    // their occupancy/bilinear contracts even with regionalSurfaceOwnership on.
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
