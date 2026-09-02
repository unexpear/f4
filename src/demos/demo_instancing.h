#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

class InstancingScene : public IDemoScene {
public:
    const char* name() const override { return "Instancing"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    static constexpr int kGrid = 10;
    static constexpr int kInstanceCount = kGrid * kGrid;
    gfx::BufferHandle vbPos_;
    gfx::BufferHandle vbInstance_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    float time_ = 0.f;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo