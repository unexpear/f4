#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string>

#include "core/noise/noise.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/render_graph/render_graph.h"

#if defined(F4_M13_OPENGL_SHADER_SOURCE)
#include "backends/opengl/opengl_device.h"
#include "platform/gl_context.h"
#include "platform/window.h"
#endif
#if defined(F4_M13_VK_COMPUTE_SPV)
#include "backends/vulkan/vulkan_device.h"
#endif
#if defined(F4_M13_DX12_COMPUTE_DXIL)
#include "backends/dx12/dx12_device.h"
#endif
#if defined(F4_M13_METAL_COMPUTE)
#include "backends/metal/metal_device.h"
#endif

#if defined(F4_M13_OPENGL_SHADER_SOURCE) || defined(F4_M13_VK_COMPUTE_SPV) || \
    defined(F4_M13_DX12_COMPUTE_DXIL) || defined(F4_M13_METAL_COMPUTE)
#define F4_M13_GPU_COMPUTE 1
#endif

using namespace f4;

namespace {

#if defined(F4_M13_GPU_COMPUTE)
constexpr std::size_t kSampleCount = 4;
constexpr std::size_t kValuesPerSample = 9;
constexpr std::size_t kValueCount = kSampleCount * kValuesPerSample;
using NoiseValues = std::array<float, kValueCount>;

struct NoiseSample {
    float x;
    float y;
    float z;
    std::uint64_t seed;
};

constexpr std::array<NoiseSample, kSampleCount> kSamples = {{
    {0.125f, -0.75f, 1.5f, 0ull},
    {-13.25f, 7.875f, -2.5f, 1ull},
    {1024.125f, -2048.75f, 0.03125f, 0x0123456789abcdefull},
    {-0.001f, 0.999f, 42.25f, 0xfedcba9876543210ull},
}};

auto cpuReference() -> NoiseValues {
    NoiseValues values{};
    const noise::FbmParams fbm{};
    for (std::size_t sampleIndex = 0; sampleIndex < kSamples.size(); ++sampleIndex) {
        const NoiseSample& sample = kSamples[sampleIndex];
        const std::size_t base = sampleIndex * kValuesPerSample;
        values[base + 0] = noise::value2D(sample.x, sample.y, sample.seed);
        values[base + 1] = noise::value3D(sample.x, sample.y, sample.z, sample.seed);
        values[base + 2] = noise::perlin2D(sample.x, sample.y, sample.seed);
        values[base + 3] = noise::perlin3D(sample.x, sample.y, sample.z, sample.seed);
        values[base + 4] = noise::simplex2D(sample.x, sample.y, sample.seed);
        values[base + 5] = noise::fbm2D(fbm, sample.x, sample.y, sample.seed);
        values[base + 6] = noise::fbm3D(fbm, sample.x, sample.y, sample.z, sample.seed);
        const math::Vec2 warp = noise::warp2D(sample.x, sample.y, sample.seed, 2.75f);
        values[base + 7] = warp.x;
        values[base + 8] = warp.y;
    }
    return values;
}

auto loadShaderFile(const char* path, gfx::ShaderStage stage) -> gfx::ShaderDesc {
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

auto runNoiseCompute(gfx::IDevice& device, gfx::ShaderDesc shaderDesc) -> NoiseValues {
    auto shader = device.createShader(shaderDesc);
    REQUIRE(shader.hasValue());
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.computeShader = shader.value();
    gfx::BindGroup group;
    gfx::BindingSlot outputSlot;
    outputSlot.type = gfx::BindingType::StorageBuffer;
    outputSlot.stageMask = gfx::shaderStageBit(gfx::ShaderStage::Compute);
    group.slots.push(outputSlot);
    pipelineDesc.layout.groups.push(group);
    auto pipeline = device.createPipeline(pipelineDesc);
    REQUIRE(pipeline.hasValue());

    gfx::BufferDesc bufferDesc;
    bufferDesc.sizeBytes = sizeof(NoiseValues);
    bufferDesc.usage = gfx::BufferUsage::Storage | gfx::BufferUsage::TransferSrc;
    auto output = device.createBuffer(bufferDesc, {});
    REQUIRE(output.hasValue());

    gfx::CommandBuffer commands;
    commands.begin();
    commands.bindPipeline(pipeline.value());
    commands.bindResource(0, gfx::BindingType::StorageBuffer,
                          output.value().index, output.value().generation);
    commands.dispatch(static_cast<std::uint32_t>(kSampleCount));
    commands.bufferBarrier(
        output.value(),
        {gfx::PipelineStageMask::ComputeShader, gfx::ResourceAccessMask::ShaderWrite},
        {gfx::PipelineStageMask::Host, gfx::ResourceAccessMask::HostRead});
    commands.end();
    REQUIRE(device.submit(commands) == gfx::GfxError::None);
    CHECK(device.stats().dispatchCalls == 1);

    NoiseValues values{};
    REQUIRE(device.readBuffer(output.value(), 0, std::as_writable_bytes(std::span(values))) ==
            gfx::GfxError::None);

    // Exercise the same kernel through the graph, including a WAW dependency
    // with unchanged state and a forward dependency on an independent pass.
    gfx::RenderGraph graph;
    gfx::ResourceDesc graphOutput;
    graphOutput.type = gfx::ResourceType::Buffer;
    graphOutput.buffer = bufferDesc;
    const auto outputId = graph.declareResource(std::move(graphOutput));
    for (int i = 0; i < 2; ++i) {
        gfx::PassDesc pass;
        if (i == 0) pass.dependsOn.push(3);
        pass.accesses.push({outputId, {gfx::PipelineStageMask::ComputeShader,
            gfx::ResourceAccessMask::ShaderWrite, gfx::TextureLayout::Undefined}});
        pass.record = [&](gfx::CommandBuffer& cb) {
            const auto handle = graph.buffer(outputId);
            cb.bindPipeline(pipeline.value());
            cb.bindResource(0, gfx::BindingType::StorageBuffer, handle.index, handle.generation);
            cb.dispatch(static_cast<std::uint32_t>(kSampleCount));
        };
        (void)graph.declarePass(std::move(pass));
    }
    gfx::PassDesc readback;
    readback.accesses.push({outputId, {gfx::PipelineStageMask::Host,
        gfx::ResourceAccessMask::HostRead, gfx::TextureLayout::Undefined}});
    (void)graph.declarePass(std::move(readback));
    (void)graph.declarePass({});
    for (int frame = 0; frame < 2; ++frame) {
        REQUIRE(graph.execute(device, commands) == gfx::GfxError::None);
        REQUIRE(graph.executionOrder().size() == 4);
        CHECK(graph.executionOrder()[0] == 3);
        std::size_t barriers = 0;
        for (const auto& command : commands.commands()) {
            if (command.kind == gfx::CommandKind::ResourceBarrier) ++barriers;
        }
        CHECK(barriers == 3);
        NoiseValues graphValues{};
        REQUIRE(device.readBuffer(graph.buffer(outputId), 0,
            std::as_writable_bytes(std::span(graphValues))) == gfx::GfxError::None);
        CHECK(std::memcmp(values.data(), graphValues.data(), sizeof(values)) == 0);
    }
    CHECK(graph.release(device) == gfx::GfxError::None);
    device.waitIdle();
    CHECK(device.destroyBuffer(output.value()) == gfx::GfxError::None);
    CHECK(device.destroyPipeline(pipeline.value()) == gfx::GfxError::None);
    CHECK(device.destroyShader(shader.value()) == gfx::GfxError::None);
    return values;
}

void checkCpuParity(const char* backendName, const NoiseValues& values) {
    const NoiseValues reference = cpuReference();
    const engine::ShaderFloatParityReport report =
        engine::compareShaderFloats(reference, values);
    INFO(std::string(backendName) << " max absolute error=" << report.maxAbsoluteError
                                  << ", max relative error=" << report.maxRelativeError
                                  << ", worst sample=" << report.worstSample);
    CHECK(report.matches());
    CHECK(report.maxAbsoluteError * 4.0f <= math::kParityTolerance.abs);
}

[[maybe_unused]] void checkCrossBackendParity(const char* pairName,
                                              const NoiseValues& first,
                                              const NoiseValues& second) {
    const engine::ShaderFloatParityReport report =
        engine::compareShaderFloats(first, second);
    INFO(std::string(pairName) << " max absolute error=" << report.maxAbsoluteError
                               << ", max relative error=" << report.maxRelativeError
                               << ", worst sample=" << report.worstSample);
    CHECK(report.matches());
    CHECK(report.maxAbsoluteError * 4.0f <= math::kParityTolerance.abs);
}
#endif

} // namespace

#if defined(F4_M13_OPENGL_SHADER_SOURCE)
TEST_CASE("M13: OpenGL compute noise matches the CPU contract") {
    auto windowResult = platform::Window::create({"M13 OpenGL compute", 32, 32, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* device = gfx::createOpenGLDevice();
    REQUIRE(device != nullptr);
    const NoiseValues values = runNoiseCompute(
        *device, loadShaderFile(F4_M13_OPENGL_SHADER_SOURCE, gfx::ShaderStage::Compute));
    checkCpuParity("OpenGL", values);
    gfx::deleteDevice(device);
    delete window;
}
#endif

#if defined(F4_M13_VK_COMPUTE_SPV)
TEST_CASE("M13: Vulkan compute noise matches the CPU contract") {
    gfx::IDevice* base = gfx::createVulkanDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::VulkanDevice*>(base);
    const NoiseValues values = runNoiseCompute(
        *device, loadShaderFile(F4_M13_VK_COMPUTE_SPV, gfx::ShaderStage::Compute));
    checkCpuParity("Vulkan", values);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}
#endif

#if defined(F4_M13_DX12_COMPUTE_DXIL)
TEST_CASE("M13: DX12 compute noise matches the CPU contract") {
    gfx::IDevice* base = gfx::createDx12Device();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::Dx12Device*>(base);
    const NoiseValues values = runNoiseCompute(
        *device, loadShaderFile(F4_M13_DX12_COMPUTE_DXIL, gfx::ShaderStage::Compute));
    checkCpuParity("DX12", values);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}
#endif

#if defined(F4_M13_METAL_COMPUTE)
TEST_CASE("Metal hardware: M13 compute noise matches the CPU contract") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    const NoiseValues values = runNoiseCompute(
        *device, loadShaderFile(F4_M13_METAL_COMPUTE, gfx::ShaderStage::Compute));
    checkCpuParity("Metal", values);
    device->waitIdle();
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}
#endif

#if defined(F4_M13_VK_COMPUTE_SPV) && defined(F4_M13_DX12_COMPUTE_DXIL)
TEST_CASE("M13: Vulkan and DX12 compute noise agree") {
    gfx::IDevice* vulkan = gfx::createVulkanDevice();
    gfx::IDevice* dx12 = gfx::createDx12Device();
    REQUIRE(vulkan != nullptr);
    REQUIRE(dx12 != nullptr);
    const NoiseValues vkValues = runNoiseCompute(
        *vulkan, loadShaderFile(F4_M13_VK_COMPUTE_SPV, gfx::ShaderStage::Compute));
    const NoiseValues dxValues = runNoiseCompute(
        *dx12, loadShaderFile(F4_M13_DX12_COMPUTE_DXIL, gfx::ShaderStage::Compute));
    checkCrossBackendParity("Vulkan/DX12", vkValues, dxValues);
    gfx::deleteDevice(dx12);
    gfx::deleteDevice(vulkan);
}
#endif

#if defined(F4_M13_OPENGL_SHADER_SOURCE) && defined(F4_M13_VK_COMPUTE_SPV)
TEST_CASE("M13: OpenGL and Vulkan compute noise agree") {
    auto windowResult = platform::Window::create({"M13 backend parity", 32, 32, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGl = gfx::createOpenGLDevice();
    gfx::IDevice* vulkan = gfx::createVulkanDevice();
    REQUIRE(openGl != nullptr);
    REQUIRE(vulkan != nullptr);
    const NoiseValues glValues = runNoiseCompute(
        *openGl, loadShaderFile(F4_M13_OPENGL_SHADER_SOURCE, gfx::ShaderStage::Compute));
    const NoiseValues vkValues = runNoiseCompute(
        *vulkan, loadShaderFile(F4_M13_VK_COMPUTE_SPV, gfx::ShaderStage::Compute));
    checkCrossBackendParity("OpenGL/Vulkan", glValues, vkValues);
    gfx::deleteDevice(vulkan);
    gfx::deleteDevice(openGl);
    delete window;
}
#endif
