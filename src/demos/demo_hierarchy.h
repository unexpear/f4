#pragma once
#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"
#include "engine/rendering/world_grid.h"
namespace f4::demo {
class HierarchyScene : public IDemoScene {
public:
    const char* name() const override { return "Planet & Moon"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t w, std::uint32_t h) override { (void)w; (void)h; }
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* m) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t width, std::uint32_t height) override;
private:
    // Sphere mesh for the sun/earth/moon hierarchy. Each body gets its own UBO slot
    // via ConstantRing (correct under deferred submits on any backend).
    gfx::BufferHandle vbSphere_{}, ibSphere_{};
    gfx::PipelineHandle pipeSphere_{};
    gfx::ShaderHandle vs_{}, fs_{};
    std::uint32_t sphereIndexCount_ = 0;
    engine::ConstantRing constRing_;
    engine::WorldGrid grid_;
    gfx::IDevice* dev_ = nullptr;
    float time_ = 0.f;
    bool showGrid_ = false;   // grid hidden by default; G toggles it on
    bool gHeld_ = false;
    bool sHeld_ = false;      // scale grid up
    bool tHeld_ = false;      // scale grid down
    float gridExtent_ = 5.0f;
    std::uint32_t gridDivisions_ = 10;
    // ECS hierarchy to demonstrate local->world
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace f4::demo
