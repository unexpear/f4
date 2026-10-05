#pragma once

#include <vector>
#include "core/jobs/job_system.h"
#include "engine/world/world_streaming.h"
#include "engine/world/boundary_discovery.h"

namespace f4::world {

enum class BoundaryCompositionState { Idle, WaitingSources, Queued, Ready, Failed, NoBoundaries };
struct BoundaryCompositionStatus {
    BoundaryCompositionState state = BoundaryCompositionState::Idle;
    std::optional<StreamingBoundaryError> captureError;
    std::optional<BoundaryReplacementError> buildError;
    std::optional<BoundaryDiscoveryError> discoveryError;
    bool submissionRejected = false, workerException = false;
};
struct BoundaryCompositionStats {
    std::uint64_t submitted = 0, accepted = 0, discarded = 0, failed = 0;
};

// Main-thread, explicitly ticked controller for a caller-selected boundary set.
// One immutable worker ticket at a time, including after request/clear. The pool
// must outlive this controller; queued callbacks never access it or the world.
class BoundaryCompositionJob {
public:
    explicit BoundaryCompositionJob(jobs::JobSystem& pool, StreamingBoundaryLimits captureLimits = {},
        BoundaryCompositionLimits buildLimits = {}, std::size_t snapshotBytes = 128u * 1024u * 1024u);
    BoundaryCompositionJob(const BoundaryCompositionJob&) = delete;
    auto operator=(const BoundaryCompositionJob&) -> BoundaryCompositionJob& = delete;
    // Invalid request counts leave the previous request untouched. Re-requesting
    // an accepted set explicitly retries an unchanged failed build.
    // Staging a different set keeps a still-current composition. Capture or
    // build failure of the new set leaves that composition in result() when
    // boundaryCurrent still holds. Discovery failure, retirement, and clear()
    // drop it.
    auto request(std::span<const StreamingBoundaryRequest> requests) -> bool;
    // Re-evaluate ownership in a caller-selected window. Repeating an unchanged
    // set preserves queued work and cached failures. Empty success retires;
    // discovery failure blocks publication, never selects plain terrain.
    // Call again after window/map changes; this does not assert source coverage.
    auto requestWindow(const RegionMap& regions, TerrainFootprint window,
                       std::size_t maxTiles = 4096) -> std::optional<BoundaryDiscoveryError>;
    // Derive the window from the current camera-selected resident chunk
    // footprints. No resident coverage fails closed; source readiness is still
    // checked by update() before a composition is accepted.
    auto requestResidentWindow(const WorldStreamer& world, std::size_t maxTiles = 4096)
        -> std::optional<BoundaryDiscoveryError>;
    // Discover the resident policy pair. The mesh rectangle can contain a
    // foreign edge, so the request uses the pair-adjacency rectangle instead.
    // Discovery still fails that rectangle when a foreign tile sits inside it.
    auto requestResidentWindow(const WorldStreamer& world, PolicyKind first, PolicyKind second,
                               std::size_t maxTiles = 4096) -> std::optional<BoundaryDiscoveryError>;
    // Explicitly target plain resident terrain. Unlike clear(), this asks the
    // view to retire transitions through its normal staged publication path.
    void requestRetirement();
    void clear();
    auto update(const WorldStreamer& world) -> BoundaryCompositionStatus;
    auto result(const WorldStreamer& world) const -> const StreamingBoundaryComposition*;
    auto status() const -> BoundaryCompositionStatus { return status_; }
    auto stats() const -> BoundaryCompositionStats { return stats_; }
    auto inFlight() const -> bool { return bool(ticket_); }
private:
    struct Ticket;
    jobs::JobSystem& pool_;
    StreamingBoundaryLimits captureLimits_;
    BoundaryCompositionLimits buildLimits_;
    std::size_t snapshotBytes_;
    std::uint64_t generation_ = 0;
    bool retireRequested_ = false;
    std::optional<BoundaryDiscoveryError> discoveryError_;
    std::vector<StreamingBoundaryRequest> requests_;
    std::vector<StreamingBoundaryRequest> acceptedRequests_;
    std::shared_ptr<Ticket> ticket_;
    std::vector<StreamingBoundarySnapshot> failedInput_;
    std::optional<StreamingBoundaryComposition> result_;
    BoundaryCompositionStatus status_;
    BoundaryCompositionStats stats_;
};
} // namespace f4::world
