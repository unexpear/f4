#include "demos/demo_bench.h"

#include <cstdio>
#include <cstring>

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

int DemoBench::run(std::size_t smokeFramesPerScene) {
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
