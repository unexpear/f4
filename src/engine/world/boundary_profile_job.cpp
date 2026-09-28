#include "engine/world/boundary_profile_job.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace f4::world {

struct BoundaryProfileJob::Ticket {
    StreamingBoundarySnapshot snapshot;
    std::uint64_t generation = 0;
    std::optional<containers::Array<TerrainBoundarySpan>> spans;
    std::optional<TerrainBoundaryError> error;
    bool exception = false;
    // Single worker writes payload, then releases it to the main-thread reader.
    std::atomic<bool> done{false};
};

BoundaryProfileJob::BoundaryProfileJob(jobs::JobSystem& jobs, StreamingBoundaryLimits limits,
                                       std::size_t segmentLimit)
    : jobs_(jobs), limits_(limits), segmentLimit_(segmentLimit) {}

void BoundaryProfileJob::request(StreamingBoundaryRequest request) {
    clear();
    request_ = request;
    status_.state = BoundaryProfileState::WaitingSources;
}

void BoundaryProfileJob::clear() {
    ++generation_;
    request_.reset(); result_.reset(); failedInput_.reset(); status_ = {};
    // A queued callback owns only its ticket, never this controller or a world.
    // Keep the slot until it completes so replacement cannot grow the queue.
}

auto BoundaryProfileJob::result(const WorldStreamer& world) const -> const BoundaryProfileResult* {
    return result_ && world.boundaryCurrent(result_->snapshot) ? &*result_ : nullptr;
}

auto BoundaryProfileJob::update(const WorldStreamer& world, jobs::JobPriority priority) -> BoundaryProfileStatus {
    if (result_ && !world.boundaryCurrent(result_->snapshot)) { result_.reset(); status_ = {}; }
    if (failedInput_ && !world.boundaryCurrent(*failedInput_)) { failedInput_.reset(); status_ = {}; }
    if (ticket_) {
        if (!ticket_->done.load(std::memory_order_acquire)) {
            status_.state = request_ ? BoundaryProfileState::Queued : BoundaryProfileState::Idle;
            return status_;
        }
        auto completed = std::move(ticket_);
        if (!request_ || completed->generation != generation_ || !world.boundaryCurrent(completed->snapshot)) {
            ++stats_.discarded;
            status_ = {};
        } else if (completed->exception || completed->error) {
            failedInput_ = completed->snapshot;
            status_ = {}; status_.state = BoundaryProfileState::Failed;
            status_.workerException = completed->exception; status_.profileError = completed->error;
            ++stats_.failed;
        } else {
            const bool complete = !completed->spans->isEmpty() && std::all_of(completed->spans->begin(), completed->spans->end(),
                [](const auto& span) { return span.negativeCount == 1 && span.positiveCount == 1 && span.negative && span.positive; });
            result_ = BoundaryProfileResult{completed->snapshot, std::move(*completed->spans), complete};
            status_ = {}; status_.state = complete ? BoundaryProfileState::Ready : BoundaryProfileState::Incomplete;
            ++stats_.accepted;
        }
    }
    if (!request_) { status_ = {}; return status_; }
    if (result_ || failedInput_) return status_;
    const auto captured = world.captureBoundary(*request_, limits_);
    if (!captured) {
        status_ = {}; status_.captureError = captured.error();
        status_.state = captured.error() == StreamingBoundaryError::SourcesPending
            ? BoundaryProfileState::WaitingSources : BoundaryProfileState::Failed;
        return status_;
    }
    auto ticket = std::make_shared<Ticket>();
    ticket->snapshot = captured.value(); ticket->generation = generation_;
    const auto limit = segmentLimit_;
    const bool submitted = jobs_.submit(priority, [ticket, limit] {
        try {
            auto profile = ticket->snapshot.profile(limit);
            if (profile) ticket->spans = std::move(profile).value();
            else ticket->error = profile.error();
        } catch (...) { ticket->exception = true; }
        ticket->done.store(true, std::memory_order_release);
    });
    status_ = {};
    if (!submitted) {
        failedInput_ = ticket->snapshot;
        status_.state = BoundaryProfileState::Failed; status_.submissionRejected = true;
        ++stats_.failed;
        return status_;
    }
    ticket_ = std::move(ticket);
    ++stats_.submitted;
    status_.state = BoundaryProfileState::Queued;
    return status_;
}

} // namespace f4::world
