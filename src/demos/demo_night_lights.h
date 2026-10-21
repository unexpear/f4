#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

// Dark ground lit only by many colored, clustered point lights hovering over
// small emissive lamps, between pillars that catch point-light shadows.
// Every light is flagged castShadows; the renderer shadows the nearest /
// brightest few (the budget) through a cube-face shadow atlas. Keys:
//   [ / ]  halve / double the lamp grid side (8..64 per side, 64..4096 lights)
//   P      toggle point lights            M  toggle the drifting animation
//   B      toggle HDR bloom               - / =  bloom intensity down / up
//   ; / '  bloom threshold down / up
//   K      toggle point-light shadows     , / .  halve / double the shadow
//                                                budget (1..16 lights)
class NightLightsScene : public IDemoScene {
public:
    const char* name() const override { return "Night Lights (clustered point lights)"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width,
                std::uint32_t height) override;
    // Bloom needs the renderer's own render graph (HDR target + post chain),
    // so the scene renders through Renderer::render() instead of recording
    // into the bench's pass.
    bool recordsOwnPasses() const override { return true; }
    void renderPasses(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width,
                      std::uint32_t height, gfx::FramebufferHandle target) override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace f4::demo
