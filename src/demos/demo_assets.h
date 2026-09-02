#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

class AssetsScene : public IDemoScene {
public:
    const char* name() const override { return "Assets"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    static constexpr std::uint32_t kTexW = 128;
    static constexpr std::uint32_t kTexH = 128;
    gfx::BufferHandle vbQuad_;
    gfx::BufferHandle ibQuad_;
    gfx::TextureHandle tex_;
    gfx::SamplerHandle samp_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo