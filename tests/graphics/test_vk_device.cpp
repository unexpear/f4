#include <doctest/doctest.h>

#include <limits>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include <vulkan/vulkan_core.h>

#include "backends/vulkan/vulkan_device.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/device/device.h"
#include "random.hpp"
#if defined(F4_GRAPHICS_HAS_OPENGL)
#include <epoxy/gl.h>
#include "backends/opengl/opengl_device.h"
#include "platform/gl_context.h"
#endif
#if defined(F4_GRAPHICS_HAS_VULKAN_WINDOW)
#include "platform/window.h"
#endif

using namespace f4;

#if defined(F4_VK_TEST_VERTEX_SPV)
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

auto shaderSource(const char* source, gfx::ShaderStage stage) -> gfx::ShaderDesc {
    gfx::ShaderDesc desc;
    desc.stage = stage;
    const std::size_t size = std::strlen(source);
    desc.blob.resize(size);
    std::memcpy(desc.blob.data(), source, size);
    return desc;
}

} // namespace
#endif

#if defined(F4_GRAPHICS_HAS_OPENGL) && defined(F4_VK_TEST_PARITY_VERTEX_SPV)
namespace {

constexpr std::size_t kParitySampleCount = 256;
using ParitySamples = std::array<float, kParitySampleCount>;
using ParityImage = std::array<std::uint8_t, kParitySampleCount>;

struct ParityCapture {
    ParitySamples floats{};
    ParityImage rgba8{};
};

auto parityLayout() -> gfx::PipelineLayoutDesc {
    gfx::PipelineLayoutDesc layout;
    gfx::BindGroup group;
    gfx::BindingSlot storage;
    storage.type = gfx::BindingType::StorageBuffer;
    storage.stageMask = gfx::shaderStageBit(gfx::ShaderStage::Fragment);
    group.slots.push(storage);
    layout.groups.push(group);
    return layout;
}

auto captureOpenGlParity(const ParitySamples& inputs) -> ParityCapture {
    constexpr const char* vertexSource = R"glsl(#version 430 core
const vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() { gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0); }
)glsl";
    constexpr const char* fragmentSource = R"glsl(#version 430 core
layout(std430, binding = 0) readonly buffer Inputs { float values[]; } inputsBuffer;
layout(location = 0) out vec4 outColor;
float probe(float inputValue) {
    float a = inputValue * 0.75;
    float b = inputValue * inputValue + 0.125;
    return (a + b) / (abs(inputValue) + 1.0);
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int base = (pixel.y * 8 + pixel.x) * 4;
    outColor = vec4(probe(inputsBuffer.values[base + 0]),
                    probe(inputsBuffer.values[base + 1]),
                    probe(inputsBuffer.values[base + 2]),
                    probe(inputsBuffer.values[base + 3]));
}
)glsl";
    constexpr const char* imageFragmentSource = R"glsl(#version 430 core
layout(location = 0) out vec4 outColor;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    outColor = ((pixel.x + pixel.y) & 1) == 0
                   ? vec4(1.0, 0.0, 0.0, 1.0)
                   : vec4(0.0, 1.0, 0.0, 1.0);
}
)glsl";
    auto windowResult = platform::Window::create({"M9 OpenGL parity", 8, 8, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* device = gfx::createOpenGLDevice();
    REQUIRE(device != nullptr);

    auto vertex = device->createShader(shaderSource(vertexSource, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(shaderSource(fragmentSource, gfx::ShaderStage::Fragment));
    auto imageFragment = device->createShader(
        shaderSource(imageFragmentSource, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    REQUIRE(imageFragment.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    pipelineDesc.layout = parityLayout();
    gfx::ColorAttachmentState colorState;
    colorState.format = gfx::TextureFormat::RGBA32_FLOAT;
    pipelineDesc.colorAttachments.push(colorState);
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
    ParityCapture output;
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_FLOAT, output.floats.data());
    REQUIRE(glGetError() == GL_NO_ERROR);

    gfx::PipelineStateDesc imagePipelineDesc = pipelineDesc;
    imagePipelineDesc.fragmentShader = imageFragment.value();
    imagePipelineDesc.colorAttachments[0].format = gfx::TextureFormat::RGBA8_UNORM;
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
    commands.bindResource(0, gfx::BindingType::StorageBuffer, input.value().index,
                          input.value().generation);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, output.rgba8.data());
    REQUIRE(glGetError() == GL_NO_ERROR);

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
    delete window;
    return output;
}

auto captureVulkanParity(const ParitySamples& inputs) -> ParityCapture {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    auto vertex = device->createShader(
        loadShader(F4_VK_TEST_PARITY_VERTEX_SPV, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_VK_TEST_PARITY_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
    auto imageFragment = device->createShader(
        loadShader(F4_VK_TEST_IMAGE_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    REQUIRE(imageFragment.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    pipelineDesc.layout = parityLayout();
    gfx::ColorAttachmentState colorState;
    colorState.format = gfx::TextureFormat::RGBA32_FLOAT;
    pipelineDesc.colorAttachments.push(colorState);
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
    commands.bufferBarrier(
        input.value(),
        {gfx::PipelineStageMask::Host, gfx::ResourceAccessMask::HostWrite,
         gfx::TextureLayout::Undefined},
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead,
         gfx::TextureLayout::Undefined});
    commands.beginRenderPass(framebuffer.value());
    commands.bindPipeline(pipeline.value());
    commands.bindResource(0, gfx::BindingType::StorageBuffer, input.value().index,
                          input.value().generation);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    ParityCapture output;
    REQUIRE(device->readTexture(target.value(), 0,
                                std::as_writable_bytes(std::span(output.floats))) ==
            gfx::GfxError::None);
    CHECK(device->validationErrorCount() == 0);

    gfx::PipelineStateDesc imagePipelineDesc = pipelineDesc;
    imagePipelineDesc.fragmentShader = imageFragment.value();
    imagePipelineDesc.colorAttachments[0].format = gfx::TextureFormat::RGBA8_UNORM;
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
    commands.bufferBarrier(
        input.value(),
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead,
         gfx::TextureLayout::Undefined},
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead,
         gfx::TextureLayout::Undefined});
    commands.beginRenderPass(imageFramebuffer.value());
    commands.bindPipeline(imagePipeline.value());
    commands.bindResource(0, gfx::BindingType::StorageBuffer, input.value().index,
                          input.value().generation);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    REQUIRE(device->readTexture(
                imageTarget.value(), 0,
                std::as_writable_bytes(std::span(output.rgba8))) == gfx::GfxError::None);
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
    return output;
}

} // namespace
#endif

TEST_CASE("Vulkan backend: creates a Vulkan 1.3 headless device and graphics queue") {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    REQUIRE(base->backend() == gfx::Backend::Vulkan);
    auto* device = static_cast<gfx::VulkanDevice*>(base);

    CHECK(device->apiVersion() >= VK_API_VERSION_1_3);
    CHECK(device->framesInFlight() == 2);
    if (device->validationEnabled()) CHECK(device->synchronizationValidationEnabled());
    CHECK(device->deviceName()[0] != '\0');
    CHECK(device->graphicsQueueFamily() != std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(device->hasSurface());
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);

    gfx::deleteDevice(device);
}

TEST_CASE("Vulkan backend: buffer lifecycle, writes, and accounting") {
    gfx::IDevice* device = gfx::createVulkanDevice();
    REQUIRE(device != nullptr);
    const std::array<std::byte, 16> initial{};
    gfx::BufferDesc desc;
    desc.sizeBytes = initial.size();
    desc.usage = gfx::BufferUsage::Uniform;
    auto buffer = device->createBuffer(desc, initial);
    REQUIRE(buffer.hasValue());
    CHECK(device->stats().bufferCount == 1);
    CHECK(device->stats().bufferBytes == initial.size());
    CHECK(device->writeBuffer(buffer.value(), 8, std::span(initial).first(8)) ==
          gfx::GfxError::None);
    CHECK(device->writeBuffer(buffer.value(), 9, std::span(initial).first(8)) ==
          gfx::GfxError::BadDescriptor);
    CHECK(device->destroyBuffer(buffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(buffer.value()) == gfx::GfxError::StaleHandle);
    CHECK(device->stats().bufferCount == 0);
    CHECK(static_cast<gfx::VulkanDevice*>(device)->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Vulkan backend: texture uploads validate mip footprints") {
    gfx::IDevice* device = gfx::createVulkanDevice();
    REQUIRE(device != nullptr);
    gfx::TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.mipLevels = 2;
    auto texture = device->createTexture(desc);
    REQUIRE(texture.hasValue());
    const std::array<std::byte, 64> mip0{};
    const std::array<std::byte, 16> mip1{};
    CHECK(device->writeTexture(texture.value(), 0, mip0) == gfx::GfxError::None);
    CHECK(device->writeTexture(texture.value(), 1, mip1) == gfx::GfxError::None);
    CHECK(device->writeTexture(texture.value(), 1, mip0) == gfx::GfxError::BadDescriptor);
    CHECK(device->writeTexture(texture.value(), 2, mip1) == gfx::GfxError::BadDescriptor);
    CHECK(device->stats().textureCount == 1);
    CHECK(device->stats().textureBytes == 80);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::StaleHandle);
    CHECK(static_cast<gfx::VulkanDevice*>(device)->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Vulkan backend: sampler lifecycle validates anisotropy") {
    gfx::IDevice* device = gfx::createVulkanDevice();
    REQUIRE(device != nullptr);
    gfx::SamplerDesc desc;
    desc.maxAnisotropy = 4.0f;
    auto sampler = device->createSampler(desc);
    REQUIRE(sampler.hasValue());
    CHECK(device->stats().samplerCount == 1);
    CHECK(device->destroySampler(sampler.value()) == gfx::GfxError::None);
    CHECK(device->destroySampler(sampler.value()) == gfx::GfxError::StaleHandle);
    desc.maxAnisotropy = 0.0f;
    CHECK(device->createSampler(desc).error() == gfx::GfxError::BadDescriptor);
    CHECK(static_cast<gfx::VulkanDevice*>(device)->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Vulkan backend: synchronization2 barriers and fenced retirement") {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    gfx::BufferDesc bufferDesc;
    bufferDesc.sizeBytes = 16;
    bufferDesc.usage = gfx::BufferUsage::Uniform;
    const std::array<std::byte, 16> initial{};
    auto buffer = device->createBuffer(bufferDesc, initial);
    REQUIRE(buffer.hasValue());
    gfx::TextureDesc textureDesc;
    textureDesc.width = 4;
    textureDesc.height = 4;
    auto texture = device->createTexture(textureDesc);
    REQUIRE(texture.hasValue());

    gfx::CommandBuffer commands;
    commands.begin();
    commands.bufferBarrier(
        buffer.value(),
        {gfx::PipelineStageMask::Host, gfx::ResourceAccessMask::HostWrite,
         gfx::TextureLayout::Undefined},
        {gfx::PipelineStageMask::VertexShader, gfx::ResourceAccessMask::UniformRead,
         gfx::TextureLayout::Undefined});
    commands.textureBarrier(
        texture.value(), {},
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead,
         gfx::TextureLayout::ShaderRead});
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);

    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(buffer.value()) == gfx::GfxError::None);
    CHECK(device->stats().textureCount == 0);
    CHECK(device->stats().bufferCount == 0);

    gfx::CommandBuffer empty;
    empty.begin();
    empty.end();
    REQUIRE(device->submit(empty) == gfx::GfxError::None);
    REQUIRE(device->submit(empty) == gfx::GfxError::None);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

#if defined(F4_GRAPHICS_HAS_OPENGL) && defined(F4_VK_TEST_PARITY_VERTEX_SPV)
TEST_CASE("M9: OpenGL and Vulkan execute the same seeded float probe") {
    ParitySamples inputs{};
    ParitySamples reference{};
    f4test::Random random(0x8A11CEu);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        inputs[index] = random.nextSigned();
        const float a = inputs[index] * 0.75f;
        const float b = inputs[index] * inputs[index] + 0.125f;
        reference[index] = (a + b) / (std::fabs(inputs[index]) + 1.0f);
    }
    const ParityCapture openGl = captureOpenGlParity(inputs);
    const ParityCapture vulkan = captureVulkanParity(inputs);
    const engine::ShaderFloatParityReport openGlReport =
        engine::compareShaderFloats(reference, openGl.floats);
    const engine::ShaderFloatParityReport vulkanReport =
        engine::compareShaderFloats(reference, vulkan.floats);
    const engine::ShaderFloatParityReport crossBackendReport =
        engine::compareShaderFloats(openGl.floats, vulkan.floats);
    const engine::ShaderImageParityReport imageReport =
        engine::compareShaderImagesRgba8(openGl.rgba8, vulkan.rgba8);
    INFO("OpenGL max absolute error=" << openGlReport.maxAbsoluteError);
    INFO("Vulkan max absolute error=" << vulkanReport.maxAbsoluteError);
    INFO("cross-backend max absolute error=" << crossBackendReport.maxAbsoluteError);
    INFO("RGBA8 mismatch pixels=" << imageReport.mismatchPixelCount
                                   << " max channel delta="
                                   << static_cast<int>(imageReport.maxChannelDelta));
    CHECK(openGlReport.matches());
    CHECK(vulkanReport.matches());
    CHECK(crossBackendReport.matches());
    CHECK(imageReport.matches());
}
#endif

#if defined(F4_VK_TEST_VERTEX_SPV)
TEST_CASE("Vulkan backend: build-time SPIR-V creates real shader modules") {
    gfx::IDevice* device = gfx::createVulkanDevice();
    REQUIRE(device != nullptr);
    gfx::ShaderDesc invalid;
    invalid.blob.resize(4);
    CHECK(device->createShader(invalid).error() == gfx::GfxError::BadDescriptor);

    auto vertex = device->createShader(loadShader(F4_VK_TEST_VERTEX_SPV, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_VK_TEST_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
    REQUIRE(vertex.hasValue());
    REQUIRE(fragment.hasValue());
    CHECK(device->stats().shaderCount == 2);
    CHECK(device->stats().shaderCompiled == 2);
    CHECK(device->stats().shaderFailed == 1);
    CHECK(device->destroyShader(vertex.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragment.value()) == gfx::GfxError::None);
    CHECK(static_cast<gfx::VulkanDevice*>(device)->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Vulkan backend: dynamic rendering produces a pixel-verified triangle") {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    auto vertexShader = device->createShader(
        loadShader(F4_VK_TEST_VERTEX_SPV, gfx::ShaderStage::Vertex));
    auto fragmentShader = device->createShader(
        loadShader(F4_VK_TEST_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
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
        vertexDesc,
        std::span(reinterpret_cast<const std::byte*>(vertices.data()), sizeof(vertices)));
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
    const std::size_t center = (32 * 64 + 32) * 4;
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 0]) > 20);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 1]) > 150);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 2]) > 50);
    CHECK(device->stats().drawCalls == 1);
    CHECK(device->stats().primitives == 1);
    CHECK(device->validationErrorCount() == 0);

    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(depthTexture.value()) == gfx::GfxError::None);
    CHECK(device->destroyTexture(texture.value()) == gfx::GfxError::None);
    CHECK(device->destroyBuffer(vertexBuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(fragmentShader.value()) == gfx::GfxError::None);
    CHECK(device->destroyShader(vertexShader.value()) == gfx::GfxError::None);
    gfx::deleteDevice(device);
}

#if defined(F4_VK_TEST_BIND_VERTEX_SPV)
TEST_CASE("Vulkan backend: descriptor sets bind buffers, images, and samplers") {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    auto vertexShader = device->createShader(
        loadShader(F4_VK_TEST_BIND_VERTEX_SPV, gfx::ShaderStage::Vertex));
    auto fragmentShader = device->createShader(
        loadShader(F4_VK_TEST_BIND_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
    REQUIRE(vertexShader.hasValue());
    REQUIRE(fragmentShader.hasValue());

    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.value();
    pipelineDesc.fragmentShader = fragmentShader.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::BindGroup group;
    gfx::BindingSlot uniformSlot;
    uniformSlot.type = gfx::BindingType::UniformBuffer;
    uniformSlot.stageMask = gfx::shaderStageBit(gfx::ShaderStage::Vertex);
    group.slots.push(uniformSlot);
    gfx::BindingSlot textureSlot;
    textureSlot.type = gfx::BindingType::Texture;
    textureSlot.stageMask = gfx::shaderStageBit(gfx::ShaderStage::Fragment);
    group.slots.push(textureSlot);
    gfx::BindingSlot samplerSlot;
    samplerSlot.type = gfx::BindingType::Sampler;
    samplerSlot.stageMask = gfx::shaderStageBit(gfx::ShaderStage::Fragment);
    group.slots.push(samplerSlot);
    pipelineDesc.layout.groups.push(group);
    auto pipeline = device->createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    const std::array<float, 4> params{};
    gfx::BufferDesc uniformDesc;
    uniformDesc.sizeBytes = sizeof(params);
    uniformDesc.usage = gfx::BufferUsage::Uniform;
    auto uniform = device->createBuffer(
        uniformDesc,
        std::span(reinterpret_cast<const std::byte*>(params.data()), sizeof(params)));
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
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    REQUIRE(device->submit(commands) == gfx::GfxError::None);
    std::array<std::byte, 32 * 32 * 4> pixels{};
    REQUIRE(device->readTexture(target.value(), 0, pixels) == gfx::GfxError::None);
    const std::size_t center = (16 * 32 + 16) * 4;
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 0]) == 32);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 1]) == 160);
    CHECK(std::to_integer<std::uint8_t>(pixels[center + 2]) == 224);
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
#endif
#endif

#if defined(F4_GRAPHICS_HAS_VULKAN_WINDOW)
TEST_CASE("Vulkan backend: SDL window creates a surface and presentation queue") {
    auto windowResult = platform::Window::createVulkan({"Vulkan surface test", 64, 64, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();

    gfx::IDevice* base = gfx::createVulkanDevice(window);
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    CHECK(device->hasSurface());
    CHECK(device->presentQueueFamily() != std::numeric_limits<std::uint32_t>::max());

    gfx::SwapchainDesc swapchainDesc;
    swapchainDesc.surfaceFormat = device->preferredSurfaceFormat();
    swapchainDesc.extent = {window->width(), window->height()};
    auto swapchain = device->createSwapchain(swapchainDesc);
    REQUIRE(swapchain.hasValue());
    CHECK(device->stats().swapchainCount == 1);

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = swapchainDesc.extent;
    gfx::AttachmentDesc color;
    color.clearOnLoad = true;
    color.clear.color[0] = 0.1f;
    color.clear.color[1] = 0.2f;
    color.clear.color[2] = 0.3f;
    color.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(color);
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    gfx::CommandBuffer commands;
    commands.begin();
    commands.beginRenderPass(framebuffer.value());
    commands.endRenderPass();
    commands.end();
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(device->submit(commands) == gfx::GfxError::None);
    }
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);

    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroySwapchain(swapchain.value()) == gfx::GfxError::None);
    CHECK(device->stats().swapchainCount == 0);

    swapchain = device->createSwapchain(swapchainDesc);
    REQUIRE(swapchain.hasValue());
    framebuffer = device->createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    gfx::CommandBuffer recreatedCommands;
    recreatedCommands.begin();
    recreatedCommands.beginRenderPass(framebuffer.value());
    recreatedCommands.endRenderPass();
    recreatedCommands.end();
    REQUIRE(device->submit(recreatedCommands) == gfx::GfxError::None);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    CHECK(device->destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device->destroySwapchain(swapchain.value()) == gfx::GfxError::None);

    gfx::deleteDevice(device);
    delete window;
}
#endif
