#pragma once

#include <cstdint>

namespace f4::gfx {

// Shared device error codes. No exceptions past the RHI seam (M5 discipline).
enum class GfxError : std::uint8_t {
    None,
    InvalidHandle,
    StaleHandle,
    BadDescriptor,
    OutOfMemory,
    BackendUnavailable,
    DeviceLost,
};

} // namespace f4::gfx
