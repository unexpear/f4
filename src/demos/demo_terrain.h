#pragma once

#include <array>

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class TerrainScene : public IDemoScene {
public:
    const char* name() const override { return "M15 Terrain Meshers"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) override;

private:
    struct TerrainGpuMesh {
        gfx::BufferHandle vertexBuffer{};
        gfx::BufferHandle indexBuffer{};
        gfx::IndexFormat indexFormat = gfx::IndexFormat::U16;
        std::uint32_t indexCount = 0;
        math::Mat4 model = math::Mat4::identity();
    };

    std::array<TerrainGpuMesh, 3> meshes_{};
    gfx::PipelineHandle pipeline_;
    gfx::ShaderHandle vs_;
    gfx::ShaderHandle fs_;
    engine::ConstantRing constRing_;
    bool inited_ = false;
    gfx::IDevice* device_ = nullptr;
};

} // namespace f4::demo
