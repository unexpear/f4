#pragma once

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class ResizeScene : public IDemoScene {
public:
    const char* name() const override { return "Resize"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    gfx::BufferHandle vbPos_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    engine::ConstantRing constRing_;
    std::uint32_t width_ = 900, height_ = 600;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo