#include "platform/gl_context.h"

#include <cstdio>

#include <epoxy/gl.h>

#include "core/assert.h"

namespace f4::platform {

// Parse "MAJOR.MINOR..." from the GL_VERSION string (e.g. "4.3.0 - Build ...").
[[nodiscard]] auto parseVersion(const char* s, int* major, int* minor) -> bool {
    if (s == nullptr) {
        return false;
    }
    int mj = 0;
    int mn = 0;
    if (std::sscanf(s, "%d.%d", &mj, &mn) != 2) {
        return false;
    }
    *major = mj;
    *minor = mn;
    return true;
}

auto initGl(std::uint32_t wantMajor, std::uint32_t wantMinor)
    -> Result<GlInitResult, GlError> {
    if (epoxy_is_desktop_gl() == 0) {
        return GlError::BackendUnavailable;
    }

    const char* versionStr = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* rendererStr = reinterpret_cast<const char*>(glGetString(GL_RENDERER));

    int major = 0;
    int minor = 0;
    if (!parseVersion(versionStr, &major, &minor)) {
        return GlError::NoContext;
    }
    if (major < static_cast<int>(wantMajor) ||
        (major == static_cast<int>(wantMajor) && minor < static_cast<int>(wantMinor))) {
        return GlError::VersionTooLow;
    }

    GlInitResult result;
    result.renderer = rendererStr;
    result.version = versionStr;
    result.major = major;
    result.minor = minor;
    return result;
}

} // namespace f4::platform
