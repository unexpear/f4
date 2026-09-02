#pragma once

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class TerrainScene : public IDemoScene {
public:
    const char* name() const override { return "Terrain Grid"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    gfx::BufferHandle vbGrid_;
    gfx::BufferHandle ibGrid_;
    gfx::BufferHandle vbCube_;
    gfx::BufferHandle ibCube_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    engine::ConstantRing constRing_;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
    float time_ = 0.f;
    static constexpr int kGridSize = 20;
};

} // namespace f4::demo