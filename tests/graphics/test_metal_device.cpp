#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

#include "backends/metal/metal_device.h"
#include "graphics/command/command_buffer.h"
#include "platform/window.h"

using namespace f4;

namespace {

auto loadShader(const char* path, gfx::ShaderStage stage) -> gfx::ShaderDesc {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.good());
    const std::streamsize size = stream.tellg();
    REQUIRE(size > 0);
    stream.seekg(0);
    gfx::ShaderDesc desc;
    desc.stage = stage;
    desc.blob.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(desc.blob.data()), size);
    REQUIRE(stream.good());
    return desc;
}

} // namespace

TEST_CASE("Metal hardware: native device exposes a command queue") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    CHECK(device->backend() == gfx::Backend::Metal);
    CHECK(device->deviceName()[0] != '\0');
    CHECK(device->framesInFlight() == 3);
    CHECK_FALSE(device->hasWindow());
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Metal hardware: resource readback and retirement preserve bytes") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);

    gfx::BufferDesc bufferDesc;
    bufferDesc.sizeBytes = 64;
    bufferDesc.usage = gfx::BufferUsage::Uniform;
    std::array<std::byte, 16> initial{};
    for (std::size_t index = 0; index < initial.size(); ++index) {
        initial[index] = static_cast<std::byte>(index + 1);
    }
    auto buffer = device->createBuffer(bufferDesc, initial);
    REQUIRE(buffer.hasValue());
    std::array<std::byte, 16> bufferReadback{};
    REQUIRE(device->readBuffer(buffer.value(), 0, bufferReadback) == gfx::GfxError::None);
    CHECK(bufferReadback == initial);
    CHECK(device->writeBuffer(buffer.value(), 56, initial) == gfx::GfxError::BadDescriptor);

    gfx::TextureDesc textureDesc;
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.mipLevels = 2;
    auto texture = device->createTexture(textureDesc);
    REQUIRE(texture.hasValue());
    std::array<std::byte, 64> mip0{};
    std::array<std::byte, 16> mip1{};
    for (std::size_t index = 0; index < mip0.size(); ++index) {
        mip0[index] = static_cast<std::byte>(index);
    }
    REQUIRE(device->writeTexture(texture.value(), 0, mip0) == gfx::GfxError::None);
    REQUIRE(device->writeTexture(texture.value(), 1, mip1) == gfx::GfxError::None);
    std::array<std::byte, 64> textureReadback{};
    REQUIRE(device->readTexture(texture.value(), 0, textureReadback) == gfx::GfxError::None);
    CHECK(textureReadback == mip0);

    auto sampler = device->createSampler(gfx::SamplerDesc{});
    REQUIRE(sampler.hasValue());
    CHECK(device->stats().bufferCount == 1);
    CHECK(device->stats().textureBytes == 80);
    CHECK(device->destroySampler(sampler.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(buffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(buffer.value()) == gfx::GfxError::StaleHandle);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

#if defined(F4_METAL_TEST_BIND_SHADER)
TEST_CASE("Metal hardware: 130 resource-bound draws render without a fixed ceiling") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    auto vertex = device->createShader(
        loadShader(F4_METAL_TEST_BIND_SHADER, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_METAL_TEST_BIND_SHADER, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());

    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::BindGroup group;
    group.slots.push({0, gfx::BindingType::UniformBuffer,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    group.slots.push({0, gfx::BindingType::Texture,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    group.slots.push({0, gfx::BindingType::Sampler,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    pipelineDesc.layout.groups.push(group);
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    gfx::TextureDesc sourceDesc;
    sourceDesc.width = 2;
    sourceDesc.height = 2;
    auto source = device->createTexture(sourceDesc);
    REQUIRE(source.hasValue());
    std::array<std::byte, 16> texels{};
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        texels[pixel * 4 + 0] = std::byte{32};
        texels[pixel * 4 + 1] = std::byte{160};
        texels[pixel * 4 + 2] = std::byte{224};
        texels[pixel * 4 + 3] = std::byte{255};
    }
    REQUIRE(device->writeTexture(source.value(), 0, texels) == gfx::GfxError::None);
    auto sampler = device->createSampler(gfx::SamplerDesc{});
    REQUIRE(sampler.hasValue());

    constexpr std::size_t drawCount = 130;
    const std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    gfx::BufferDesc uniformDesc;
    uniformDesc.sizeBytes = sizeof(tint);
    uniformDesc.usage = gfx::BufferUsage::Uniform;
    std::vector<gfx::BufferHandle> uniforms;
    uniforms.reserve(drawCount);
    for (std::size_t draw = 0; draw < drawCount; ++draw) {
        auto uniform = device->createBuffer(uniformDesc, std::as_bytes(std::span(tint)));
        REQUIRE(uniform.hasValue());
        uniforms.push_back(uniform.value());
    }

    gfx::TextureDesc targetDesc;
    targetDesc.width = 32;
    targetDesc.height = 32;
    auto target = device->createTexture(targetDesc);
    REQUIRE(target.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {32, 32};
    gfx::AttachmentDesc color;
    color.texture = target.value();
    color.clearOnLoad = true;
    color.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(color);
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    gfx::CommandBuffer commands;
    commands.begin();
    commands.beginRenderPass(framebuffer.value());
    commands.bindPipeline(pipeline.value());
    commands.bindResource(0, gfx::BindingType::Texture,
                          source.value().index, source.value().generation);
    commands.bindResource(0, gfx::BindingType::Sampler,
                          sampler.value().index, sampler.value().generation);
    for (const gfx::BufferHandle uniform : uniforms) {
        commands.bindResource(0, gfx::BindingType::UniformBuffer,
                              uniform.index, uniform.generation);
        commands.draw(3);
    }
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    for (const gfx::BufferHandle uniform : uniforms) {
        REQUIRE(device->destroyBuffer(uniform) == gfx::GfxError::None);
    }
    REQUIRE(device->destroySampler(sampler.value()) == gfx::GfxError::None);
    REQUIRE(device->destroyTexture(source.value()) == gfx::GfxError::None);

    std::array<std::byte, 32 * 32 * 4> pixels{};
    REQUIRE(device->readTexture(target.value(), 0, pixels) == gfx::GfxError::None);
    const std::size_t center = (16 * 32 + 16) * 4;
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 0]) == 32);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 1]) == 160);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 2]) == 224);
    CHECK(device->stats().drawCalls == drawCount);
    CHECK(device->validationErrorCount() == 0);

    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(target.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragment.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertex.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
}
#endif

#if defined(F4_METAL_TEST_TRIANGLE_SHADER)
TEST_CASE("Metal hardware: SDL CAMetalLayer presents and recreates") {
    auto windowResult = platform::Window::createMetal({"Metal swapchain test", 96, 64, true});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    gfx::IDevice* base = gfx::createMetalDevice(window);
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    CHECK(device->hasWindow());
    auto vertex = device->createShader(
        loadShader(F4_METAL_TEST_TRIANGLE_SHADER, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_METAL_TEST_TRIANGLE_SHADER, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::ColorAttachmentState colorState;
    colorState.format = device->preferredSurfaceFormat();
    pipelineDesc.colorAttachments.push(colorState);
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    for (std::size_t recreation = 0; recreation < 2; ++recreation) {
        gfx::SwapchainDesc swapchainDesc;
        swapchainDesc.surfaceFormat = device->preferredSurfaceFormat();
        swapchainDesc.extent = {
            window->width() + static_cast<std::uint32_t>(recreation * 32),
            window->height() + static_cast<std::uint32_t>(recreation * 16)};
        auto swapchain = device->createSwapchain(swapchainDesc);
        REQUIRE(swapchain.hasValue());
        gfx::FramebufferDesc mismatchedDesc;
        mismatchedDesc.extent = {swapchainDesc.extent.width + 1,
                                 swapchainDesc.extent.height};
        mismatchedDesc.attachments.push(gfx::AttachmentDesc{});
        CHECK_FALSE(device->createFramebuffer(mismatchedDesc).hasValue());
        gfx::FramebufferDesc framebufferDesc;
        framebufferDesc.extent = swapchainDesc.extent;
        gfx::AttachmentDesc color;
        color.clearOnLoad = true;
        framebufferDesc.attachments.push(color);
        auto framebuffer = device->createFramebuffer(framebufferDesc);
        REQUIRE(framebuffer.hasValue());
        for (std::size_t frame = 0; frame < 3; ++frame) {
            gfx::CommandBuffer commands;
            commands.begin();
            commands.beginRenderPass(framebuffer.value());
            commands.bindPipeline(pipeline.value());
            commands.draw(3);
            commands.endRenderPass();
            commands.end();
            REQUIRE(device->submit(commands) == gfx::GfxError::None);
        }
        device->waitIdle();
        CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
        CHECK(device->destroySwapchain(swapchain.value()) == gfx::GfxError::None);
    }
    CHECK(device->validationErrorCount() == 0);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragment.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertex.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
    delete window;
}
#endif
