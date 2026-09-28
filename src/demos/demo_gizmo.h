#pragma once
#include "demos/demo_bench.h"
namespace f4::demo {
class GizmoScene : public IDemoScene {
public:
    const char* name() const override { return "Gizmo RH/LH"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t w, std::uint32_t h) override { (void)w; (void)h; }
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* m) override;
    bool prepareStaticCapture(FreeCamera& camera) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t width, std::uint32_t height) override;
private:
    gfx::BufferHandle vbCube_{}, ibCube_{}, vbArrow_{}, ibArrow_{}, ubo_{};
    gfx::PipelineHandle pipe_{};
    gfx::PipelineHandle mirroredPipe_{};
    gfx::ShaderHandle vs_{}, fs_{};
    bool isRH_ = true;
    bool hHeld_ = false;
    float angle_ = 0.f;
    gfx::IDevice* dev_ = nullptr;
};
} // namespace f4::demo
