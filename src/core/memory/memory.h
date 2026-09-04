#pragma once

#include <cstddef>
#include <cstdint>

namespace f4::memory {

constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);

[[nodiscard]] constexpr auto isPowerOfTwo(std::size_t value) -> bool {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr auto alignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr auto isAlignedTo(std::size_t value, std::size_t alignment) -> bool {
    return (value & (alignment - 1)) == 0;
}

// Single seam for all engine heap allocations. Containers and allocators go
// through these wrappers only; replacing the backing (custom allocator,
// tracking layer, console malloc) means touching this file alone.
//
// Alignment must be a power of two. Zero-size requests are invalid
// (asserted); aligned operator new would throw or return a useless block.

auto heapAllocate(std::size_t size, std::size_t alignment = kDefaultAlignment) -> void*;

void heapFree(void* pointer, std::size_t alignment = kDefaultAlignment) noexcept;

// ---- Diagnostics: per-frame heap accounting --------------------------------
//
// heapAllocate/heapFree feed an optional HeapTracker. Metrics (console/HUD
// sinks) read per-frame snapshots via HeapTracker::frame(); beginFrame/endFrame
// delimit "this frame". The tracker is opt-in (enableHeapTracking) so the
// default engine path stays on the cheap atomics-only side.

struct HeapFrameStats {
    std::uint64_t allocations = 0;    // heapAllocate calls in the frame
    std::uint64_t frees = 0;          // heapFree calls in the frame
    std::uint64_t bytesAllocated = 0; // bytes requested in the frame
    std::uint64_t bytesFreed = 0;     // bytes returned in the frame
    std::uint64_t liveGroups = 0;     // distinct pointers still live
    std::uint64_t peakLiveBytes = 0;  // peak simultaneous live bytes in-frame
};

class HeapTracker {
public:
    void beginFrame();                         // latch frame-start counters
    void endFrame();                           // latch per-frame snapshot
    [[nodiscard]] HeapFrameStats frame() const;    // last completed frame
    [[nodiscard]] std::uint64_t totalAllocations() const;
    [[nodiscard]] std::uint64_t totalFrees() const;
    [[nodiscard]] std::uint64_t liveBytes() const;
    [[nodiscard]] std::uint64_t liveGroups() const;

    void recordAllocate(void* ptr, std::size_t size);
    void recordFree(void* ptr);

private:
    std::uint64_t frameStartAlloc_ = 0;
    std::uint64_t frameStartFrees_ = 0;
    std::uint64_t frameStartBytes_ = 0;
    std::uint64_t frameStartFreed_ = 0;
    HeapFrameStats frame_;

};

// Toggles tracker hooks in heapAllocate/heapFree. Disabled by default so the
// non-diagnostic path pays only atomic counters. Not meant to be toggled while
// other threads allocate.
void enableHeapTracking(bool enabled);

[[nodiscard]] auto heapTracker() -> HeapTracker&;

} // namespace f4::memory
