#include "core/memory/memory.h"

#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>

#include "core/assert.h"

namespace f4::memory {

auto heapAllocate(std::size_t size, std::size_t alignment) -> void* {
    F4_ASSERT(size > 0, "zero-size heap allocation");
    F4_ASSERT(isPowerOfTwo(alignment), "alignment must be a power of two");
    void* ptr = ::operator new(size, std::align_val_t(alignment));
    heapTracker().recordAllocate(ptr, size);
    return ptr;
}

void heapFree(void* pointer, std::size_t alignment) noexcept {
    heapTracker().recordFree(pointer);
    ::operator delete(pointer, std::align_val_t(alignment));
}

namespace {

// Atomic monotonic totals keep the non-tracked path cheap and thread-safe.
std::atomic<std::uint64_t> g_totalAllocations{0};
std::atomic<std::uint64_t> g_totalFrees{0};
std::atomic<std::uint64_t> g_totalBytes{0};
std::atomic<std::uint64_t> g_totalFreed{0};
std::atomic<std::uint64_t> g_liveBytes{0};
std::atomic<std::uint64_t> g_liveGroups{0};
std::atomic<std::uint64_t> g_peakLiveBytes{0};

// Pointer->size map only maintained while enabled (needed for freed-byte
// accounting). Guarded; std::unordered_map allocates via operator new directly
// (never our seam), so there is no recursion into the tracker.
std::atomic<bool> g_enabled{false};
std::mutex g_sizesMutex;
std::unordered_map<void*, std::uint64_t> g_sizes;

} // namespace

void enableHeapTracking(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        // Capture totals already established before tracking was enabled so
        // frame deltas only reflect allocations that happened while enabled.
        heapTracker().beginFrame();
    }
}

void HeapTracker::recordAllocate(void* ptr, std::size_t size) {
    const std::uint64_t u = static_cast<std::uint64_t>(size);
    g_totalBytes.fetch_add(u, std::memory_order_relaxed);
    g_totalAllocations.fetch_add(1, std::memory_order_relaxed);
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return; // live accounting only tracked while diagnostics are active
    }
    const std::uint64_t live = g_liveBytes.fetch_add(u, std::memory_order_relaxed) + u;
    g_liveGroups.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t peak = g_peakLiveBytes.load(std::memory_order_relaxed);
    while (peak < live && !g_peakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    std::lock_guard<std::mutex> lock(g_sizesMutex);
    g_sizes.emplace(ptr, u);
}

void HeapTracker::recordFree(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    g_totalFrees.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t size = 0;
    if (g_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_sizesMutex);
        const auto it = g_sizes.find(ptr);
        if (it != g_sizes.end()) {
            size = it->second;
            g_sizes.erase(it);
        }
    }
    if (size != 0) {
        g_totalFreed.fetch_add(size, std::memory_order_relaxed);
        g_liveBytes.fetch_sub(size, std::memory_order_relaxed);
        g_liveGroups.fetch_sub(1, std::memory_order_relaxed);
    }
}

void HeapTracker::beginFrame() {
    frameStartAlloc_ = g_totalAllocations.load(std::memory_order_relaxed);
    frameStartFrees_ = g_totalFrees.load(std::memory_order_relaxed);
    frameStartBytes_ = g_totalBytes.load(std::memory_order_relaxed);
    frameStartFreed_ = g_totalFreed.load(std::memory_order_relaxed);
    g_peakLiveBytes.store(g_liveBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void HeapTracker::endFrame() {
    HeapFrameStats s;
    s.allocations = g_totalAllocations.load(std::memory_order_relaxed) - frameStartAlloc_;
    s.frees = g_totalFrees.load(std::memory_order_relaxed) - frameStartFrees_;
    s.bytesAllocated = g_totalBytes.load(std::memory_order_relaxed) - frameStartBytes_;
    s.bytesFreed = g_totalFreed.load(std::memory_order_relaxed) - frameStartFreed_;
    s.liveGroups = g_liveGroups.load(std::memory_order_relaxed);
    s.peakLiveBytes = g_peakLiveBytes.load(std::memory_order_relaxed);
    frame_ = s;
    beginFrame(); // prime the next frame
}

auto HeapTracker::frame() const -> HeapFrameStats {
    return frame_;
}

auto HeapTracker::totalAllocations() const -> std::uint64_t {
    return g_totalAllocations.load(std::memory_order_relaxed);
}

auto HeapTracker::totalFrees() const -> std::uint64_t {
    return g_totalFrees.load(std::memory_order_relaxed);
}

auto HeapTracker::liveBytes() const -> std::uint64_t {
    return g_liveBytes.load(std::memory_order_relaxed);
}

auto HeapTracker::liveGroups() const -> std::uint64_t {
    return g_liveGroups.load(std::memory_order_relaxed);
}

auto heapTracker() -> HeapTracker& {
    static HeapTracker tracker;
    return tracker;
}

} // namespace f4::memory
