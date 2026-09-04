#include "core/jobs/job_system.h"

#include <algorithm>
#include <utility>

namespace f4::jobs {

auto JobSystem::LowerPriority::operator()(const QueuedJob& a, const QueuedJob& b) const
    -> bool {
    if (a.priority.distanceSquared != b.priority.distanceSquared) {
        return a.priority.distanceSquared > b.priority.distanceSquared;
    }
    if (a.priority.stage != b.priority.stage) {
        return a.priority.stage > b.priority.stage;
    }
    return a.sequence > b.sequence;
}

JobSystem::JobSystem(std::size_t workerCount) {
    workerCount = std::max<std::size_t>(workerCount, 1);
    workers_.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

JobSystem::~JobSystem() { shutdown(); }

auto JobSystem::submit(JobPriority priority, std::function<void()> job) -> bool {
    if (!job) return false;
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) return false;
        queue_.push({priority, nextSequence_++, std::move(job)});
    }
    ready_.notify_one();
    return true;
}

void JobSystem::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void JobSystem::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        accepting_ = false;
    }
    waitIdle();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (std::thread& worker : workers_) worker.join();
    workers_.clear();
}

auto JobSystem::pendingCount() const -> std::size_t {
    std::lock_guard lock(mutex_);
    return queue_.size() + active_;
}

void JobSystem::workerLoop() {
    while (true) {
        QueuedJob job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = queue_.top();
            queue_.pop();
            ++active_;
        }

        try {
            job.function();
            ++completed_;
        } catch (...) {
            ++failed_;
        }

        {
            std::lock_guard lock(mutex_);
            --active_;
            if (queue_.empty() && active_ == 0) idle_.notify_all();
        }
    }
}

} // namespace f4::jobs
