#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

// Dark ground lit only by many colored, clustered point lights hovering over
// small emissive lamps. Keys: [ / ] halve / double the lamp grid side
// (8..64 per side, i.e. 64..4096 lights), P toggles point lights, M toggles
// the drifting animation.
class NightLightsScene : public IDemoScene {
public:
    const char* name() const override { return "Night Lights (clustered point lights)"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width,
                std::uint32_t height) override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace f4::demo
