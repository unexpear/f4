#pragma once

#include <chrono>
#include <array>
#include <optional>
#include <unordered_map>

#include "engine/rendering/renderer.h"
#include "engine/world/world_streaming.h"
#include "engine/world/boundary_composition_job.h"

namespace f4::demo {

// Fixed height-world fixture. Changing generation parameters requires a save version bump.
inline constexpr std::uint64_t kStreamingDemoSeed = 0x4d313644454d4full;
inline constexpr std::chrono::microseconds kStreamingDemoFrameBudget{2000};
inline constexpr std::chrono::microseconds kStreamingDemoUploadReserve{1000};
inline constexpr world::StreamingChunkKey kEditedChunk{world::PolicyKind::Height, {0, 0, 0}};
auto streamingDemoConfig() -> world::WorldStreamingConfig;
// Concurrent boundary diagnostic, not a watertight transition implementation.
auto streamingMixedDemoConfig() -> world::WorldStreamingConfig;
void setStreamingMixedRegions(world::RegionMap& regions, unsigned phase = 0);
auto streamingDemoPatch() -> containers::Array<world::ChunkDelta>;
auto encodeStreamingDemoSave(const containers::Array<world::ChunkDelta>& edits)
    -> std::optional<containers::Array<std::byte>>;
auto decodeStreamingDemoSave(const containers::Array<std::byte>& bytes)
    -> std::optional<containers::Array<world::ChunkDelta>>;
auto applyStreamingDemoEdits(world::WorldStreamer& world,
                             const containers::Array<world::ChunkDelta>& edits) -> bool;

struct StreamingUploadStats {
    std::uint32_t uploaded = 0;
    std::uint32_t reused = 0; // Validated resident handles borrowed by a staged view.
    std::uint32_t retired = 0;
    std::uint64_t bytes = 0;
    std::uint32_t deferred = 0; // Ready versions left by the count or time limit.
    std::uint32_t budgetDeferred = 0;
    bool progressAfterDeadline = false;
    std::uint32_t reloadRetired = 0;
};

struct BoundaryUploadBudget {
    // One newly staged resident or composed-policy group is a work unit, including empty ones.
    std::uint32_t uploads = 2;
    std::uint32_t retirements = 2;
    // Admission deadline: already-expired means no GPU work, including retirement.
    // One admitted mesh conversion/upload or deletion cannot be preempted.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    // False retains the full-reupload reference path for diagnostics. Reuse
    // still validates world identity, published version, region and transform.
    bool reuseUnchanged = true;
};

// Demo integration adapter, not a replacement for the engine resource manager.
// 25 residents plus 25 detached reload meshes fit 64 slots. Composed replacement
// instead admits two complete 25+3 views, with no detached reload meshes.
// Each policy has a fixed opaque vertex-colored material group, so cross-policy
// depth ties do not inherit worker-dependent mesh allocation order. Not seam repair.
// Empty voxel meshes count as synchronized residents without an entity/GPU mesh.
class StreamingDemoView {
public:
    explicit StreamingDemoView(gfx::IDevice& device);
    ~StreamingDemoView();
    auto init() -> bool;
    // Deadline includes preceding CPU streaming/replay. Resident unload retirement is mandatory;
    // reload cleanup admits at most two old meshes (at least one even after expiry).
    // one ready upload may exceed the deadline to ensure progress. A zero count
    // disables uploads, including that exception. Omit deadline for count-only use.
    auto sync(world::WorldStreamer& world, std::uint32_t uploadLimit = 2,
              std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) -> bool;
    auto caughtUp(const world::WorldStreamer& world) const -> bool;
    // Explicit frozen inspection mode. Stage every policy mesh, validate the
    // complete dependency set, then switch visibility and view queries together.
    // Originals remain allocated for rollback. sync() freezes ALL displayed
    // residents until removal/replacement. Use syncBoundaryComposition for rebuilding.
    auto installBoundaryComposition(world::WorldStreamer& world,
                                     const world::StreamingBoundaryComposition& composition) -> bool;
    // Transactionally replace ALL residents and the corner from settled world
    // publication. Old GPU/query state survives any returned staging failure.
    // Up to 56 old/new terrain mesh slots coexist; no frame-budget guarantee.
    auto replaceBoundaryComposition(world::WorldStreamer& world,
                                     const world::StreamingBoundaryComposition& composition) -> bool;
    // Call instead of ordinary sync while following explicit boundary requests.
    // Pending/invalid sources retain the old view. False reports build/upload
    // failure; failed staging retires incrementally before retry. Staged entities
    // stay hidden, and every resident stamp is revalidated before commit.
    auto syncBoundaryComposition(world::WorldStreamer& world, world::BoundaryCompositionJob& job,
                                 BoundaryUploadBudget budget = {}) -> bool;
    auto boundaryUploadPending() const -> bool { return pendingBoundary_.has_value(); }
    void removeBoundaryComposition();
    void removeBoundaryComposition(world::WorldStreamer& world);
    auto hasBoundaryComposition() const -> bool { return boundary_.has_value(); }
    auto acceptedSurfaceCurrent(const world::WorldStreamer& world) const -> bool;
    // Queries the engine accepted-view slot, which is the package this view has
    // published. A retained result need not be current. Plain sync does not
    // replace the drawn meshes until that same package can be published.
    auto raycastDisplayedTerrain(const world::WorldStreamer& world, math::Vec3 origin,
                                  math::Vec3 direction, float maxDistance,
                                  bool cullBackFaces = false) const
        -> std::optional<world::StreamingBoundaryHit>;
    auto render(gfx::FramebufferHandle target, const engine::Camera& camera, float aspect)
        -> gfx::GfxError;
    void clear();
    void clear(world::WorldStreamer& world);
    // Stop drawing old terrain now; retire its meshes in sync (at most two/call).
    // Reject reload during boundary staging/retirement or an earlier reload.
    // Explicit remove/clear/destruction drain boundary resources immediately.
    auto beginReload() -> bool;
    auto beginReload(world::WorldStreamer& world) -> bool;
    auto pendingRetirements() const -> std::size_t { return reloadMeshes_.size() + boundaryRetired_.size(); }
    auto residentCount() const -> std::size_t { return chunks_.size(); }
    auto uploadStats() const -> StreamingUploadStats { return uploads_; }
    auto renderStats() const -> engine::FrameStats { return renderer_.stats(); }

private:
    auto publishBoundaryComposition(world::WorldStreamer& world,
                                    const world::StreamingBoundaryComposition& composition, bool replace) -> bool;
    struct Resident {
        world::ChunkHandle source;
        std::uint64_t version = 0;
        ecs::Entity entity;
        resources::MeshHandle mesh;
        std::shared_ptr<const world::MeshData> geometry;
        math::Vec3 origin;
        std::optional<std::uint64_t> regionRevision;
        std::shared_ptr<const std::byte> owner;
        bool borrowed = false; // Pending-only: the accepted view still owns these GPU handles.
    };
    struct Boundary {
        world::StreamingBoundaryComposition composition;
        std::array<resources::MeshHandle, 3> meshes;
        std::array<ecs::Entity, 3> entities;
    };
    struct BoundaryUpload {
        Boundary boundary;
        bool plain = false;
        std::uint64_t regionRevision = 0;
        std::shared_ptr<const std::byte> owner;
        containers::Array<world::StreamingChunkInfo> desired;
        std::unordered_map<world::StreamingChunkKey, Resident> chunks;
        std::size_t nextResident = 0, nextPolicy = 0;
    };
    auto makeSurfaceView(const std::unordered_map<world::StreamingChunkKey, Resident>& residents,
                         const world::StreamingBoundaryComposition* composition = nullptr) const
        -> std::optional<world::StreamingSurfaceView>;
    auto boundaryUploadCurrent(const world::WorldStreamer& world) const -> bool;
    auto residentCurrent(const Resident& resident, const world::WorldStreamer& world,
                         const world::StreamingChunkInfo& chunk) const -> bool;
    void cancelBoundaryUpload();
    void retireBoundaryMeshes(BoundaryUploadBudget budget);
    std::optional<BoundaryUpload> pendingBoundary_;
    std::shared_ptr<const std::byte> plainOwner_;
    std::uint64_t plainRevision_ = 0;
    containers::Array<resources::MeshHandle> boundaryRetired_;
    std::optional<Boundary> boundary_;
    std::optional<world::StreamingSurfaceView> surfaceView_;
    ecs::Registry registry_{64};
    engine::Renderer renderer_;
    ecs::Entity camera_;
    std::array<resources::MaterialHandle, 3> materials_;
    std::unordered_map<world::StreamingChunkKey, Resident> chunks_;
    containers::Array<resources::MeshHandle> reloadMeshes_;
    StreamingUploadStats uploads_;
};

} // namespace f4::demo
