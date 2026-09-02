#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

class TriangleScene : public IDemoScene {
public:
    const char* name() const override { return "Triangle"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    gfx::BufferHandle vbPos_;
    gfx::BufferHandle vbColor_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    bool inited_ = false;
};

} // namespace f4::demo