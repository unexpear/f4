#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace f4::jobs {

struct JobPriority {
    std::uint64_t distanceSquared = 0;
    std::uint32_t stage = 0;
    friend constexpr auto operator==(const JobPriority&, const JobPriority&) -> bool = default;
};

class JobSystem {
public:
    explicit JobSystem(std::size_t workerCount = std::thread::hardware_concurrency());
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    auto operator=(const JobSystem&) -> JobSystem& = delete;

    auto submit(JobPriority priority, std::function<void()> job) -> bool;
    void waitIdle();
    void shutdown();

    [[nodiscard]] auto workerCount() const -> std::size_t { return workers_.size(); }
    [[nodiscard]] auto pendingCount() const -> std::size_t;
    [[nodiscard]] auto completedCount() const -> std::uint64_t { return completed_.load(); }
    [[nodiscard]] auto failedCount() const -> std::uint64_t { return failed_.load(); }

private:
    struct QueuedJob {
        JobPriority priority;
        std::uint64_t sequence = 0;
        std::function<void()> function;
    };

    struct LowerPriority {
        auto operator()(const QueuedJob& a, const QueuedJob& b) const -> bool;
    };

    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::priority_queue<QueuedJob, std::vector<QueuedJob>, LowerPriority> queue_;
    std::size_t active_ = 0;
    std::uint64_t nextSequence_ = 0;
    bool accepting_ = true;
    bool stopping_ = false;
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::vector<std::jthread> workers_;
};

} // namespace f4::jobs
