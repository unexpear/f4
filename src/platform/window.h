#pragma once

#include <cstdint>

#include "core/result.h"

namespace f4::platform {

enum class WindowError : std::uint8_t {
    None,
    SdlInitFailed,
    WindowCreateFailed,
    GlContextCreateFailed,
    BackendUnavailable,
};

[[nodiscard]] constexpr auto toString(WindowError e) -> const char* {
    switch (e) {
    case WindowError::None: return "none";
    case WindowError::SdlInitFailed: return "SDL_Init failed";
    case WindowError::WindowCreateFailed: return "SDL_CreateWindow failed";
    case WindowError::GlContextCreateFailed: return "SDL_GL_CreateContext failed";
    case WindowError::BackendUnavailable: return "backend unavailable";
    }
    return "unknown";
}

// Creation parameters for a Window. `visible == false` yields a hidden window —
// used for the headless software-GL conformance path (M7 A1) where we render to
// an FBO and read pixels back without ever showing a window.
struct WindowDesc {
    const char* title = "f4";
    std::uint32_t width = 800;
    std::uint32_t height = 600;
    bool visible = true;
};

// A wrapped SDL2 window with either an OpenGL context or Vulkan surface support.
// SDL stays hidden behind this seam so graphics backends never depend on SDL
// types directly.
class Window {
public:
    Window() = default;
    ~Window(); // closes SDL resources

    Window(const Window&) = delete;
    auto operator=(const Window&) -> Window& = delete;
    Window(Window&&) = delete;
    auto operator=(Window&&) -> Window& = delete;

    // Opens the window and creates a GL 3.3+ core context. Returns the window
    // if successful (caller owns it and must delete it) or an error.
    static auto create(const WindowDesc& desc) -> Result<Window*, WindowError>;

    // Opens an SDL_WINDOW_VULKAN window without creating an OpenGL context.
    static auto createVulkan(const WindowDesc& desc) -> Result<Window*, WindowError>;

    // Makes the GL context current on the calling thread.
    auto makeCurrent() -> bool;

    // Presents the back buffer (swap). No-op for hidden/offscreen windows.
    void swap();

    // Live framebuffer (drawable) size in pixels — suitable for viewport /
    // projection / framebuffer extent. Recomputed on every call so the window
    // can be resized without rebuilding this object (DPI / OS-reported size).
    [[nodiscard]] auto width() const -> std::uint32_t;
    [[nodiscard]] auto height() const -> std::uint32_t;
    // Live window (logical) size — pixel coordinates delivered by input events
    // (SDL mouse events) are in this space, which may differ from the drawable
    // size on HiDPI displays.
    [[nodiscard]] auto logicalWidth() const -> std::uint32_t;
    [[nodiscard]] auto logicalHeight() const -> std::uint32_t;
    [[nodiscard]] auto glContext() const -> void*;

    // SDL Vulkan bridge using opaque arguments so Vulkan types do not leak into
    // the platform header. `surfaceOut` points to a VkSurfaceKHR.
    [[nodiscard]] auto vulkanInstanceExtensions(std::uint32_t* count,
                                                const char** names) const -> bool;
    [[nodiscard]] auto createVulkanSurface(void* instance, void* surfaceOut) const -> bool;

    // OpenGL VSync control. Vulkan present mode belongs to the swapchain.
    [[nodiscard]] auto vsyncEnabled() const -> bool;
    void setVSyncEnabled(bool enabled);

private:
    static auto createInternal(const WindowDesc& desc, bool vulkan)
        -> Result<Window*, WindowError>;

    void* window_ = nullptr;   // SDL_Window*
    void* glContext_ = nullptr; // SDL_GLContext
    bool vulkan_ = false;
    std::uint32_t width_ = 0;  // fallback size if SDL cannot report a drawable
    std::uint32_t height_ = 0;
};

} // namespace f4::platform
