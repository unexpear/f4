#pragma once

#include <cstdint>

#include "core/result.h"

namespace f4::platform {

enum class GlError : std::uint8_t {
    None,
    NoContext,
    BackendUnavailable,
    VersionTooLow,
};

// One-time GL context bootstrap on top of libepoxy. Must be called with a GL
// context current. Verifies a desktop GL 3.3+ profile is reachable and returns
// the renderer/version strings for diagnostics.
//
// libepoxy loads the real GL entry points lazily on first call, so this also
// forces the initial dispatch to resolve against the active WGL context — which
// is what makes a hidden (headless) llvmpipe context fully functional.
struct GlInitResult {
    const char* renderer = nullptr;
    const char* version = nullptr;
    int major = 0;
    int minor = 0;
};

[[nodiscard]] auto initGl(std::uint32_t wantMajor, std::uint32_t wantMinor)
    -> Result<GlInitResult, GlError>;

} // namespace f4::platform
