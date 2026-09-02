#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>

#include "backends/dx12/dx12_device.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/command/command_buffer.h"
#include "random.hpp"
#if defined(F4_GRAPHICS_HAS_DX12_WINDOW)
#include "platform/window.h"
#endif

using namespace f4;

#if defined(F4_DX12_TEST_VERTEX_DXIL)
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
#endif

#if defined(F4_GRAPHICS_HAS_DX12_WINDOW) && defined(F4_DX12_TEST_VERTEX_DXIL)
TEST_CASE("DX12 backend: SDL HWND swapchain presents and recreates") {
    auto windowResult = platform::Window::createNative(
        {"DX12 swapchain test", 96, 64, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    gfx::IDevice* base = gfx::createDx12Device(window);
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    CHECK(device->hasWindow());
    auto vertex = device->createShader(
        loadShader(F4_DX12_TEST_PARITY_VERTEX_DXIL, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_DX12_TEST_IMAGE_FRAGMENT_DXIL, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    for (std::size_t recreation = 0; recreation < 2; ++recreation) {
        gfx::SwapchainDesc swapchainDesc;
        swapchainDesc.extent = {window->width(), window->height()};
        auto swapchain = device->createSwapchain(swapchainDesc);
        REQUIRE(swapchain.hasValue());
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

TEST_CASE("DX12 backend: creates a headless device and direct queue") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    CHECK(device->backend() == gfx::Backend::DirectX12);
    CHECK(device->deviceName()[0] != '\0');
    CHECK(device->featureLevel() >= 0xb000u);
    CHECK(device->framesInFlight() == 2);
    CHECK_FALSE(device->hasWindow());
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("DX12 backend: buffer, texture, sampler, and retirement lifecycle") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    gfx::BufferDesc bufferDesc;
    bufferDesc.sizeBytes = 64;
    bufferDesc.usage = gfx::BufferUsage::Uniform;
    const std::array<std::byte, 16> initial{};
    auto buffer = device->createBuffer(bufferDesc, initial);
    REQUIRE(buffer.hasValue());
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
    std::array<std::byte, 64> readback{};
    REQUIRE(device->readTexture(texture.value(), 0, readback) == gfx::GfxError::None);
    CHECK(readback == mip0);
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

TEST_CASE("DX12 backend: rejected command lists roll resource state back") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    gfx::TextureDesc textureDesc;
    textureDesc.width = 4;
    textureDesc.height = 4;
    auto texture = device->createTexture(textureDesc);
    REQUIRE(texture.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {4, 4};
    gfx::AttachmentDesc color;
    color.texture = texture.value();
    color.clearOnLoad = true;
    color.clear.color[0] = 0.25f;
    color.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(color);
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    gfx::CommandBuffer rejected;
    rejected.begin();
    rejected.textureBarrier(
        texture.value(), {},
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead,
         gfx::TextureLayout::ShaderRead});
    rejected.beginRenderPass(framebuffer.value());
    rejected.end();
    CHECK(device->submit(rejected) == gfx::GfxError::BadDescriptor);

    gfx::CommandBuffer valid;
    valid.begin();
    valid.beginRenderPass(framebuffer.value());
    valid.endRenderPass();
    valid.end();
    REQUIRE(device->submit(valid) == gfx::GfxError::None);
    std::array<std::byte, 4 * 4 * 4> pixels{};
    REQUIRE(device->readTexture(texture.value(), 0, pixels) == gfx::GfxError::None);
    CHECK(std::to_integer<std::uint8_t>(pixels[0]) == 64);
    CHECK(device->validationErrorCount() == 0);
    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
}

#if defined(F4_DX12_TEST_VERTEX_DXIL)
TEST_CASE("DX12 backend: DXC shaders render a pixel-verified triangle") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    auto vertexShader = device->createShader(
        loadShader(F4_DX12_TEST_VERTEX_DXIL, gfx::ShaderStage::Vertex));
    auto fragmentShader = device->createShader(
        loadShader(F4_DX12_TEST_FRAGMENT_DXIL, gfx::ShaderStage::Fragment));
    REQUIRE(vertexShader.hasValue());
    REQUIRE(fragmentShader.hasValue());

    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.value();
    pipelineDesc.fragmentShader = fragmentShader.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    pipelineDesc.depthStencil.depthTestEnable = true;
    pipelineDesc.depthStencil.depthWriteEnable = true;
    pipelineDesc.depthStencil.format = gfx::TextureFormat::Depth24Stencil8;
    pipelineDesc.vertexInput.bindings.push({0, sizeof(float) * 2, 0});
    pipelineDesc.vertexInput.attributes.push(
        {0, 0, gfx::VertexFormat::Float32x2, 0});
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    constexpr std::array<float, 6> vertices{{-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f}};
    gfx::BufferDesc vertexDesc;
    vertexDesc.sizeBytes = sizeof(vertices);
    vertexDesc.usage = gfx::BufferUsage::Vertex;
    auto vertexBuffer = device->createBuffer(
        vertexDesc, std::span(reinterpret_cast<const std::byte*>(vertices.data()),
                              sizeof(vertices)));
    REQUIRE(vertexBuffer.hasValue());
    gfx::TextureDesc textureDesc;
    textureDesc.width = 64;
    textureDesc.height = 64;
    auto texture = device->createTexture(textureDesc);
    REQUIRE(texture.hasValue());
    gfx::TextureDesc depthDesc;
    depthDesc.format = gfx::TextureFormat::Depth24Stencil8;
    depthDesc.width = 64;
    depthDesc.height = 64;
    auto depthTexture = device->createTexture(depthDesc);
    REQUIRE(depthTexture.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    gfx::AttachmentDesc color;
    color.texture = texture.value();
    color.clearOnLoad = true;
    color.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(color);
    gfx::AttachmentDesc depth;
    depth.type = gfx::AttachmentType::Depth;
    depth.texture = depthTexture.value();
    depth.clearOnLoad = true;
    depth.clear.depth = 1.0f;
    framebufferDesc.attachments.push(depth);
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    gfx::CommandBuffer commands;
    commands.begin();
    commands.beginRenderPass(framebuffer.value());
    commands.bindPipeline(pipeline.value());
    commands.bindVertexBuffer(0, vertexBuffer.value(), sizeof(float) * 2);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    std::array<std::byte, 64 * 64 * 4> pixels{};
    REQUIRE(device->readTexture(texture.value(), 0, pixels) == gfx::GfxError::None);
    const auto firstFrame = pixels;
    for (std::size_t frame = 1; frame < 12; ++frame) {
        REQUIRE(device->submit(commands) == gfx::GfxError::None);
        REQUIRE(device->readTexture(texture.value(), 0, pixels) == gfx::GfxError::None);
        CHECK(pixels == firstFrame);
    }
    const std::size_t center = (32 * 64 + 32) * 4;
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 0]) > 20);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 1]) > 150);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 2]) > 50);
    CHECK(device->stats().drawCalls == 12);
    CHECK(device->validationErrorCount() == 0);
    CHECK(device->stats().shaderCount == 2);

    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(depthTexture.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(vertexBuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragmentShader.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertexShader.value()) == gfx::GfxError::None);
    CHECK(device->stats().shaderCount == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("DX12 backend: descriptor tables bind CBV, SRV, and sampler past 64 draws") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    auto vertexShader = device->createShader(
        loadShader(F4_DX12_TEST_BIND_VERTEX_DXIL, gfx::ShaderStage::Vertex));
    auto fragmentShader = device->createShader(
        loadShader(F4_DX12_TEST_BIND_FRAGMENT_DXIL, gfx::ShaderStage::Fragment));
    REQUIRE(vertexShader.hasValue());
    REQUIRE(fragmentShader.hasValue());

    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.value();
    pipelineDesc.fragmentShader = fragmentShader.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::BindGroup group;
    group.slots.push({0, gfx::BindingType::UniformBuffer,
                      gfx::shaderStageBit(gfx::ShaderStage::Vertex), 1});
    group.slots.push({0, gfx::BindingType::Texture,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    group.slots.push({0, gfx::BindingType::Sampler,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    pipelineDesc.layout.groups.push(group);
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    const std::array<float, 4> params{};
    gfx::BufferDesc uniformDesc;
    uniformDesc.sizeBytes = sizeof(params);
    uniformDesc.usage = gfx::BufferUsage::Uniform;
    auto uniform = device->createBuffer(uniformDesc, std::as_bytes(std::span(params)));
    REQUIRE(uniform.hasValue());
    gfx::TextureDesc sampledDesc;
    sampledDesc.width = 2;
    sampledDesc.height = 2;
    auto sampled = device->createTexture(sampledDesc);
    REQUIRE(sampled.hasValue());
    std::array<std::byte, 16> texels{};
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        texels[pixel * 4 + 0] = std::byte{32};
        texels[pixel * 4 + 1] = std::byte{160};
        texels[pixel * 4 + 2] = std::byte{224};
        texels[pixel * 4 + 3] = std::byte{255};
    }
    REQUIRE(device->writeTexture(sampled.value(), 0, texels) == gfx::GfxError::None);
    auto sampler = device->createSampler(gfx::SamplerDesc{});
    REQUIRE(sampler.hasValue());
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
    commands.bindResource(0, gfx::BindingType::UniformBuffer, uniform.value().index,
                          uniform.value().generation);
    commands.bindResource(0, gfx::BindingType::Texture, sampled.value().index,
                          sampled.value().generation);
    commands.bindResource(0, gfx::BindingType::Sampler, sampler.value().index,
                          sampler.value().generation);
    for (std::size_t draw = 0; draw < 130; ++draw) commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    std::array<std::byte, 32 * 32 * 4> pixels{};
    REQUIRE(device->readTexture(target.value(), 0, pixels) == gfx::GfxError::None);
    const std::size_t center = (16 * 32 + 16) * 4;
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 0]) == 32);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 1]) == 160);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 2]) == 224);
    CHECK(device->stats().drawCalls == 130);
    CHECK(device->validationErrorCount() == 0);

    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(target.value()) == gfx::GfxError::None);
    CHECK(device->destroySampler(sampler.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(sampled.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(uniform.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragmentShader.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertexShader.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
}

TEST_CASE("M10: DX12 executes the shared seeded float and RGBA8 parity probes") {
    constexpr std::size_t sampleCount = 256;
    using Samples = std::array<float, sampleCount>;
    Samples inputs{};
    Samples reference{};
    f4test::Random random(0x8A11CEu);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        inputs[index] = random.nextSigned();
        const float a = inputs[index] * 0.75f;
        const float b = inputs[index] * inputs[index] + 0.125f;
        reference[index] = (a + b) / (std::fabs(inputs[index]) + 1.0f);
    }

    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    auto vertex = device->createShader(
        loadShader(F4_DX12_TEST_PARITY_VERTEX_DXIL, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_DX12_TEST_PARITY_FRAGMENT_DXIL, gfx::ShaderStage::Fragment));
    auto imageFragment = device->createShader(
        loadShader(F4_DX12_TEST_IMAGE_FRAGMENT_DXIL, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    REQUIRE(imageFragment.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::BindGroup group;
    group.slots.push({0, gfx::BindingType::StorageBuffer,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    pipelineDesc.layout.groups.push(group);
    gfx::ColorAttachmentState floatColor;
    floatColor.format = gfx::TextureFormat::RGBA32_FLOAT;
    pipelineDesc.colorAttachments.push(floatColor);
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());
    gfx::BufferDesc inputDesc;
    inputDesc.sizeBytes = sizeof(inputs);
    inputDesc.usage = gfx::BufferUsage::Storage;
    auto input = device->createBuffer(inputDesc, std::as_bytes(std::span(inputs)));
    REQUIRE(input.hasValue());
    gfx::TextureDesc targetDesc;
    targetDesc.width = 8;
    targetDesc.height = 8;
    targetDesc.format = gfx::TextureFormat::RGBA32_FLOAT;
    auto target = device->createTexture(targetDesc);
    REQUIRE(target.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {8, 8};
    gfx::AttachmentDesc color;
    color.texture = target.value();
    framebufferDesc.attachments.push(color);
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    gfx::CommandBuffer commands;
    commands.begin();
    commands.beginRenderPass(framebuffer.value());
    commands.bindPipeline(pipeline.value());
    commands.bindResource(0, gfx::BindingType::StorageBuffer, input.value().index,
                          input.value().generation);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    Samples actual{};
    REQUIRE(device->readTexture(target.value(), 0,
                                std::as_writable_bytes(std::span(actual))) ==
            gfx::GfxError::None);
    const engine::ShaderFloatParityReport floatReport =
        engine::compareShaderFloats(reference, actual);
    INFO("DX12 max absolute error=" << floatReport.maxAbsoluteError);
    CHECK(floatReport.matches());

    gfx::PipelineStateDesc imagePipelineDesc;
    imagePipelineDesc.vertexShader = vertex.value();
    imagePipelineDesc.fragmentShader = imageFragment.value();
    imagePipelineDesc.raster.cullMode = gfx::CullMode::None;
    auto imagePipeline = device->createPipeline(imagePipelineDesc);
    REQUIRE(imagePipeline.hasValue());
    gfx::TextureDesc imageTargetDesc;
    imageTargetDesc.width = 8;
    imageTargetDesc.height = 8;
    auto imageTarget = device->createTexture(imageTargetDesc);
    REQUIRE(imageTarget.hasValue());
    gfx::FramebufferDesc imageFramebufferDesc;
    imageFramebufferDesc.extent = {8, 8};
    gfx::AttachmentDesc imageColor;
    imageColor.texture = imageTarget.value();
    imageFramebufferDesc.attachments.push(imageColor);
    auto imageFramebuffer = device->createFramebuffer(imageFramebufferDesc);
    REQUIRE(imageFramebuffer.hasValue());
    commands.begin();
    commands.beginRenderPass(imageFramebuffer.value());
    commands.bindPipeline(imagePipeline.value());
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    std::array<std::uint8_t, sampleCount> image{};
    REQUIRE(device->readTexture(
                imageTarget.value(), 0,
                std::as_writable_bytes(std::span(image))) == gfx::GfxError::None);
    std::array<std::uint8_t, sampleCount> expectedImage{};
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const std::size_t offset = (y * 8 + x) * 4;
            expectedImage[offset + (((x + y) & 1u) == 0 ? 0u : 1u)] = 255;
            expectedImage[offset + 3] = 255;
        }
    }
    const engine::ShaderImageParityReport imageReport =
        engine::compareShaderImagesRgba8(expectedImage, image);
    INFO("DX12 RGBA8 mismatch pixels=" << imageReport.mismatchPixelCount);
    CHECK(imageReport.matches());
    CHECK(device->validationErrorCount() == 0);

    CHECK(device->destroyFramebuffer(imageFramebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(imageTarget.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(imagePipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(target.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(input.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(imageFragment.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragment.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertex.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
}
#endif
