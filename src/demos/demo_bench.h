#pragma once

#include <SDL2/SDL.h>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "core/diagnostics/diagnostics.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "core/math/quat.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::demo {

// Free-fly camera (WASD + mouse look, shift=sprint, space/ctrl=up/down)
struct FreeCamera {
    math::Vec3 position{0.f, 5.f, 8.f};
    math::Vec3 velocity{0.f, 0.f, 0.f};
    float yaw = 1.57079633f;
    float pitch = 0.55f;
    float moveSpeed = 5.f;
    float sprintMultiplier = 3.f;
    float mouseSensitivity = 0.002f;

    [[nodiscard]] math::Mat4 viewMatrix() const {
        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        math::Vec3 forward = {-cy * cp, -sp, -sy * cp};
        math::Vec3 target = position + forward;
        return math::Mat4::lookAtRH(position, target, math::Vec3(0.f, 1.f, 0.f));
    }

    [[nodiscard]] math::Mat4 projMatrix(float aspect, float fovY = 1.04719758f, float zNear = 0.1f, float zFar = 100.f) const {
        return math::Mat4::perspectiveRH_ZO(fovY, aspect, zNear, zFar);
    }

    [[nodiscard]] math::Mat4 viewProjMatrix(float aspect, float fovY = 1.04719758f, float zNear = 0.1f, float zFar = 100.f) const {
        return projMatrix(aspect, fovY, zNear, zFar).mul(viewMatrix());
    }

    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion = nullptr, bool sprint = false) {
        float speed = moveSpeed * (sprint ? sprintMultiplier : 1.f) * dt;

        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        math::Vec3 forward = {-cy * cp, -sp, -sy * cp};
        math::Vec3 right = {-sy, 0.f, cy};
        math::Vec3 up = {0.f, 1.f, 0.f};

        if (keys[SDL_SCANCODE_W]) position += forward * speed;
        if (keys[SDL_SCANCODE_S]) position -= forward * speed;
        if (keys[SDL_SCANCODE_A]) position -= right * speed;
        if (keys[SDL_SCANCODE_D]) position += right * speed;
        if (keys[SDL_SCANCODE_SPACE]) position += up * speed;
        if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) position -= up * speed;

        if (mouseMotion) {
            yaw -= mouseMotion->xrel * mouseSensitivity;
            pitch -= mouseMotion->yrel * mouseSensitivity;
            const float maxPitch = 1.55f;
            if (pitch > maxPitch) pitch = maxPitch;
            if (pitch < -maxPitch) pitch = -maxPitch;
        }
    }
};

// Interface each demo scene implements
struct IDemoScene {
    virtual ~IDemoScene() = default;
    virtual const char* name() const = 0;
    virtual bool init(gfx::IDevice* device, platform::Window* window) = 0;
    virtual void shutdown(gfx::IDevice* device) = 0;
    virtual void onResize(std::uint32_t width, std::uint32_t height) = 0;
    virtual void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion = nullptr) = 0;
    virtual void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) = 0;
    virtual bool recordsOwnPasses() const { return false; }
    virtual void renderPasses(gfx::CommandBuffer& /*cb*/, const FreeCamera& /*camera*/,
                              std::uint32_t /*width*/, std::uint32_t /*height*/,
                              gfx::FramebufferHandle /*target*/) {}
    virtual void renderHud(gfx::CommandBuffer& /*cb*/, std::uint32_t /*width*/, std::uint32_t /*height*/) {}
};

// Demo bench: owns window/device/HUD, manages scene switching
class DemoBench {
public:
    DemoBench(const char* title = "f4 Demo Bench", int width = 900, int height = 600);
    ~DemoBench();

    DemoBench(const DemoBench&) = delete;
    DemoBench& operator=(const DemoBench&) = delete;

    // Register a scene (takes ownership)
    void addScene(IDemoScene* scene);

    // Run interactively, or visit every scene for a bounded number of frames.
    int run(std::size_t smokeFramesPerScene = 0, const char* captureDirectory = nullptr);

    // Switch to scene by index. Keyboard shortcuts cover 0-17; Tab covers all.
    bool setScene(std::size_t index);

    // Get current scene
    IDemoScene* currentScene() const { return (current_ < scenes_.size()) ? scenes_[current_] : nullptr; }
    std::size_t currentIndex() const { return current_; }
    std::size_t sceneCount() const { return scenes_.size(); }

    platform::Window* window() const { return window_.window(); }
    gfx::IDevice* device() const { return window_.device(); }
    const FreeCamera& camera() const { return camera_; }
    FreeCamera& camera() { return camera_; }
    ::demo::Hud& hud() { return hud_; }
    diag::Diagnostics& diagnostics() { return diagnostics_; }

private:
    ::demo::DemoWindow window_;
    ::demo::Hud hud_;
    diag::Diagnostics diagnostics_;
    containers::Array<IDemoScene*> scenes_;
    std::size_t current_ = 0;
    bool hasCurrent_ = false;
    FreeCamera camera_;
    gfx::FramebufferHandle fb_;
    gfx::FramebufferHandle overlayFb_;
    bool mouseCaptured_ = true;
    bool running_ = false;
    Uint64 prevTicks_ = 0;
    Uint64 freq_ = 0;

    // Metric IDs
    std::uint32_t idFps_ = 0;
    std::uint32_t idFrameMs_ = 0;
    std::uint32_t idDraw_ = 0;
    std::uint32_t idTris_ = 0;
    std::uint32_t idMem_ = 0;
    std::uint32_t idScene_ = 0;

    void rebuildFramebuffer();
    void handleEvents();
    void updateMetrics(float dt);
    [[nodiscard]] bool captureBackbuffer(const char* directory,
                                         std::size_t frameInScene) const;
};

} // namespace f4::demo
