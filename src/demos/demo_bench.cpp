#include "demos/demo_bench.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include <epoxy/gl.h>
#include "backends/opengl/opengl_device.h"
#include "core/diagnostics/diagnostics.h"
#include "graphics/hud/hud_overlay.h"
#include "platform/gl_context.h"
#include "platform/window.h"
#include "SDL2/SDL.h"

namespace f4::demo {

DemoBench::DemoBench(const char* title, int width, int height)
    : window_(title, width, height, true), hud_(window_.device()), diagnostics_() {
    freq_ = SDL_GetPerformanceFrequency();
    prevTicks_ = SDL_GetPerformanceCounter();

    hud_.init();
    diagnostics_.setSampleSink(&::demo::Hud::onSample, &hud_);

    idFps_ = diagnostics_.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    idFrameMs_ = diagnostics_.metrics().declare({"frame_ms", diag::MetricType::Double, "frame", "ms"});
    idDraw_ = diagnostics_.metrics().declare({"draw_calls", diag::MetricType::UInt, "gpu", "calls"});
    idTris_ = diagnostics_.metrics().declare({"triangles", diag::MetricType::UInt, "gpu", "count"});
    idMem_ = diagnostics_.metrics().declare({"live_bytes", diag::MetricType::UInt, "memory", "B"});
    idScene_ = diagnostics_.metrics().declare({"scene", diag::MetricType::String, "telemetry", nullptr});

    rebuildFramebuffer();
}

DemoBench::~DemoBench() {
    for (auto* s : scenes_) {
        if (s) s->shutdown(window_.device());
        delete s;
    }
    scenes_.clear();
}

void DemoBench::addScene(IDemoScene* scene) {
    scenes_.push(scene);
}

bool DemoBench::setScene(std::size_t index) {
    if (index >= scenes_.size()) return false;
    if (current_ < scenes_.size() && scenes_[current_]) {
        scenes_[current_]->shutdown(window_.device());
    }
    current_ = index;
    hasCurrent_ = true;
    if (scenes_[current_]) {
        if (!scenes_[current_]->init(window_.device(), window_.window())) {
            return false;
        }
        scenes_[current_]->onResize(window_.window()->width(), window_.window()->height());
    }
    diagnostics_.metrics()[idScene_].set(scenes_[current_] ? scenes_[current_]->name() : "none");
    ::demo::Hud::onSample(diagnostics_.metrics(), &hud_);
    return true;
}

void DemoBench::rebuildFramebuffer() {
    if (fb_.isValid()) {
        window_.device()->destroyFramebuffer(fb_);
    }
    if (overlayFb_.isValid()) {
        window_.device()->destroyFramebuffer(overlayFb_);
    }
    gfx::FramebufferDesc fbd;
    fbd.extent.width = static_cast<std::uint32_t>(window_.window()->width());
    fbd.extent.height = static_cast<std::uint32_t>(window_.window()->height());
    gfx::AttachmentDesc color;
    color.type = gfx::AttachmentType::Color;
    color.clearOnLoad = true;
    // Dark slate background: keeps dark-background demos (particles/backface) readable
    // while still letting mid-gray geometry stand out from the void.
    color.clear.color[0] = 0.16f; color.clear.color[1] = 0.16f; color.clear.color[2] = 0.22f; color.clear.color[3] = 1.f;
    fbd.attachments.push(color);
    gfx::AttachmentDesc depth;
    depth.type = gfx::AttachmentType::Depth;
    depth.clearOnLoad = true;
    depth.clear.depth = 1.f;
    fbd.attachments.push(depth);
    auto fbRes = window_.device()->createFramebuffer(fbd);
    if (fbRes.hasValue()) fb_ = fbRes.value();

    gfx::FramebufferDesc overlayDesc;
    overlayDesc.extent = fbd.extent;
    gfx::AttachmentDesc overlayColor;
    overlayColor.type = gfx::AttachmentType::Color;
    overlayColor.clearOnLoad = false;
    overlayDesc.attachments.push(overlayColor);
    gfx::AttachmentDesc overlayDepth;
    overlayDepth.type = gfx::AttachmentType::Depth;
    overlayDepth.clearOnLoad = false;
    overlayDesc.attachments.push(overlayDepth);
    auto overlayRes = window_.device()->createFramebuffer(overlayDesc);
    if (overlayRes.hasValue()) overlayFb_ = overlayRes.value();
}

void DemoBench::handleEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            running_ = false;
            return;
        }
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) {
            running_ = false;
            return;
        }
        if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE) {
                if (mouseCaptured_) {
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    mouseCaptured_ = false;
                } else {
                    running_ = false;
                    return;
                }
            }
            // 1-9 = scenes 0-8, Shift+1-9 = scenes 9-17
            if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_9) {
                std::size_t idx = static_cast<std::size_t>(ev.key.keysym.sym - SDLK_1);
                bool shift = (ev.key.keysym.mod & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
                if (shift) idx += 9;
                if (idx < scenes_.size()) {
                    setScene(idx);
                }
            }
            if (ev.key.keysym.sym == SDLK_TAB) {
                // Cycle scenes
                if (!scenes_.isEmpty()) {
                    setScene((current_ + 1) % scenes_.size());
                }
            }
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            if (!mouseCaptured_) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                mouseCaptured_ = true;
            }
        }
        if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                rebuildFramebuffer();
                if (current_ < scenes_.size() && scenes_[current_]) {
                    scenes_[current_]->onResize(static_cast<std::uint32_t>(ev.window.data1),
                                               static_cast<std::uint32_t>(ev.window.data2));
                }
            }
        }
    }
}

void DemoBench::updateMetrics(float dt) {
    diagnostics_.metrics()[idFps_].set(dt > 0 ? 1.0 / dt : 0.0);
    diagnostics_.metrics()[idFrameMs_].set(dt * 1000.0);
    if (auto* dev = window_.device()) {
        auto st = dev->stats();
        diagnostics_.metrics()[idDraw_].set(st.drawCalls + st.drawIndexedCalls);
        diagnostics_.metrics()[idTris_].set(st.primitives);
        diagnostics_.metrics()[idMem_].set(st.bufferBytes + st.textureBytes);
        dev->resetFrameStats();
    }
    if (current_ < scenes_.size() && scenes_[current_]) {
        diagnostics_.metrics()[idScene_].set(scenes_[current_]->name());
    }
}

bool DemoBench::captureBackbuffer(const char* directory,
                                  std::size_t frameInScene) const {
    if (directory == nullptr || currentScene() == nullptr) return true;
    const int width = window_.window()->width();
    const int height = window_.window()->height();
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() / 4) {
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError) {
        std::fprintf(stderr, "Capture directory error: %s\n",
                     filesystemError.message().c_str());
        return false;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    if (static_cast<std::size_t>(height) >
        std::numeric_limits<std::size_t>::max() / rowBytes) {
        return false;
    }
    containers::Array<std::uint8_t> pixels;
    pixels.resize(rowBytes * static_cast<std::size_t>(height));
    GLint previousPackAlignment = 4;
    GLint previousReadBuffer = GL_BACK;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
    while (glGetError() != GL_NO_ERROR) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const GLenum readError = glGetError();
    glReadBuffer(static_cast<GLenum>(previousReadBuffer));
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    if (readError != GL_NO_ERROR) {
        std::fprintf(stderr, "OpenGL capture failed: 0x%x\n",
                     static_cast<unsigned>(readError));
        return false;
    }

    for (int y = 0; y < height / 2; ++y) {
        const std::size_t top = static_cast<std::size_t>(y) * rowBytes;
        const std::size_t bottom = static_cast<std::size_t>(height - y - 1) * rowBytes;
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::uint8_t value = pixels[top + x];
            pixels[top + x] = pixels[bottom + x];
            pixels[bottom + x] = value;
        }
    }

    std::string sceneName = currentScene()->name();
    for (char& character : sceneName) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value)) character = '_';
    }
    char fileName[256]{};
    std::snprintf(fileName, sizeof(fileName), "%02zu_%s_%03zu.bmp",
                  current_, sceneName.c_str(), frameInScene);
    const std::filesystem::path path = std::filesystem::path(directory) / fileName;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels.data(), width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) {
        std::fprintf(stderr, "SDL capture surface failed: %s\n", SDL_GetError());
        return false;
    }
    const int saveResult = SDL_SaveBMP(surface, path.string().c_str());
    SDL_FreeSurface(surface);
    if (saveResult != 0) {
        std::fprintf(stderr, "SDL capture write failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

int DemoBench::run(std::size_t smokeFramesPerScene, const char* captureDirectory) {
    if (!window_.ok()) return 1;

    // Initialize first scene (respect pre-selected scene from main)
    if (!hasCurrent_ && !scenes_.isEmpty() && !setScene(0)) return 1;

    // Capture mouse initially
    SDL_SetRelativeMouseMode(SDL_TRUE);
    mouseCaptured_ = true;
    running_ = true;
    std::size_t sceneFrame = 0;

    if (smokeFramesPerScene > 0 && currentScene()) {
        std::printf("Smoke testing scene %zu/%zu: %s\n", current_ + 1, scenes_.size(),
                    currentScene()->name());
    }

    while (running_) {
        handleEvents();
        if (!running_) break;

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(static_cast<double>(now - prevTicks_) / static_cast<double>(freq_));
        prevTicks_ = now;
        if (dt <= 0.f) dt = 1.f / 60.f;

        const Uint8* keys = SDL_GetKeyboardState(nullptr);

        // Update current scene
        if (current_ < scenes_.size() && scenes_[current_]) {
            scenes_[current_]->update(dt, keys, nullptr);
        }

        // Update camera
        bool sprint = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
        camera_.update(dt, keys, nullptr, sprint);

        updateMetrics(dt);
        diagnostics_.update(static_cast<double>(now) / static_cast<double>(freq_));

        // Render
        gfx::CommandBuffer cb;
        cb.begin();
        gfx::Extent vp;
        vp.width = static_cast<std::uint32_t>(window_.window()->width());
        vp.height = static_cast<std::uint32_t>(window_.window()->height());
        IDemoScene* scene = currentScene();
        if (scene != nullptr && scene->recordsOwnPasses()) {
            scene->renderPasses(cb, camera_, vp.width, vp.height, fb_);
            cb.beginRenderPass(overlayFb_);
            cb.setViewport(vp);
            hud_.render(cb, vp.width, vp.height);
            scene->renderHud(cb, vp.width, vp.height);
            cb.endRenderPass();
        } else {
            cb.beginRenderPass(fb_);
            cb.setViewport(vp);
            if (scene != nullptr) {
                scene->render(cb, camera_, vp.width, vp.height);
            }
            hud_.render(cb, vp.width, vp.height);
            if (scene != nullptr) {
                scene->renderHud(cb, vp.width, vp.height);
            }
            cb.endRenderPass();
        }
        cb.end();
        if (auto err = window_.device()->submit(cb); err != gfx::GfxError::None) {
            std::fprintf(stderr, "Submit error: %d\n", static_cast<int>(err));
            return 1;
        }

        if (!captureBackbuffer(captureDirectory, sceneFrame)) return 1;

        window_.window()->swap();

        if (smokeFramesPerScene > 0 && ++sceneFrame >= smokeFramesPerScene) {
            sceneFrame = 0;
            if (current_ + 1 >= scenes_.size()) {
                running_ = false;
            } else if (!setScene(current_ + 1)) {
                std::fprintf(stderr, "Failed to initialize scene %zu\n", current_ + 1);
                return 1;
            } else {
                std::printf("Smoke testing scene %zu/%zu: %s\n", current_ + 1,
                            scenes_.size(), currentScene()->name());
            }
        }
    }

    window_.device()->waitIdle();
    return 0;
}

} // namespace f4::demo
