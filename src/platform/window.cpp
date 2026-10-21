#include "platform/window.h"

#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <SDL2/SDL_syswm.h>
#include <windows.h>
#endif
#if defined(F4_PLATFORM_HAS_VULKAN)
#include <SDL2/SDL_vulkan.h>
#endif
#if defined(__APPLE__)
#include <SDL2/SDL_metal.h>
#endif

#include <cstdio>
#include <cstring>

#include "core/assert.h"
#include "core/logging/logger.h"
#include "graphics/gpu_selection.h"

namespace f4::platform {

namespace {

[[nodiscard]] auto toSdlFlags(const WindowDesc& desc, bool openGl, bool vulkan, bool metal)
    -> std::uint32_t {
    std::uint32_t flags = SDL_WINDOW_RESIZABLE;
    if (vulkan) flags |= SDL_WINDOW_VULKAN;
    if (openGl) flags |= SDL_WINDOW_OPENGL;
#if defined(__APPLE__)
    if (metal) flags |= SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    (void)metal;
#endif
    if (!desc.visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

auto glMajor_ = 4;
auto glMinor_ = 3;

// OpenGL cannot enumerate adapters. Hybrid-GPU drivers instead read the
// NvOptimusEnablement / AmdPowerXpressRequestHighPerformance exports of the
// executable (platform/high_performance_gpu.cpp) when their ICD loads, so the
// F4_GPU=integrated override clears them before the first GL window.
void applyOpenGlGpuPreference() {
    static bool applied = false;
    if (applied) return;
    applied = true;
    const gfx::GpuPreference preference = gfx::gpuPreferenceFromEnvironment();
#if defined(_WIN32)
    HMODULE executable = GetModuleHandleW(nullptr);
    const auto exported = [&](const char* symbol) -> void* {
        const FARPROC address = GetProcAddress(executable, symbol);
        void* data = nullptr;
        static_assert(sizeof(address) == sizeof(data));
        std::memcpy(&data, &address, sizeof(data));
        return data;
    };
    auto* nvidia = static_cast<unsigned long*>(exported("NvOptimusEnablement"));
    auto* amd = static_cast<int*>(exported("AmdPowerXpressRequestHighPerformance"));
    if (preference.mode == gfx::GpuPreferenceMode::Integrated) {
        if (nvidia != nullptr) *nvidia = 0;
        if (amd != nullptr) *amd = 0;
    }
    if (nvidia == nullptr && amd == nullptr) {
        F4_LOG_INFO() << "OpenGL GPU hint (F4_GPU=" << gfx::gpuPreferenceName(preference)
                      << "): executable exports no Optimus/PowerXpress hint";
    } else {
        F4_LOG_INFO() << "OpenGL GPU hint (F4_GPU=" << gfx::gpuPreferenceName(preference)
                      << "): NvOptimusEnablement=" << (nvidia != nullptr ? *nvidia : 0ul)
                      << " AmdPowerXpressRequestHighPerformance="
                      << (amd != nullptr ? *amd : 0);
    }
#endif
    if (preference.mode == gfx::GpuPreferenceMode::Index) {
        F4_LOG_WARNING() << "OpenGL cannot select an adapter by index; F4_GPU="
                         << preference.index << " is ignored for OpenGL";
    }
}

} // namespace

auto Window::createInternal(const WindowDesc& desc, Api api)
    -> Result<Window*, WindowError> {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return WindowError::SdlInitFailed;
    }

    if (api == Api::OpenGL) {
        applyOpenGlGpuPreference();
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
        toSdlFlags(desc, api == Api::OpenGL, api == Api::Vulkan, api == Api::Metal));
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

    void* metalView = nullptr;
    if (api == Api::Metal) {
#if defined(__APPLE__)
        metalView = SDL_Metal_CreateView(window);
        if (metalView == nullptr) {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return WindowError::BackendUnavailable;
        }
#else
        SDL_DestroyWindow(window);
        SDL_Quit();
        return WindowError::BackendUnavailable;
#endif
    }

    auto* w = new Window();
    w->window_ = window;
    w->glContext_ = context;
    w->metalView_ = metalView;
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

auto Window::createMetal(const WindowDesc& desc) -> Result<Window*, WindowError> {
#if defined(__APPLE__)
    return createInternal(desc, Api::Metal);
#else
    (void)desc;
    return WindowError::BackendUnavailable;
#endif
}

Window::~Window() {
    if (glContext_ != nullptr) {
        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(glContext_));
    }
#if defined(__APPLE__)
    if (metalView_ != nullptr) {
        SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metalView_));
    }
#endif
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
#if defined(__APPLE__)
    if (api_ == Api::Metal) {
        SDL_Metal_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
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
#if defined(__APPLE__)
    if (api_ == Api::Metal) {
        SDL_Metal_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
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

auto Window::metalLayer() const -> void* {
#if defined(__APPLE__)
    if (api_ != Api::Metal || metalView_ == nullptr) return nullptr;
    return SDL_Metal_GetLayer(static_cast<SDL_MetalView>(metalView_));
#else
    return nullptr;
#endif
}

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
