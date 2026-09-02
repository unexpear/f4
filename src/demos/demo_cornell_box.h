#pragma once

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class CornellBoxScene : public IDemoScene {
public:
    const char* name() const override { return "Cornell Box"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    gfx::BufferHandle vb_;
    gfx::BufferHandle ib_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    engine::ConstantRing constRing_;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
    std::uint32_t indexCount_ = 0;
};

} // namespace f4::demo
