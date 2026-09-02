#pragma once

#include "demos/demo_bench.h"

namespace f4 {

class Registry;
class IDevice;

namespace ecs {
class Registry;
}

namespace engine {
class Renderer;
}

} // namespace f4

namespace f4::demo {

class EcsScene : public IDemoScene {
public:
    const char* name() const override { return "ECS Renderer"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    // Forward declare to avoid including heavy headers in .h
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace f4::demo