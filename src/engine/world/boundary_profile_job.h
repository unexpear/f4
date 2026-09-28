#pragma once

#include "core/jobs/job_system.h"
#include "engine/world/world_streaming.h"

namespace f4::world {

enum class BoundaryProfileState { Idle, WaitingSources, Queued, Ready, Incomplete, Failed };
struct BoundaryProfileStatus {
    BoundaryProfileState state = BoundaryProfileState::Idle;
    std::optional<StreamingBoundaryError> captureError;
    std::optional<TerrainBoundaryError> profileError;
    bool submissionRejected = false, workerException = false;
};
struct BoundaryProfileStats {
    std::uint64_t submitted = 0, accepted = 0, discarded = 0, failed = 0;
};
struct BoundaryProfileResult {
    StreamingBoundarySnapshot snapshot;
    containers::Array<TerrainBoundarySpan> spans;
    bool complete = false;
};

// Explicitly ticked main-thread controller for ONE boundary, using an external
// job pool. No WorldStreamer access occurs on a worker. At most one job remains
// outstanding even after request replacement/clear; stale work drains safely.
// Does not generate walls, discover boundaries, or impose a global/frame budget.
class BoundaryProfileJob {
public:
    explicit BoundaryProfileJob(jobs::JobSystem& jobs, StreamingBoundaryLimits limits = {},
                                std::size_t segmentLimit = 65536);
    BoundaryProfileJob(const BoundaryProfileJob&) = delete;
    auto operator=(const BoundaryProfileJob&) -> BoundaryProfileJob& = delete;
    // Also explicitly retries a failed unchanged input. Every call supersedes
    // the previous request, even when its values are equal.
    void request(StreamingBoundaryRequest request);
    void clear();
    auto update(const WorldStreamer& world, jobs::JobPriority priority = {0, 2}) -> BoundaryProfileStatus;
    // Freshness is checked again on access; never returns a stale cached result.
    // An Incomplete result exposes missing/overlapping spans, not usable walls.
    // Pointer valid until request/clear/update/destruction. Call before consuming
    // on the main thread, with no intervening world mutation.
    [[nodiscard]] auto result(const WorldStreamer& world) const -> const BoundaryProfileResult*;
    [[nodiscard]] auto inFlight() const -> bool { return bool(ticket_); }
    [[nodiscard]] auto stats() const -> BoundaryProfileStats { return stats_; }
private:
    struct Ticket;
    jobs::JobSystem& jobs_;
    StreamingBoundaryLimits limits_;
    std::size_t segmentLimit_;
    std::uint64_t generation_ = 0;
    std::optional<StreamingBoundaryRequest> request_;
    std::shared_ptr<Ticket> ticket_;
    std::optional<BoundaryProfileResult> result_;
    std::optional<StreamingBoundarySnapshot> failedInput_;
    BoundaryProfileStatus status_;
    BoundaryProfileStats stats_;
};

} // namespace f4::world
