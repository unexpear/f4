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

// A wrapped SDL2 window with an OpenGL (WGL) context. Owns both. SDL is
// completely hidden behind this seam so GL/Vulkan/DX12 backends never see SDL.
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

    // Makes the GL context current on the calling thread.
    auto makeCurrent() -> bool;

    // Presents the back buffer (swap). No-op for hidden/offscreen windows.
    void swap();

    [[nodiscard]] auto width() const -> std::uint32_t;
    [[nodiscard]] auto height() const -> std::uint32_t;
    [[nodiscard]] auto glContext() const -> void*;

private:
    void* window_ = nullptr;   // SDL_Window*
    void* glContext_ = nullptr; // SDL_GLContext
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace f4::platform
