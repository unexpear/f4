#pragma once
#include "demos/demo_bench.h"
namespace f4::demo {
class BackfaceScene : public IDemoScene {
public:
    const char* name() const override { return "Backface Cull"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t w, std::uint32_t h) override { (void)w; (void)h; }
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* m) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t width, std::uint32_t height) override;
private:
    gfx::BufferHandle vb_{};
    gfx::BufferHandle ubo_{};
    gfx::PipelineHandle pipeBack_{};
    gfx::PipelineHandle pipeNone_{};
    gfx::ShaderHandle vs_{}, fs_{};
    bool cullBack_ = true;
    bool keyHeld_ = false;
    bool autoRot_ = true;
    bool rotKeyHeld_ = false;
    float angle_ = 0.f;
    gfx::IDevice* dev_ = nullptr;
};
} // namespace f4::demo
