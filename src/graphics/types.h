#pragma once

#include <cstdint>

namespace f4::gfx {

// ---------------------------------------------------------------------------
// Bake-time backend selection. Exactly one of these is defined per build from
// the F4_GRAPHICS_BACKEND cache var (see src/CMakeLists.txt). Backend-specific
// types MUST NOT leak above the RHI line — engine code depends on the
// abstract IDevice and these neutral enums only.
// ---------------------------------------------------------------------------
enum class Backend : std::uint8_t {
    Null,
    OpenGL,
    Vulkan,
    DirectX12,
};

// Coarse device capability tier (M6 only needs the minimum; backends fill in
// the rest in M7+).
enum class TextureFormat : std::uint8_t {
    RGBA8_UNORM,
    BGRA8_UNORM,
    R8_UNORM,
    RG8_UNORM,
    R16_FLOAT,
    RG16_FLOAT,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    Depth32F,
    Depth24Stencil8,
};

// SWAPCHAIN / framebuffer geometry.
struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend constexpr auto operator==(const Extent& a, const Extent& b) -> bool {
        return a.width == b.width && a.height == b.height;
    }
    friend constexpr auto operator!=(const Extent& a, const Extent& b) -> bool {
        return !(a == b);
    }
};

enum class PresentMode : std::uint8_t {
    Immediate,
    Mailbox,
    Fifo,
};

enum class PrimitiveTopology : std::uint8_t {
    PointList,
    LineList,
    TriangleList,
    TriangleStrip,
};

enum class IndexFormat : std::uint8_t {
    U16,
    U32,
};

[[nodiscard]] constexpr auto indexFormatSize(IndexFormat format) -> std::uint32_t {
    return format == IndexFormat::U16 ? 2u : 4u;
}

enum class CompareOp : std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

enum class CullMode : std::uint8_t { None, Front, Back };

enum class Filter : std::uint8_t { Nearest, Linear };

enum class AddressMode : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge };

enum class TextureFilter : std::uint8_t {
    MinMagNearest,
    MinMagLinear,
    MinLinearMagNearest,
    MinNearestMagLinear,
};

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    SrcColor,
    OneMinusSrcColor,
};

enum class BlendOp : std::uint8_t { Add, Sub, RevSub, Min, Max };

struct ColorWriteMask {
    static constexpr std::uint32_t R = 1u << 0;
    static constexpr std::uint32_t G = 1u << 1;
    static constexpr std::uint32_t B = 1u << 2;
    static constexpr std::uint32_t A = 1u << 3;
    static constexpr std::uint32_t All = R | G | B | A;
};

// Clear color / depth for render-pass attachment clears.
struct ClearValue {
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float depth = 1.0f;
    std::uint32_t stencil = 0;
};

} // namespace f4::gfx
