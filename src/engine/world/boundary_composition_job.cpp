#include "engine/world/boundary_composition_job.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace f4::world {
struct BoundaryCompositionJob::Ticket {
    std::vector<StreamingBoundarySnapshot> snapshots;
    std::uint64_t generation = 0;
    std::optional<StreamingBoundaryComposition> composition;
    std::optional<BoundaryReplacementError> error;
    bool exception = false;
    std::atomic<bool> done{false};
};

BoundaryCompositionJob::BoundaryCompositionJob(jobs::JobSystem& pool, StreamingBoundaryLimits captureLimits,
    BoundaryCompositionLimits buildLimits, std::size_t snapshotBytes)
    : pool_(pool), captureLimits_(captureLimits), buildLimits_(buildLimits), snapshotBytes_(snapshotBytes) {}

auto BoundaryCompositionJob::request(std::span<const StreamingBoundaryRequest> requests) -> bool {
    if (requests.empty() || requests.size() > buildLimits_.maxBoundaries || requests.size() > 256) return false;
    std::vector<StreamingBoundaryRequest> next(requests.begin(), requests.end());
    clear(); requests_ = std::move(next);
    status_.state = BoundaryCompositionState::WaitingSources;
    return true;
}
void BoundaryCompositionJob::clear() {
    retireRequested_ = false;
    discoveryError_.reset();
    ++generation_; requests_.clear(); result_.reset(); failedInput_.clear(); status_ = {};
    // Keep the outstanding slot until its callback drains, even after clear.
}
void BoundaryCompositionJob::requestRetirement() {
    clear(); retireRequested_ = true; status_.state = BoundaryCompositionState::NoBoundaries;
}

auto BoundaryCompositionJob::requestWindow(const RegionMap& regions, TerrainFootprint window, std::size_t maxTiles)
    -> std::optional<BoundaryDiscoveryError> {
    auto discovered = discoverTerrainBoundaries(regions, window, {maxTiles, buildLimits_.maxBoundaries});
    if (!discovered) {
        clear(); discoveryError_ = discovered.error();
        status_.state = BoundaryCompositionState::Failed; status_.discoveryError = discoveryError_;
        return discoveryError_;
    }
    const auto& next = discovered.value();
    if (next.empty()) {
        if (!retireRequested_) requestRetirement();
    } else {
        const auto same = [](const auto& a, const auto& b) {
            return a.line.axis == b.line.axis && a.line.plane == b.line.plane &&
                a.line.begin == b.line.begin && a.line.end == b.line.end &&
                a.negative == b.negative && a.positive == b.positive;
        };
        if (next.size() != requests_.size() || !std::equal(next.begin(), next.end(), requests_.begin(), same))
            (void)request(next);
    }
    return {};
}
auto BoundaryCompositionJob::requestResidentWindow(const WorldStreamer& world, std::size_t maxTiles)
    -> std::optional<BoundaryDiscoveryError> {
    const auto footprint = world.residentTerrainFootprint();
    if (!footprint) {
        clear(); discoveryError_ = BoundaryDiscoveryError::NoResidentCoverage;
        status_.state = BoundaryCompositionState::Failed; status_.discoveryError = discoveryError_;
        return discoveryError_;
    }
    return requestWindow(world.regions(), *footprint, maxTiles);
}
auto BoundaryCompositionJob::requestResidentWindow(const WorldStreamer& world, PolicyKind first, PolicyKind second,
                                                   std::size_t maxTiles)
    -> std::optional<BoundaryDiscoveryError> {
    const auto footprint = world.residentTerrainFootprint(first, second);
    if (!footprint) {
        clear(); discoveryError_ = BoundaryDiscoveryError::NoResidentCoverage;
        status_.state = BoundaryCompositionState::Failed; status_.discoveryError = discoveryError_;
        return discoveryError_;
    }
    auto discovered = discoverTerrainBoundaries(world.regions(), *footprint,
        {maxTiles, buildLimits_.maxBoundaries}, std::pair{first, second});
    if (!discovered) {
        clear(); discoveryError_ = discovered.error();
        status_.state = BoundaryCompositionState::Failed; status_.discoveryError = discoveryError_;
        return discoveryError_;
    }
    const auto& next = discovered.value();
    if (next.empty()) {
        if (!retireRequested_) requestRetirement();
        return {};
    }
    const auto same = [](const auto& a, const auto& b) {
        return a.line.axis == b.line.axis && a.line.plane == b.line.plane &&
            a.line.begin == b.line.begin && a.line.end == b.line.end &&
            a.negative == b.negative && a.positive == b.positive;
    };
    if (next.size() != requests_.size() || !std::equal(next.begin(), next.end(), requests_.begin(), same))
        (void)request(next);
    return {};
}
auto BoundaryCompositionJob::result(const WorldStreamer& world) const -> const StreamingBoundaryComposition* {
    return result_ && world.boundaryCurrent(*result_) ? &*result_ : nullptr;
}
auto BoundaryCompositionJob::update(const WorldStreamer& world) -> BoundaryCompositionStatus {
    const auto current = [&](const auto& snapshots) {
        return std::all_of(snapshots.begin(), snapshots.end(), [&](const auto& s) { return world.boundaryCurrent(s); });
    };
    if (result_ && !world.boundaryCurrent(*result_)) { result_.reset(); status_ = {}; }
    if (!failedInput_.empty() && !current(failedInput_)) { failedInput_.clear(); status_ = {}; }
    if (ticket_) {
        if (!ticket_->done.load(std::memory_order_acquire)) {
            status_.state = discoveryError_ ? BoundaryCompositionState::Failed :
                retireRequested_ ? BoundaryCompositionState::NoBoundaries :
                (requests_.empty() ? BoundaryCompositionState::Idle : BoundaryCompositionState::Queued);
            return status_;
        }
        auto completed = std::move(ticket_);
        status_ = {};
        if (requests_.empty() || completed->generation != generation_ || !current(completed->snapshots)) {
            ++stats_.discarded;
        } else if (completed->exception || completed->error) {
            failedInput_ = std::move(completed->snapshots);
            status_.state = BoundaryCompositionState::Failed;
            status_.workerException = completed->exception; status_.buildError = completed->error;
            ++stats_.failed;
        } else {
            result_ = std::move(completed->composition);
            status_.state = BoundaryCompositionState::Ready; ++stats_.accepted;
        }
    }
    if (requests_.empty()) {
        status_ = {};
        if (discoveryError_) {
            status_.state = BoundaryCompositionState::Failed; status_.discoveryError = discoveryError_;
        } else if (retireRequested_) status_.state = BoundaryCompositionState::NoBoundaries;
        return status_;
    }
    if (result_ || !failedInput_.empty()) return status_;
    auto ticket = std::make_shared<Ticket>(); ticket->generation = generation_;
    // Bound retained copies during capture too, not only inside the worker.
    std::size_t bytes = 0;
    for (const auto& request : requests_) {
        auto limits = captureLimits_;
        limits.maxMeshBytes = std::min(limits.maxMeshBytes, snapshotBytes_ - bytes);
        auto captured = world.captureBoundary(request, limits);
        if (!captured) {
            status_ = {}; status_.captureError = captured.error();
            status_.state = captured.error() == StreamingBoundaryError::SourcesPending
                ? BoundaryCompositionState::WaitingSources : BoundaryCompositionState::Failed;
            return status_;
        }
        bytes += captured.value().meshBytes();
        ticket->snapshots.push_back(std::move(captured).value());
    }
    const auto limits = buildLimits_; const auto budget = snapshotBytes_;
    const bool submitted = pool_.submit({0, 3}, [ticket, limits, budget] {
        try {
            auto built = StreamingBoundaryComposition::build(ticket->snapshots, limits, budget);
            if (built) ticket->composition = std::move(built).value();
            else ticket->error = built.error();
        } catch (...) { ticket->exception = true; }
        ticket->done.store(true, std::memory_order_release);
    });
    status_ = {};
    if (!submitted) {
        failedInput_ = std::move(ticket->snapshots);
        status_.state = BoundaryCompositionState::Failed; status_.submissionRejected = true; ++stats_.failed;
    } else {
        ticket_ = std::move(ticket); ++stats_.submitted; status_.state = BoundaryCompositionState::Queued;
    }
    return status_;
}
} // namespace f4::world
