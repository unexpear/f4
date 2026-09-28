#pragma once

#include "demos/demo_bench.h"
#include "engine/rendering/shader_constants.h"

namespace f4::demo {

class PbrSphereGridScene final : public IDemoScene {
public:
    const char* name() const override { return "PBR Spheres: metallic x roughness"; }
    bool prepareStaticCapture(FreeCamera& camera) override;
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;

private:
    gfx::IDevice* device_ = nullptr;
    gfx::BufferHandle vertexBuffer_{};
    gfx::BufferHandle indexBuffer_{};
    gfx::BufferHandle instanceBuffer_{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::PipelineHandle pipeline_{};
    engine::ConstantRing constants_{};
    std::uint32_t indexCount_ = 0;
    bool initialized_ = false;
};

class MipmappingScene final : public IDemoScene {
public:
    const char* name() const override { return "Mipmapping: moire left / trilinear right"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;

private:
    gfx::TextureHandle texture_{};
    gfx::SamplerHandle levelZeroSampler_{};
    gfx::SamplerHandle mipSampler_{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::PipelineHandle pipeline_{};
    bool initialized_ = false;
};

class ZFightingScene final : public IDemoScene {
public:
    const char* name() const override { return "Z-Fighting Stress: separation increases left to right"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;

private:
    gfx::IDevice* device_ = nullptr;
    gfx::BufferHandle vertexBuffer_{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::PipelineHandle pipeline_{};
    engine::ConstantRing constants_{};
    std::uint32_t vertexCount_ = 0;
    bool initialized_ = false;
};

class AlphaOrderScene final : public IDemoScene {
public:
    const char* name() const override { return "Alpha Order: sorted left / reversed right"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;

private:
    gfx::BufferHandle panelBuffer_{};
    gfx::BufferHandle layerBuffers_[6]{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::PipelineHandle opaquePipeline_{};
    gfx::PipelineHandle alphaPipeline_{};
    bool initialized_ = false;
};

class NormalMapOrientationScene final : public IDemoScene {
public:
    const char* name() const override { return "Normal Map: +Y left / flipped-Y right"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;

private:
    gfx::TextureHandle normalTexture_{};
    gfx::SamplerHandle sampler_{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::PipelineHandle pipeline_{};
    bool initialized_ = false;
};

class HybridRenderingScene final : public IDemoScene {
public:
    const char* name() const override { return "Hybrid: deferred opaque + forward transparent"; }
    bool init(gfx::IDevice* device, platform::Window* window) override;
    void shutdown(gfx::IDevice* device) override;
    void onResize(std::uint32_t width, std::uint32_t height) override;
    void update(float dt, const Uint8* keys, const SDL_MouseMotionEvent* mouseMotion) override;
    void render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                std::uint32_t width, std::uint32_t height) override;
    bool recordsOwnPasses() const override { return true; }
    void renderPasses(gfx::CommandBuffer& cb, const FreeCamera& camera,
                      std::uint32_t width, std::uint32_t height,
                      gfx::FramebufferHandle target) override;

private:
    void destroyTargets();

    gfx::IDevice* device_ = nullptr;
    gfx::BufferHandle vertexBuffer_{};
    gfx::BufferHandle indexBuffer_{};
    gfx::BufferHandle layerBuffers_[3]{};
    gfx::ShaderHandle vertexShader_{};
    gfx::ShaderHandle fragmentShader_{};
    gfx::ShaderHandle deferredVertexShader_{};
    gfx::ShaderHandle deferredFragmentShader_{};
    gfx::ShaderHandle transparentVertexShader_{};
    gfx::ShaderHandle transparentFragmentShader_{};
    gfx::PipelineHandle geometryPipeline_{};
    gfx::PipelineHandle deferredPipeline_{};
    gfx::PipelineHandle transparentPipeline_{};
    gfx::TextureHandle albedoTexture_{};
    gfx::TextureHandle normalTexture_{};
    gfx::TextureHandle depthTexture_{};
    gfx::SamplerHandle sampler_{};
    gfx::FramebufferHandle gbuffer_{};
    engine::ConstantRing constants_{};
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    float time_ = 0.0f;
    bool initialized_ = false;
};

} // namespace f4::demo
