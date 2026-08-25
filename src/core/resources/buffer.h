#pragma once

#include <cstdint>

namespace f4::resources {

// GPU buffer descriptor. The device (M6) turns this into a live BufferHandle;
// at M5 it exists so loaders/tools can express intent before any backend does.
enum class BufferUsage : std::uint16_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};

[[nodiscard]] constexpr auto operator|(BufferUsage a, BufferUsage b) -> BufferUsage {
    return static_cast<BufferUsage>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

[[nodiscard]] constexpr auto hasUsage(BufferUsage value, BufferUsage flag) -> bool {
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}

struct BufferDesc {
    std::uint64_t sizeBytes = 0;
    BufferUsage usage = BufferUsage::None;
};

} // namespace f4::resources