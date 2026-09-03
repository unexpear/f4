#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/containers/array.h"
#include "graphics/buffer/buffer_desc.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/handle.h"

namespace f4::engine {

// Per-object constant data for a frame, exposed as a reusable engine utility.
//
// Why this exists: a graphics API queues draw commands but executes them only
// when the command buffer is submitted. On OpenGL, IDevice::writeBuffer applies
// immediately, so if several draws refer to a SINGLE uniform buffer and the app
// rewrites that buffer between the draws, every queued draw reads the LAST
// write — all objects collapse to one transform/color (a real bug we hit).
// Modern APIs (Vulkan/DX12) expose this same hazard explicitly and solve it with
// a dynamic-offset UBO or a ring of per-object buffers.
//
// ConstantRing owns a growable bank of uniform buffers for each buffered frame
// and hands each draw its own handle. Three rotating banks keep CPU writes away
// from buffers referenced by the Vulkan backend's two in-flight submissions.
//
// Usage (single-threaded, one frame at a time):
//   ring.beginFrame();
//   auto slot = ring.alloc(device, bytes);      // writes + returns a Slot
//   ring.bind(cb, slot);                        // binds that UBO at binding 0
//   ...
//   ring.endFrame();
class ConstantRing {
public:
    // A handed-out slot: a distinct UBO handle this draw should bind.
    struct Slot {
        gfx::BufferHandle handle{};
        bool valid = false;
    };

    ConstantRing() = default;
    ~ConstantRing() = default;
    ConstantRing(const ConstantRing&) = delete;
    ConstantRing& operator=(const ConstantRing&) = delete;

    // Allocate `capacity` uniform buffers of `strideBytes` each. The stride should
    // be a std140-aligned size (multiple of 16). Call once with a live device.
    [[nodiscard]] auto create(gfx::IDevice& device, std::uint32_t strideBytes,
                              std::uint32_t capacity) -> gfx::GfxError;

    void destroy(gfx::IDevice& device);

    [[nodiscard]] bool valid() const {
        return !handles_[0].isEmpty() && handles_[0][0].isValid();
    }
    [[nodiscard]] auto capacity() const -> std::size_t { return handles_[0].size(); }

    // Reset the write cursor. Call at the start of each frame.
    void beginFrame() {
        currentFrame_ = (currentFrame_ + 1) % kBufferedFrames;
        cursor_ = 0;
        allocationsThisFrame_ = 0;
    }

    // Write `data` (must be <= strideBytes) into the next ring slot and return it.
    // Multiple calls within one frame return distinct, still-valid handles. The
    // backing pool grows geometrically when the current frame needs more slots.
    [[nodiscard]] auto alloc(gfx::IDevice& device, std::span<const std::byte> data) -> Slot;

    // Bind the slot's UBO at (bindPoint=0, slot 0). No-op if invalid.
    void bind(gfx::CommandBuffer& cb, const Slot& slot);

    // Per-draw convenience: alloc-write-then-bind in one call, returned for the draw.
    [[nodiscard]] auto writeAndBind(gfx::IDevice& device, gfx::CommandBuffer& cb,
                                    std::span<const std::byte> data) -> Slot;

private:
    [[nodiscard]] auto grow(gfx::IDevice& device, std::size_t minimumCapacity)
        -> gfx::GfxError;

    gfx::IDevice* device_ = nullptr;
    static constexpr std::size_t kBufferedFrames = 3;
    std::uint32_t strideBytes_ = 0;
    std::size_t currentFrame_ = kBufferedFrames - 1;
    std::size_t cursor_ = 0;
    std::size_t allocationsThisFrame_ = 0;
    std::array<containers::Array<gfx::BufferHandle>, kBufferedFrames> handles_;
};

// Triple-buffered variable-size uploads. Each allocation owns one reusable
// buffer slot, so a recorded draw can retain its exact byte range without
// dynamic offsets. Slots grow geometrically and rotate across three frame
// banks to avoid rewriting storage still referenced by an in-flight submit.
class DynamicBufferRing {
public:
    struct Slot {
        gfx::BufferHandle handle{};
        std::uint32_t sizeBytes = 0;
        std::uint32_t capacityBytes = 0;
        bool valid = false;
    };

    DynamicBufferRing() = default;
    DynamicBufferRing(const DynamicBufferRing&) = delete;
    DynamicBufferRing& operator=(const DynamicBufferRing&) = delete;

    [[nodiscard]] auto create(gfx::IDevice& device, gfx::BufferUsage usage)
        -> gfx::GfxError;
    void destroy(gfx::IDevice& device);

    [[nodiscard]] auto valid() const -> bool { return device_ != nullptr; }
    [[nodiscard]] auto bufferCount() const -> std::size_t;

    void beginFrame() {
        currentFrame_ = (currentFrame_ + 1) % kBufferedFrames;
        cursor_ = 0;
    }

    [[nodiscard]] auto alloc(gfx::IDevice& device, std::span<const std::byte> data)
        -> Slot;

private:
    struct Record {
        gfx::BufferHandle handle{};
        std::uint32_t capacityBytes = 0;
    };

    gfx::IDevice* device_ = nullptr;
    gfx::BufferUsage usage_ = gfx::BufferUsage::None;
    static constexpr std::size_t kBufferedFrames = 3;
    std::size_t currentFrame_ = kBufferedFrames - 1;
    std::size_t cursor_ = 0;
    std::array<containers::Array<Record>, kBufferedFrames> records_;
};

} // namespace f4::engine
