#include "demos/streaming_demo_io.h"

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace f4::demo {
namespace {
void requireIo(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void writeSave(const std::filesystem::path& path,
               const containers::Array<world::ChunkDelta>& edits) {
    const auto bytes = encodeStreamingDemoSave(edits);
    requireIo(bytes.has_value(), "Cannot encode demo edits");
    auto temporary = path;
    temporary += ".tmp";
    bool opened = false;
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        requireIo(file.is_open(), "Cannot open temporary demo save");
        opened = true;
        file.write(reinterpret_cast<const char*>(bytes->data()),
                   static_cast<std::streamsize>(bytes->size()));
        file.flush();
        file.close();
        requireIo(file.good(), "Cannot write/flush/close demo save");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (opened) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        throw;
    }
}
} // namespace

auto readStreamingDemoSave(const std::filesystem::path& path)
    -> containers::Array<world::ChunkDelta> {
    constexpr std::size_t limit = 128u * 1024u;
    std::ifstream file(path, std::ios::binary);
    requireIo(file.is_open(), "Cannot read demo save");
    // Bound the read itself, not just a prior file_size query that can go stale.
    containers::Array<std::byte> bytes;
    bytes.resize(limit + 1);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    requireIo(!file.bad() && file.eof(), "Demo save exceeds 128 KiB or read failed");
    const auto count = static_cast<std::size_t>(file.gcount());
    requireIo(count <= limit, "Demo save exceeds 128 KiB");
    bytes.resize(count);
    auto edits = decodeStreamingDemoSave(bytes);
    requireIo(edits.has_value(), "Invalid demo save (format, seed, policy, cells or truncation)");
    return std::move(*edits);
}

class StreamingDemoIo::Impl {
public:
    struct Job {
        StreamingIoKind kind = StreamingIoKind::Save;
        std::filesystem::path path;
        containers::Array<world::ChunkDelta> edits;
        std::unique_ptr<world::WorldStreamer> world;
    };
    Impl() : worker_([this] { run(); }) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || job_.has_value(); });
                if (!job_) return;
                job = std::move(*job_);
                job_.reset();
            }
            StreamingIoResult result;
            result.kind = job.kind;
            const auto start = std::chrono::steady_clock::now();
            try {
                switch (job.kind) {
                case StreamingIoKind::Save: writeSave(job.path, job.edits); break;
                case StreamingIoKind::Load:
                    result.edits = readStreamingDemoSave(job.path);
                    result.world = std::make_unique<world::WorldStreamer>(streamingDemoConfig());
                    break;
                case StreamingIoKind::Retire: job.world.reset(); break;
                }
            } catch (...) {
                result.error = std::current_exception();
            }
            result.workerUs = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard lock(mutex_);
                result_ = std::move(result);
            }
        }
    }
    // Long-running work and world destruction always occur outside this lock.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> job_;
    std::optional<StreamingIoResult> result_;
    bool outstanding_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

StreamingDemoIo::StreamingDemoIo() : impl_(std::make_unique<Impl>()) {}
StreamingDemoIo::~StreamingDemoIo() = default;
void StreamingDemoIo::shutdown() { impl_->shutdown(); }

auto StreamingDemoIo::save(const std::filesystem::path& path,
                           const containers::Array<world::ChunkDelta>& edits) -> bool {
    std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_ || impl_->outstanding_) return false;
    impl_->job_ = Impl::Job{StreamingIoKind::Save, path, edits, {}};
    impl_->outstanding_ = true;
    impl_->wake_.notify_one();
    return true;
}
auto StreamingDemoIo::load(const std::filesystem::path& path) -> bool {
    std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_ || impl_->outstanding_) return false;
    impl_->job_ = Impl::Job{StreamingIoKind::Load, path, {}, {}};
    impl_->outstanding_ = true;
    impl_->wake_.notify_one();
    return true;
}
auto StreamingDemoIo::retire(std::unique_ptr<world::WorldStreamer>& world) -> bool {
    std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_ || impl_->outstanding_ || !world) return false;
    impl_->job_ = Impl::Job{StreamingIoKind::Retire, {}, {}, std::move(world)};
    impl_->outstanding_ = true;
    impl_->wake_.notify_one();
    return true;
}
auto StreamingDemoIo::poll() -> std::optional<StreamingIoResult> {
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->result_) return std::nullopt;
    auto result = std::move(impl_->result_);
    impl_->result_.reset();
    impl_->outstanding_ = false;
    return result;
}
auto StreamingDemoIo::busy() const -> bool {
    std::lock_guard lock(impl_->mutex_);
    return impl_->outstanding_;
}
} // namespace f4::demo
