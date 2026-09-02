#include "platform/window.h"

#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <SDL2/SDL_syswm.h>
#endif
#if defined(F4_PLATFORM_HAS_VULKAN)
#include <SDL2/SDL_vulkan.h>
#endif

#include <cstdio>

#include "core/assert.h"

namespace f4::platform {

namespace {

[[nodiscard]] auto toSdlFlags(const WindowDesc& desc, bool openGl, bool vulkan)
    -> std::uint32_t {
    std::uint32_t flags = SDL_WINDOW_RESIZABLE;
    if (vulkan) flags |= SDL_WINDOW_VULKAN;
    if (openGl) flags |= SDL_WINDOW_OPENGL;
    if (!desc.visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

auto glMajor_ = 4;
auto glMinor_ = 3;

} // namespace

auto Window::createInternal(const WindowDesc& desc, Api api)
    -> Result<Window*, WindowError> {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return WindowError::SdlInitFailed;
    }

    if (api == Api::OpenGL) {
        // Request a core-profile GL 4.3 context (vertex-attrib-binding API +
        // implicit FBOs + glReadPixels for the headless conformance path).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, glMajor_);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, glMinor_);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    SDL_Window* window = SDL_CreateWindow(
        desc.title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        static_cast<int>(desc.width),
        static_cast<int>(desc.height),
        toSdlFlags(desc, api == Api::OpenGL, api == Api::Vulkan));
    if (window == nullptr) {
        SDL_Quit();
        return WindowError::WindowCreateFailed;
    }

    SDL_GLContext context = nullptr;
    if (api == Api::OpenGL) {
        context = SDL_GL_CreateContext(window);
        if (context == nullptr) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return WindowError::GlContextCreateFailed;
        }
        SDL_GL_SetSwapInterval(1); // vsync
    }

    auto* w = new Window();
    w->window_ = window;
    w->glContext_ = context;
    w->api_ = api;
    w->width_ = desc.width;
    w->height_ = desc.height;
    return w;
}

auto Window::create(const WindowDesc& desc) -> Result<Window*, WindowError> {
    return createInternal(desc, Api::OpenGL);
}

auto Window::createVulkan(const WindowDesc& desc) -> Result<Window*, WindowError> {
#if defined(F4_PLATFORM_HAS_VULKAN)
    return createInternal(desc, Api::Vulkan);
#else
    (void)desc;
    return WindowError::BackendUnavailable;
#endif
}

auto Window::createNative(const WindowDesc& desc) -> Result<Window*, WindowError> {
    return createInternal(desc, Api::Native);
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
    if (window_ != nullptr && api_ == Api::OpenGL) {
        SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_));
    }
}

auto Window::vsyncEnabled() const -> bool {
    return glContext_ != nullptr && api_ == Api::OpenGL && SDL_GL_GetSwapInterval() != 0;
}

void Window::setVSyncEnabled(bool enabled) {
    if (glContext_ != nullptr && api_ == Api::OpenGL) {
        (void)SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    }
}

auto Window::width() const -> std::uint32_t {
    if (window_ == nullptr) return width_;
    int w = 0, h = 0;
#if defined(F4_PLATFORM_HAS_VULKAN)
    if (api_ == Api::Vulkan) {
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
    } else
#endif
    {
        if (api_ == Api::OpenGL) {
            SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
        } else {
            SDL_GetWindowSize(static_cast<SDL_Window*>(window_), &w, &h);
        }
    }
    return w > 0 ? static_cast<std::uint32_t>(w) : width_;
}
auto Window::height() const -> std::uint32_t {
    if (window_ == nullptr) return height_;
    int w = 0, h = 0;
#if defined(F4_PLATFORM_HAS_VULKAN)
    if (api_ == Api::Vulkan) {
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
    } else
#endif
    {
        if (api_ == Api::OpenGL) {
            SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
        } else {
            SDL_GetWindowSize(static_cast<SDL_Window*>(window_), &w, &h);
        }
    }
    return h > 0 ? static_cast<std::uint32_t>(h) : height_;
}
auto Window::logicalWidth() const -> std::uint32_t {
    if (window_ == nullptr) return width_;
    int w = 0, h = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(window_), &w, &h);
    return w > 0 ? static_cast<std::uint32_t>(w) : width_;
}
auto Window::logicalHeight() const -> std::uint32_t {
    if (window_ == nullptr) return height_;
    int w = 0, h = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(window_), &w, &h);
    return h > 0 ? static_cast<std::uint32_t>(h) : height_;
}
auto Window::glContext() const -> void* { return glContext_; }

auto Window::nativeHandle() const -> void* {
#if defined(_WIN32)
    if (window_ == nullptr) return nullptr;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(static_cast<SDL_Window*>(window_), &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        return nullptr;
    }
    return info.info.win.window;
#else
    return nullptr;
#endif
}

auto Window::vulkanInstanceExtensions(std::uint32_t* count, const char** names) const -> bool {
#if defined(F4_PLATFORM_HAS_VULKAN)
    if (api_ != Api::Vulkan || window_ == nullptr || count == nullptr) return false;
    unsigned int extensionCount = *count;
    const SDL_bool ok = SDL_Vulkan_GetInstanceExtensions(
        static_cast<SDL_Window*>(window_), &extensionCount, names);
    *count = extensionCount;
    return ok == SDL_TRUE;
#else
    (void)count;
    (void)names;
    return false;
#endif
}

auto Window::createVulkanSurface(void* instance, void* surfaceOut) const -> bool {
#if defined(F4_PLATFORM_HAS_VULKAN)
    if (api_ != Api::Vulkan || window_ == nullptr || instance == nullptr || surfaceOut == nullptr) {
        return false;
    }
    return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window_),
                                    static_cast<VkInstance>(instance),
                                    static_cast<VkSurfaceKHR*>(surfaceOut)) == SDL_TRUE;
#else
    (void)instance;
    (void)surfaceOut;
    return false;
#endif
}

} // namespace f4::platform
