#pragma once

#include <exception>
#include <filesystem>
#include <memory>
#include <optional>

#include "demos/streaming_demo_state.h"

namespace f4::demo {

// Bounded read and codec validation. Throws on failure; also used before CLI startup.
auto readStreamingDemoSave(const std::filesystem::path& path)
    -> containers::Array<world::ChunkDelta>;

enum class StreamingIoKind { Save, Load, Retire };
struct StreamingIoResult {
    StreamingIoKind kind = StreamingIoKind::Save;
    containers::Array<world::ChunkDelta> edits;
    std::unique_ptr<world::WorldStreamer> world;
    std::exception_ptr error;
    double workerUs = 0;
};

// One CPU-only worker and one outstanding request, including an unconsumed result.
// Calls are main-thread only. No worker may access live world/GPU state.
class StreamingDemoIo {
public:
    StreamingDemoIo();
    ~StreamingDemoIo();
    StreamingDemoIo(const StreamingDemoIo&) = delete;
    auto operator=(const StreamingDemoIo&) -> StreamingDemoIo& = delete;
    auto save(const std::filesystem::path& path,
              const containers::Array<world::ChunkDelta>& edits) -> bool;
    auto load(const std::filesystem::path& path) -> bool;
    // Ownership transfers only on success, after the caller stops using this world.
    auto retire(std::unique_ptr<world::WorldStreamer>& world) -> bool;
    auto poll() -> std::optional<StreamingIoResult>;
    auto busy() const -> bool;
    // Shutdown only: drains an accepted operation and joins. poll can recover its result.
    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f4::demo
