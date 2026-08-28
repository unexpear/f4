#include "platform/window.h"

#include <SDL2/SDL.h>

#include <cstdio>

#include "core/assert.h"

namespace f4::platform {

namespace {

[[nodiscard]] auto toSdlFlags(const WindowDesc& desc) -> std::uint32_t {
    std::uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (!desc.visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

auto glMajor_ = 4;
auto glMinor_ = 3;

} // namespace

auto Window::create(const WindowDesc& desc) -> Result<Window*, WindowError> {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return WindowError::SdlInitFailed;
    }

    // Request a core-profile GL 4.3 context (vertex-attrib-binding API +
    // implicit FBOs + glReadPixels for the headless conformance path).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, glMajor_);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, glMinor_);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        desc.title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        static_cast<int>(desc.width),
        static_cast<int>(desc.height),
        toSdlFlags(desc));
    if (window == nullptr) {
        SDL_Quit();
        return WindowError::WindowCreateFailed;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return WindowError::GlContextCreateFailed;
    }
    SDL_GL_SetSwapInterval(1); // vsync

    auto* w = new Window();
    w->window_ = window;
    w->glContext_ = context;
    w->width_ = desc.width;
    w->height_ = desc.height;
    return w;
}

Window::~Window() {
    if (glContext_ != nullptr) {
        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(glContext_));
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
    }
    SDL_Quit();
}

auto Window::makeCurrent() -> bool {
    if (glContext_ == nullptr) {
        return false;
    }
    return SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_),
                              static_cast<SDL_GLContext>(glContext_)) == 0;
}

void Window::swap() {
    if (window_ != nullptr) {
        SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_));
    }
}

auto Window::width() const -> std::uint32_t { return width_; }
auto Window::height() const -> std::uint32_t { return height_; }
auto Window::glContext() const -> void* { return glContext_; }

} // namespace f4::platform
