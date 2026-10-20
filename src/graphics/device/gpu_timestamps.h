#pragma once

#include <array>
#include <cstdint>

namespace f4::gfx {

// GPU timestamp queries. A command buffer records CommandBuffer::writeTimestamp
// (index) outside render passes; the GPU stores the time at which every earlier
// command of that submission finished. Each submit that writes timestamps fills
// one of kGpuTimestampSets query sets in a ring, so reading results never has
// to wait for the GPU: IDevice::latestGpuTimestamps returns the newest set that
// has finished (typically one or two frames old).
inline constexpr std::uint32_t kMaxGpuTimestamps = 32;
inline constexpr std::uint32_t kGpuTimestampSets = 4;

struct GpuTimestampFrame {
    // Matches IDevice::lastGpuTimestampSerial() right after the submit that
    // wrote these timestamps; 0 when no timestamps finished yet.
    std::uint64_t serial = 0;
    std::uint32_t count = 0; // highest written index + 1
    // Nanoseconds on the GPU clock (only differences are meaningful). Indices
    // the submit did not write read 0.
    std::array<std::uint64_t, kMaxGpuTimestamps> ns{};
};

// Backend bookkeeping shared by the GL, Vulkan, and DX12 devices.
struct GpuTimestampRing {
    struct Set {
        std::uint64_t serial = 0;
        std::uint64_t fenceValue = 0; // DX12: submission fence
        std::uint32_t count = 0;
        std::uint32_t written = 0; // bit per index
        bool pending = false;
    };
    std::array<Set, kGpuTimestampSets> sets{};
    std::uint64_t lastSerial = 0;
    std::uint32_t next = 0;
    GpuTimestampFrame latest{};

    // Claims the next set for a submit whose timestamps use [0, count).
    auto begin(std::uint32_t count, std::uint32_t written) -> std::uint32_t {
        const std::uint32_t index = next;
        next = (next + 1) % kGpuTimestampSets;
        Set& set = sets[index];
        set.serial = ++lastSerial;
        set.count = count;
        set.written = written;
        set.fenceValue = 0;
        set.pending = true;
        return index;
    }

    // Visits pending sets newest first; `ready(set index)` reads the results
    // into ns[] (returning false when the GPU has not finished). The newest
    // finished set becomes `latest`; older pending sets are dropped.
    template <typename ReadyFn>
    auto poll(ReadyFn&& ready) -> const GpuTimestampFrame& {
        std::array<std::uint32_t, kGpuTimestampSets> order{};
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < kGpuTimestampSets; ++i) {
            if (sets[i].pending && sets[i].serial > latest.serial) order[count++] = i;
        }
        for (std::uint32_t a = 0; a < count; ++a) {
            for (std::uint32_t b = a + 1; b < count; ++b) {
                if (sets[order[b]].serial > sets[order[a]].serial) {
                    const std::uint32_t t = order[a];
                    order[a] = order[b];
                    order[b] = t;
                }
            }
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t index = order[k];
            GpuTimestampFrame frame;
            frame.serial = sets[index].serial;
            frame.count = sets[index].count;
            if (!ready(index, sets[index], frame.ns)) continue;
            for (std::uint32_t i = 0; i < kMaxGpuTimestamps; ++i) {
                if ((sets[index].written & (1u << i)) == 0) frame.ns[i] = 0;
            }
            latest = frame;
            for (Set& set : sets) {
                if (set.serial <= frame.serial) set.pending = false;
            }
            break;
        }
        return latest;
    }
};

} // namespace f4::gfx
