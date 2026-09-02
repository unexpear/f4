#pragma once

#include "demos/demo_bench.h"

namespace f4::demo {

class CollisionScene : public IDemoScene {
public:
    const char* name() const override { return "Collision"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    static constexpr int kBodies = 8;
    struct Body { float x, y, vx, vy, r, g, b, size; };
    struct Instance { float x, y, size, r, g, b; };
    gfx::BufferHandle vbQuad_;
    gfx::BufferHandle vbInstance_;
    gfx::BufferHandle ibQuad_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    Body bodies_[kBodies];
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo