#pragma once

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class ParticleScene : public IDemoScene {
public:
    const char* name() const override { return "Particles"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    void spawnParticle(int i);
    static constexpr int kMaxParticles = 1000;
    struct Particle { math::Vec3 pos, vel; float life, maxLife; float size; math::Vec3 color; };

    gfx::BufferHandle vbQuad_;
    gfx::BufferHandle ibQuad_;
    gfx::BufferHandle vbInstance_;
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    engine::ConstantRing constRing_;
    Particle particles_[kMaxParticles];
    int aliveCount_ = 0;
    float spawnTimer_ = 0.f;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo
