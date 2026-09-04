#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <span>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "backends/webgpu/webgpu_device.h"
#include "core/ecs/transform.h"
#include "core/math/mat4.h"
#include "core/noise/noise.h"
#include "core/resources/mesh.h"
#include "core/resources/texture.h"
#include "demos/demo_cornell_box.h"
#include "demos/demo_render_validation.h"
#include "demos/demo_terrain.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/light.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/renderer.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/command/command_buffer.h"
#include "webgpu_shaders.h"

namespace {

using namespace f4::gfx;

constexpr Extent kCanvasExtent{960, 540};
constexpr std::uint32_t kRendererExtent = 64;
constexpr std::size_t kRendererCaptureBytes =
    static_cast<std::size_t>(kRendererExtent) * kRendererExtent * 4u;

constexpr std::uint32_t kDrawsPerFrame = 130;
constexpr std::size_t kNoiseSampleCount = 4;
constexpr std::size_t kNoiseValuesPerSample = 9;
using NoiseValues = std::array<float, kNoiseSampleCount * kNoiseValuesPerSample>;

struct NoiseSample {
    float x;
    float y;
    float z;
    std::uint64_t seed;
};

constexpr std::array<NoiseSample, kNoiseSampleCount> kNoiseSamples{{
    {0.125f, -0.75f, 1.5f, 0ull},
    {-13.25f, 7.875f, -2.5f, 1ull},
    {1024.125f, -2048.75f, 0.03125f, 0x0123456789abcdefull},
    {-0.001f, 0.999f, 42.25f, 0xfedcba9876543210ull},
}};

struct PbrVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint8_t color[4];
};
static_assert(sizeof(PbrVertex) == 36);

constexpr std::array<PbrVertex, 24> kCubeVertices{{
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {255, 175, 70, 255}},
    {{ 0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {255, 175, 70, 255}},
    {{ 0.5f,  0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {255, 175, 70, 255}},
    {{-0.5f,  0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {255, 175, 70, 255}},
    {{-0.5f, -0.5f,  0.5f}, {0, 0, 1}, {1, 1}, {55, 205, 170, 255}},
    {{ 0.5f, -0.5f,  0.5f}, {0, 0, 1}, {0, 1}, {55, 205, 170, 255}},
    {{ 0.5f,  0.5f,  0.5f}, {0, 0, 1}, {0, 0}, {55, 205, 170, 255}},
    {{-0.5f,  0.5f,  0.5f}, {0, 0, 1}, {1, 0}, {55, 205, 170, 255}},
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {225, 75, 58, 255}},
    {{-0.5f, -0.5f,  0.5f}, {-1, 0, 0}, {0, 1}, {225, 75, 58, 255}},
    {{-0.5f,  0.5f,  0.5f}, {-1, 0, 0}, {0, 0}, {225, 75, 58, 255}},
    {{-0.5f,  0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {225, 75, 58, 255}},
    {{ 0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {75, 135, 230, 255}},
    {{ 0.5f, -0.5f,  0.5f}, {1, 0, 0}, {1, 1}, {75, 135, 230, 255}},
    {{ 0.5f,  0.5f,  0.5f}, {1, 0, 0}, {1, 0}, {75, 135, 230, 255}},
    {{ 0.5f,  0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {75, 135, 230, 255}},
    {{-0.5f,  0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {245, 230, 185, 255}},
    {{ 0.5f,  0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {245, 230, 185, 255}},
    {{ 0.5f,  0.5f,  0.5f}, {0, 1, 0}, {1, 0}, {245, 230, 185, 255}},
    {{-0.5f,  0.5f,  0.5f}, {0, 1, 0}, {0, 0}, {245, 230, 185, 255}},
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {95, 80, 70, 255}},
    {{ 0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {95, 80, 70, 255}},
    {{ 0.5f, -0.5f,  0.5f}, {0, -1, 0}, {1, 1}, {95, 80, 70, 255}},
    {{-0.5f, -0.5f,  0.5f}, {0, -1, 0}, {0, 1}, {95, 80, 70, 255}},
}};

constexpr std::array<std::uint16_t, 36> kCubeIndices{
    0, 2, 1, 2, 0, 3, 4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8, 12, 14, 13, 14, 12, 15,
    16, 18, 17, 18, 16, 19, 20, 21, 22, 22, 23, 20,
};

struct PbrSceneUbo {
    f4::math::Mat4 viewProj;
    f4::math::Mat4 model;
    f4::math::Mat4 normalMatrix;
    f4::math::Mat4 lightViewProj;
    f4::math::Vec4 baseColorFactor;
    f4::math::Vec4 emissiveMetallic;
    f4::math::Vec4 cameraExposure;
    f4::math::Vec4 lightDirectionIntensity;
    f4::math::Vec4 lightColorRoughness;
    f4::math::Vec4 materialParams;
    f4::math::Vec4 renderParams;
    f4::math::Vec4 ambientFlags;
};
static_assert(sizeof(PbrSceneUbo) == 384);

constexpr auto rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                    std::uint8_t a = 255) -> std::array<std::byte, 4> {
    return {std::byte{r}, std::byte{g}, std::byte{b}, std::byte{a}};
}

constexpr std::array<std::array<std::byte, 4>, 6> kPbrPixels{{
    rgba(235, 225, 205), rgba(128, 128, 255), rgba(0, 255, 255),
    rgba(255, 255, 255), rgba(0, 0, 0), rgba(75, 115, 170),
}};
constexpr std::array<std::byte, 2> kShadowPixel{std::byte{0x00}, std::byte{0x3c}};

constexpr char kStripVertexWgsl[] = R"wgsl(
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let positions = array<vec2<f32>, 4>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0),
        vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 1.0));
    var output: VertexOutput;
    output.position = vec4<f32>(positions[index], 0.0, 1.0);
    return output;
}
)wgsl";

constexpr char kStripFragmentWgsl[] = R"wgsl(
@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.15, 0.75, 0.35, 1.0);
}
)wgsl";

struct App {
    IDevice* device = nullptr;
    WebGpuDevice* webGpu = nullptr;
    ShaderHandle vertexShader{};
    ShaderHandle fragmentShader{};
    BufferHandle vertexBuffer{};
    BufferHandle indexBuffer{};
    BufferHandle uniformBuffer{};
    std::array<TextureHandle, 7> textures{};
    SamplerHandle sampler{};
    PipelineHandle pipeline{};
    TextureHandle depthTexture{};
    SwapchainHandle swapchain{};
    FramebufferHandle framebuffer{};
    std::array<std::byte, kRendererCaptureBytes> rendererCapture{};
    TextureFormat surfaceFormat = TextureFormat::RGBA8_UNORM;
    Extent extent{};
    std::uint32_t frameCount = 0;
    std::uint32_t validationSceneIndex = 0;
    f4::demo::FreeCamera validationCamera{};
    f4::demo::PbrSphereGridScene pbrSpheres;
    f4::demo::MipmappingScene mipmapping;
    f4::demo::ZFightingScene zFighting;
    f4::demo::AlphaOrderScene alphaOrder;
    f4::demo::NormalMapOrientationScene normalMapping;
    f4::demo::HybridRenderingScene hybrid;
    f4::demo::CornellBoxScene cornell;
    f4::demo::TerrainScene terrain;
};

App gApp;

auto validationScene(std::uint32_t index) -> f4::demo::IDemoScene* {
    switch (index) {
    case 1: return &gApp.pbrSpheres;
    case 2: return &gApp.mipmapping;
    case 3: return &gApp.zFighting;
    case 4: return &gApp.alphaOrder;
    case 5: return &gApp.normalMapping;
    case 6: return &gApp.hybrid;
    case 7: return &gApp.cornell;
    case 8: return &gApp.terrain;
    default: return nullptr;
    }
}

auto shaderBlob(const char* source) -> f4::containers::Array<std::byte> {
    f4::containers::Array<std::byte> result;
    const std::size_t length = std::strlen(source);
    result.resize(length);
    std::memcpy(result.data(), source, length);
    return result;
}

auto cpuNoiseReference() -> NoiseValues {
    NoiseValues values{};
    const f4::noise::FbmParams fbm{};
    for (std::size_t sampleIndex = 0; sampleIndex < kNoiseSamples.size(); ++sampleIndex) {
        const NoiseSample& sample = kNoiseSamples[sampleIndex];
        const std::size_t base = sampleIndex * kNoiseValuesPerSample;
        values[base + 0] = f4::noise::value2D(sample.x, sample.y, sample.seed);
        values[base + 1] = f4::noise::value3D(sample.x, sample.y, sample.z, sample.seed);
        values[base + 2] = f4::noise::perlin2D(sample.x, sample.y, sample.seed);
        values[base + 3] = f4::noise::perlin3D(sample.x, sample.y, sample.z, sample.seed);
        values[base + 4] = f4::noise::simplex2D(sample.x, sample.y, sample.seed);
        values[base + 5] = f4::noise::fbm2D(fbm, sample.x, sample.y, sample.seed);
        values[base + 6] = f4::noise::fbm3D(fbm, sample.x, sample.y, sample.z, sample.seed);
        const f4::math::Vec2 warp =
            f4::noise::warp2D(sample.x, sample.y, sample.seed, 2.75f);
        values[base + 7] = warp.x;
        values[base + 8] = warp.y;
    }
    return values;
}

void fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    EM_ASM({
        document.getElementById('status').textContent = 'initialization failed';
    });
}

auto canvasExtent() -> Extent {
    double cssWidth = 0.0;
    double cssHeight = 0.0;
    if (emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        !std::isfinite(cssWidth) || !std::isfinite(cssHeight) || cssWidth <= 0.0 ||
        cssHeight <= 0.0) {
        return gApp.extent.width == 0 ? kCanvasExtent : gApp.extent;
    }
    const double ratio = std::max(1.0, emscripten_get_device_pixel_ratio());
    const double deviceLimit = static_cast<double>(gApp.webGpu->maxTextureDimension2D());
    const double maxDimension = std::min(
        deviceLimit, static_cast<double>(std::numeric_limits<int>::max()));
    return {
        static_cast<std::uint32_t>(
            std::clamp(std::round(cssWidth * ratio), 1.0, maxDimension)),
        static_cast<std::uint32_t>(
            std::clamp(std::round(cssHeight * ratio), 1.0, maxDimension)),
    };
}

auto recreateTargets(Extent extent) -> bool {
    if (gApp.framebuffer.isValid() && extent == gApp.extent) return true;
    gApp.device->waitIdle();
    if (gApp.framebuffer.isValid() &&
        gApp.device->destroyFramebuffer(gApp.framebuffer) != GfxError::None) return false;
    if (gApp.swapchain.isValid() &&
        gApp.device->destroySwapchain(gApp.swapchain) != GfxError::None) return false;
    if (gApp.depthTexture.isValid() &&
        gApp.device->destroyTexture(gApp.depthTexture) != GfxError::None) return false;
    gApp.framebuffer = {};
    gApp.swapchain = {};
    gApp.depthTexture = {};

    if (emscripten_set_canvas_element_size("#canvas", static_cast<int>(extent.width),
                                           static_cast<int>(extent.height)) !=
        EMSCRIPTEN_RESULT_SUCCESS) return false;
    SwapchainDesc swapchainDesc;
    swapchainDesc.extent = extent;
    swapchainDesc.surfaceFormat = gApp.surfaceFormat;
    auto swapchain = gApp.device->createSwapchain(swapchainDesc);
    if (!swapchain) return false;

    TextureDesc depthDesc;
    depthDesc.format = TextureFormat::Depth32F;
    depthDesc.width = extent.width;
    depthDesc.height = extent.height;
    auto depthTexture = gApp.device->createTexture(depthDesc);
    if (!depthTexture) {
        gApp.device->destroySwapchain(swapchain.value());
        return false;
    }

    FramebufferDesc framebufferDesc;
    framebufferDesc.extent = extent;
    AttachmentDesc colorAttachment;
    colorAttachment.type = AttachmentType::Color;
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.025f;
    colorAttachment.clear.color[1] = 0.055f;
    colorAttachment.clear.color[2] = 0.045f;
    colorAttachment.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(colorAttachment);
    AttachmentDesc depthAttachment;
    depthAttachment.type = AttachmentType::Depth;
    depthAttachment.texture = depthTexture.value();
    depthAttachment.clearOnLoad = true;
    depthAttachment.clear.depth = 1.0f;
    framebufferDesc.attachments.push(depthAttachment);
    auto framebuffer = gApp.device->createFramebuffer(framebufferDesc);
    if (!framebuffer) {
        gApp.device->destroyTexture(depthTexture.value());
        gApp.device->destroySwapchain(swapchain.value());
        return false;
    }

    gApp.swapchain = swapchain.value();
    gApp.depthTexture = depthTexture.value();
    gApp.framebuffer = framebuffer.value();
    gApp.extent = extent;
    gApp.frameCount = 0;
    EM_ASM({
        document.getElementById('status').textContent = 'recreating canvas...';
        document.body.dataset.f4ResizeCount = String(
            (Number(document.body.dataset.f4ResizeCount) || 0) + 1);
        delete document.body.dataset.f4Ready;
    });
    return true;
}

auto pbrSceneConstants(Extent extent) -> PbrSceneUbo {
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(extent.height);
    const f4::math::Vec3 eye{3.2f, 2.35f, 4.4f};
    const f4::math::Mat4 view = f4::math::Mat4::lookAtRH(
        eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const f4::math::Mat4 projection =
        f4::math::Mat4::perspectiveRH_ZO(0.92f, aspect, 0.1f, 100.0f);
    const f4::math::Mat4 rotation =
        f4::math::Mat4::rotationY(-0.58f).mul(f4::math::Mat4::rotationX(-0.34f));
    const f4::math::Mat4 model = rotation.mul(
        f4::math::Mat4::scaling({1.55f, 1.55f, 1.55f}));
    const f4::math::Vec3 lightDirection =
        f4::math::normalized(f4::math::Vec3{0.45f, -1.0f, 0.3f});
    return {
        projection.mul(view), model, rotation, f4::math::Mat4::identity(),
        {0.92f, 0.78f, 0.58f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.62f},
        {eye, 1.0f},
        {lightDirection, 3.6f},
        {1.0f, 0.93f, 0.82f, 0.34f},
        {1.0f, 1.0f, 1.0f, 0.5f},
        {1.0f, 0.001f, 0.0f, 0.0f},
        {0.32f, 12.0f, 0.0f, 0.0f},
    };
}

void renderFrame() {
    if (!recreateTargets(canvasExtent())) {
        fail("WebGPU canvas resize/recreation failed");
        emscripten_cancel_main_loop();
        return;
    }
    gApp.device->resetFrameStats();
    if (f4::demo::IDemoScene* scene = validationScene(gApp.validationSceneIndex)) {
        scene->onResize(gApp.extent.width, gApp.extent.height);
        scene->update(0.0f, nullptr, nullptr);
        CommandBuffer commands;
        commands.begin();
        if (scene->recordsOwnPasses()) {
            scene->renderPasses(commands, gApp.validationCamera, gApp.extent.width,
                                gApp.extent.height, gApp.framebuffer);
        } else {
            commands.beginRenderPass(gApp.framebuffer);
            commands.setViewport(gApp.extent);
            commands.setScissor(gApp.extent);
            scene->render(commands, gApp.validationCamera, gApp.extent.width,
                          gApp.extent.height);
            commands.endRenderPass();
        }
        commands.end();
        if (gApp.device->submit(commands) != GfxError::None ||
            gApp.webGpu->validationErrorCount() != 0) {
            fail("WebGPU validation scene submission failed");
            emscripten_cancel_main_loop();
            return;
        }
        const DeviceStats stats = gApp.device->stats();
        if (stats.drawCalls + stats.drawIndexedCalls == 0) {
            fail("WebGPU validation scene recorded no draws");
            emscripten_cancel_main_loop();
            return;
        }
        ++gApp.frameCount;
        if (gApp.frameCount == 3) {
            char script[512]{};
            std::snprintf(
                script, sizeof(script),
                "document.getElementById('status').textContent='WebGPU / %s';"
                "document.body.dataset.f4Ready='true';"
                "document.body.dataset.f4ValidationScene='true';"
                "document.body.dataset.f4ValidationErrors='0';",
                scene->name());
            emscripten_run_script(script);
        }
        return;
    }
    const PbrSceneUbo constants = pbrSceneConstants(gApp.extent);
    if (gApp.device->writeBuffer(gApp.uniformBuffer, 0,
                                 std::as_bytes(std::span{&constants, 1})) !=
        GfxError::None) {
        fail("WebGPU PBR constants upload failed");
        emscripten_cancel_main_loop();
        return;
    }
    CommandBuffer commands;
    commands.begin();
    commands.beginRenderPass(gApp.framebuffer);
    commands.bindPipeline(gApp.pipeline);
    commands.bindVertexBuffer(0, gApp.vertexBuffer, sizeof(PbrVertex));
    commands.bindIndexBuffer(gApp.indexBuffer, IndexFormat::U16);
    commands.setViewport(gApp.extent);
    commands.setScissor(gApp.extent);
    commands.bindResource(0, 0, BindingType::UniformBuffer,
                          gApp.uniformBuffer.index, gApp.uniformBuffer.generation);
    for (std::uint32_t slot = 0; slot < gApp.textures.size(); ++slot) {
        commands.bindResource(0, slot, BindingType::Texture,
                              gApp.textures[slot].index, gApp.textures[slot].generation);
        commands.bindResource(0, slot, BindingType::Sampler,
                              gApp.sampler.index, gApp.sampler.generation);
    }
    for (std::uint32_t draw = 0; draw < kDrawsPerFrame; ++draw) {
        commands.drawIndexed(static_cast<std::uint32_t>(kCubeIndices.size()));
    }
    commands.endRenderPass();
    commands.end();

    if (gApp.device->submit(commands) != GfxError::None) {
        fail("WebGPU command submission failed");
        emscripten_cancel_main_loop();
        return;
    }
    const DeviceStats frameStats = gApp.device->stats();
    if (frameStats.drawIndexedCalls != kDrawsPerFrame ||
        frameStats.batches != kDrawsPerFrame ||
        gApp.webGpu->validationErrorCount() != 0) {
        fail("WebGPU 130-draw or validation conformance failed");
        emscripten_cancel_main_loop();
        return;
    }
    ++gApp.frameCount;
    if (gApp.frameCount == 3) {
        EM_ASM({
            document.getElementById('status').textContent =
                'native WebGPU / PBR cube / M13 parity / 130 draws';
            document.body.dataset.f4Ready = 'true';
            document.body.dataset.f4PbrVisual = 'true';
            document.body.dataset.f4Draws = '130';
            document.body.dataset.f4ValidationErrors = '0';
            document.body.dataset.f4StableResize = document.body.dataset.f4ResizeCount;
        });
    }
}

template <typename T>
auto take(f4::Result<T, GfxError>&& result, T& output, const char* message) -> bool {
    if (!result) {
        fail(message);
        return false;
    }
    output = result.value();
    return true;
}

auto validateNoiseParity() -> bool {
    ShaderDesc shaderDesc;
    shaderDesc.stage = ShaderStage::Compute;
    shaderDesc.blob = shaderBlob(f4::web::kM13NoiseWgsl);
    ShaderHandle shader;
    if (!take(gApp.device->createShader(shaderDesc), shader,
              "M13 WGSL compute module creation failed")) return false;

    auto cleanup = [&](BufferHandle parameters, BufferHandle output,
                       PipelineHandle pipeline) {
        if (parameters.isValid()) gApp.device->destroyBuffer(parameters);
        if (output.isValid()) gApp.device->destroyBuffer(output);
        if (pipeline.isValid()) gApp.device->destroyPipeline(pipeline);
        if (shader.isValid()) gApp.device->destroyShader(shader);
    };

    PipelineStateDesc pipelineDesc;
    pipelineDesc.computeShader = shader;
    BindGroup group;
    group.slots.push(BindingSlot{
        0, BindingType::UniformBuffer, shaderStageBit(ShaderStage::Compute), 1});
    group.slots.push(BindingSlot{
        0, BindingType::StorageBuffer, shaderStageBit(ShaderStage::Compute), 1});
    pipelineDesc.layout.groups.push(group);
    PipelineHandle pipeline;
    if (!take(gApp.device->createPipeline(pipelineDesc), pipeline,
              "M13 WebGPU compute pipeline creation failed")) {
        cleanup({}, {}, {});
        return false;
    }

    constexpr std::array<std::uint32_t, 4> kParameters{};
    BufferDesc parametersDesc;
    parametersDesc.sizeBytes = sizeof(kParameters);
    parametersDesc.usage = BufferUsage::Uniform;
    BufferHandle parameters;
    if (!take(gApp.device->createBuffer(parametersDesc, std::as_bytes(std::span{kParameters})),
              parameters, "M13 WebGPU parameter buffer creation failed")) {
        cleanup({}, {}, pipeline);
        return false;
    }

    BufferDesc outputDesc;
    outputDesc.sizeBytes = sizeof(NoiseValues);
    outputDesc.usage = BufferUsage::Storage | BufferUsage::TransferSrc;
    BufferHandle output;
    if (!take(gApp.device->createBuffer(outputDesc, {}), output,
              "M13 WebGPU output buffer creation failed")) {
        cleanup(parameters, {}, pipeline);
        return false;
    }

    CommandBuffer commands;
    commands.begin();
    commands.bindPipeline(pipeline);
    commands.bindResource(0, BindingType::UniformBuffer,
                          parameters.index, parameters.generation);
    commands.bindResource(0, BindingType::StorageBuffer, output.index, output.generation);
    commands.dispatch(static_cast<std::uint32_t>(kNoiseSampleCount));
    commands.bufferBarrier(
        output,
        {PipelineStageMask::ComputeShader, ResourceAccessMask::ShaderWrite},
        {PipelineStageMask::Host, ResourceAccessMask::HostRead});
    commands.end();
    if (gApp.device->submit(commands) != GfxError::None) {
        fail("M13 WebGPU compute submission failed");
        cleanup(parameters, output, pipeline);
        return false;
    }

    // WebGPU destroy() keeps previously submitted work alive and reclaims the
    // allocation after that work completes. Exercise that contract directly.
    const bool retired = gApp.device->destroyBuffer(parameters) == GfxError::None &&
                         gApp.device->destroyPipeline(pipeline) == GfxError::None &&
                         gApp.device->destroyShader(shader) == GfxError::None;
    parameters = {};
    pipeline = {};
    shader = {};
    if (!retired) {
        cleanup({}, output, {});
        return false;
    }

    NoiseValues gpuValues{};
    if (gApp.device->readBuffer(
            output, 0, std::as_writable_bytes(std::span{gpuValues})) != GfxError::None) {
        fail("M13 WebGPU compute readback failed");
        cleanup({}, output, {});
        return false;
    }
    const f4::engine::ShaderFloatParityReport report =
        f4::engine::compareShaderFloats(cpuNoiseReference(), gpuValues);
    const bool matches = report.matches() &&
                         report.maxAbsoluteError * 4.0f <= f4::math::kParityTolerance.abs;
    if (!matches) {
        std::fprintf(stderr,
                     "M13 WebGPU parity failed: mismatches=%zu max_abs=%g max_rel=%g sample=%zu\n",
                     report.mismatchCount, static_cast<double>(report.maxAbsoluteError),
                     static_cast<double>(report.maxRelativeError), report.worstSample);
    }

    const bool destroyed = gApp.device->destroyBuffer(output) == GfxError::None;
    return matches && retired && destroyed && gApp.webGpu->validationErrorCount() == 0;
}

auto validateIndexedTriangleStrip() -> bool {
    constexpr std::uint32_t size = 8;
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    PipelineHandle pipeline;
    BufferHandle indexBuffer;
    BufferHandle mismatchBuffer;
    TextureHandle color;
    FramebufferHandle framebuffer;
    auto cleanup = [&] {
        if (framebuffer.isValid()) gApp.device->destroyFramebuffer(framebuffer);
        if (color.isValid()) gApp.device->destroyTexture(color);
        if (mismatchBuffer.isValid()) gApp.device->destroyBuffer(mismatchBuffer);
        if (indexBuffer.isValid()) gApp.device->destroyBuffer(indexBuffer);
        if (pipeline.isValid()) gApp.device->destroyPipeline(pipeline);
        if (fragmentShader.isValid()) gApp.device->destroyShader(fragmentShader);
        if (vertexShader.isValid()) gApp.device->destroyShader(vertexShader);
    };

    ShaderDesc shaderDesc;
    shaderDesc.stage = ShaderStage::Vertex;
    shaderDesc.blob = shaderBlob(kStripVertexWgsl);
    if (!take(gApp.device->createShader(shaderDesc), vertexShader,
              "WebGPU strip vertex shader creation failed")) return false;
    shaderDesc.stage = ShaderStage::Fragment;
    shaderDesc.blob = shaderBlob(kStripFragmentWgsl);
    if (!take(gApp.device->createShader(shaderDesc), fragmentShader,
              "WebGPU strip fragment shader creation failed")) {
        cleanup();
        return false;
    }

    PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.topology = PrimitiveTopology::TriangleStrip;
    pipelineDesc.stripIndexFormat = IndexFormat::U16;
    pipelineDesc.raster.cullMode = CullMode::None;
    if (!take(gApp.device->createPipeline(pipelineDesc), pipeline,
              "WebGPU indexed strip pipeline creation failed")) {
        cleanup();
        return false;
    }

    constexpr std::array<std::uint16_t, 4> indices{0, 1, 2, 3};
    BufferDesc indexDesc;
    indexDesc.sizeBytes = sizeof(indices);
    indexDesc.usage = BufferUsage::Index;
    if (!take(gApp.device->createBuffer(indexDesc, std::as_bytes(std::span{indices})),
              indexBuffer, "WebGPU indexed strip buffer creation failed")) {
        cleanup();
        return false;
    }
    constexpr std::array<std::uint32_t, 4> mismatchIndices{0, 1, 2, 3};
    indexDesc.sizeBytes = sizeof(mismatchIndices);
    if (!take(gApp.device->createBuffer(
                  indexDesc, std::as_bytes(std::span{mismatchIndices})),
              mismatchBuffer, "WebGPU strip mismatch buffer creation failed")) {
        cleanup();
        return false;
    }

    TextureDesc colorDesc;
    colorDesc.width = size;
    colorDesc.height = size;
    if (!take(gApp.device->createTexture(colorDesc), color,
              "WebGPU strip target creation failed")) {
        cleanup();
        return false;
    }
    FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {size, size};
    AttachmentDesc attachment;
    attachment.texture = color;
    attachment.clearOnLoad = true;
    framebufferDesc.attachments.push(attachment);
    if (!take(gApp.device->createFramebuffer(framebufferDesc), framebuffer,
              "WebGPU strip framebuffer creation failed")) {
        cleanup();
        return false;
    }

    auto recordStrip = [&](BufferHandle buffer, IndexFormat format) {
        CommandBuffer commands;
        commands.begin();
        commands.beginRenderPass(framebuffer);
        commands.setViewport({size, size});
        commands.bindPipeline(pipeline);
        commands.bindIndexBuffer(buffer, format);
        commands.drawIndexed(4);
        commands.endRenderPass();
        commands.end();
        return gApp.device->submit(commands);
    };
    if (recordStrip(mismatchBuffer, IndexFormat::U32) != GfxError::BadDescriptor ||
        recordStrip(indexBuffer, IndexFormat::U16) != GfxError::None) {
        cleanup();
        return false;
    }

    std::array<std::byte, size * size * 4u> pixels{};
    if (gApp.webGpu->readTexture(color, 0, pixels) != GfxError::None) {
        cleanup();
        return false;
    }
    const std::size_t lit = std::count_if(
        pixels.begin(), pixels.end(), [](std::byte value) { return value != std::byte{0}; });
    const bool valid = lit > pixels.size() / 2u &&
                       gApp.webGpu->validationErrorCount() == 0;
    cleanup();
    return valid;
}

auto validatePbrShaderFamily() -> bool {
    f4::engine::MaterialCache materials;
    const auto material = materials.getOrCreate({});
    if (materials.ensurePipeline(*gApp.device, material) != GfxError::None) return false;

    VertexInputLayout instanced;
    instanced.bindings.push({0, 36, 0});
    instanced.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    instanced.attributes.push({0, 1, VertexFormat::Float32x3, 12});
    instanced.attributes.push({0, 2, VertexFormat::Float32x2, 24});
    instanced.attributes.push({0, 3, VertexFormat::UNorm8x4, 32});
    instanced.bindings.push({1, 112, 1});
    for (std::uint32_t index = 0; index < 7; ++index) {
        instanced.attributes.push(
            {1, 9u + index, VertexFormat::Float32x4, index * 16u});
    }
    if (materials.ensurePipeline(*gApp.device, material, instanced) != GfxError::None ||
        materials.ensureShadowPipeline(*gApp.device, instanced) != GfxError::None) {
        materials.clear(*gApp.device);
        return false;
    }
    materials.clear(*gApp.device);
    return gApp.webGpu->validationErrorCount() == 0;
}

auto makeRendererTriangle() -> f4::resources::Mesh {
    constexpr std::array<PbrVertex, 3> vertices{{
        {{-0.6f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f},
         {255, 255, 255, 255}},
        {{ 0.6f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f},
         {255, 255, 255, 255}},
        {{ 0.0f,  0.6f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f},
         {255, 255, 255, 255}},
    }};
    constexpr std::array<std::uint16_t, 3> indices{{0, 1, 2}};
    f4::resources::Mesh mesh;
    f4::resources::VertexStream stream;
    stream.strideBytes = sizeof(PbrVertex);
    stream.data.resize(sizeof(vertices));
    std::memcpy(stream.data.data(), vertices.data(), sizeof(vertices));
    mesh.streams.push(std::move(stream));
    mesh.attributes.push({f4::resources::VertexSemantic::Position,
                          f4::resources::VertexFormat::F32_3, 0, 0});
    mesh.attributes.push({f4::resources::VertexSemantic::Normal,
                          f4::resources::VertexFormat::F32_3, 0, 12});
    mesh.attributes.push({f4::resources::VertexSemantic::TexCoord0,
                          f4::resources::VertexFormat::F32_2, 0, 24});
    mesh.attributes.push({f4::resources::VertexSemantic::Color0,
                          f4::resources::VertexFormat::UNORM8_4, 0, 32});
    mesh.indices.resize(sizeof(indices));
    std::memcpy(mesh.indices.data(), indices.data(), sizeof(indices));
    mesh.submeshes.push({0, indices.size()});
    return mesh;
}

auto makeRendererTexture(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
    -> f4::resources::Texture {
    f4::resources::Texture texture;
    texture.format = f4::resources::TextureFormat::RGBA8_UNORM;
    f4::resources::TextureMip mip;
    mip.width = 2;
    mip.height = 2;
    mip.bytes.resize(16);
    for (std::size_t offset = 0; offset < mip.bytes.size(); offset += 4) {
        mip.bytes[offset + 0] = static_cast<std::byte>(red);
        mip.bytes[offset + 1] = static_cast<std::byte>(green);
        mip.bytes[offset + 2] = static_cast<std::byte>(blue);
        mip.bytes[offset + 3] = std::byte{255};
    }
    texture.mips.push(std::move(mip));
    return texture;
}

auto validateRendererFrame() -> bool {
    TextureHandle color;
    TextureHandle depth;
    FramebufferHandle framebuffer;
    auto cleanup = [&]() {
        if (framebuffer.isValid()) gApp.device->destroyFramebuffer(framebuffer);
        if (depth.isValid()) gApp.device->destroyTexture(depth);
        if (color.isValid()) gApp.device->destroyTexture(color);
    };

    TextureDesc colorDesc;
    colorDesc.width = kRendererExtent;
    colorDesc.height = kRendererExtent;
    if (!take(gApp.device->createTexture(colorDesc), color,
              "WebGPU renderer color target creation failed")) return false;
    TextureDesc depthDesc;
    depthDesc.width = kRendererExtent;
    depthDesc.height = kRendererExtent;
    depthDesc.format = TextureFormat::Depth32F;
    if (!take(gApp.device->createTexture(depthDesc), depth,
              "WebGPU renderer depth target creation failed")) {
        cleanup();
        return false;
    }
    FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {kRendererExtent, kRendererExtent};
    AttachmentDesc colorAttachment;
    colorAttachment.texture = color;
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.01f;
    colorAttachment.clear.color[1] = 0.015f;
    colorAttachment.clear.color[2] = 0.02f;
    framebufferDesc.attachments.push(colorAttachment);
    AttachmentDesc depthAttachment;
    depthAttachment.type = AttachmentType::Depth;
    depthAttachment.texture = depth;
    depthAttachment.clearOnLoad = true;
    framebufferDesc.attachments.push(depthAttachment);
    if (!take(gApp.device->createFramebuffer(framebufferDesc), framebuffer,
              "WebGPU renderer framebuffer creation failed")) {
        cleanup();
        return false;
    }

    std::size_t litPixels = 0;
    std::uint32_t digest = 2166136261u;
    bool rendered = false;
    {
        f4::ecs::Registry registry(16);
        f4::engine::Renderer renderer(registry, *gApp.device);
        renderer.pbrSettings().shadowsEnabled = false;
        renderer.pbrSettings().ambientIntensity = 0.2f;
        if (renderer.init() != GfxError::None) {
            fail("WebGPU engine Renderer initialization failed");
        } else {
            const auto albedo = renderer.createTexture(makeRendererTexture(205, 96, 35));
            const auto normal = renderer.createTexture(makeRendererTexture(145, 178, 238));
            const auto metallicRoughness =
                renderer.createTexture(makeRendererTexture(0, 140, 76));
            f4::engine::StandardMaterialDesc materialDesc;
            materialDesc.albedo = albedo;
            materialDesc.normal = normal;
            materialDesc.metallicRoughness = metallicRoughness;
            materialDesc.metallicFactor = 0.8f;
            materialDesc.roughnessFactor = 0.85f;
            const auto material = renderer.createMaterial(materialDesc);
            const auto mesh = renderer.createMesh(makeRendererTriangle());
            const f4::ecs::Entity cameraEntity = registry.create();
            f4::engine::Camera camera;
            camera.eye = {0.0f, 0.0f, 2.0f};
            camera.center = {0.0f, 0.0f, 0.0f};
            registry.emplace<f4::engine::Camera>(cameraEntity, camera);
            const f4::ecs::Entity lightEntity = registry.create();
            f4::engine::DirectionalLight light;
            light.direction = {-0.25f, -0.15f, -1.0f};
            light.color = {1.0f, 0.92f, 0.8f};
            light.intensity = 2.5f;
            registry.emplace<f4::engine::DirectionalLight>(lightEntity, light);
            const f4::ecs::Entity object = registry.create();
            registry.emplace<f4::ecs::LocalTransform>(object, f4::ecs::LocalTransform{});
            registry.emplace<f4::engine::MeshRenderer>(
                object, f4::engine::MeshRenderer{mesh, material, 0, false, false});
            f4::ecs::updateWorldTransforms(registry);

            gApp.device->resetFrameStats();
            rendered = albedo.isValid() && normal.isValid() &&
                       metallicRoughness.isValid() && material.isValid() && mesh.isValid() &&
                       renderer.render(framebuffer, cameraEntity, 1.0f) == GfxError::None;
            if (rendered) {
                gApp.device->waitIdle();
                rendered = gApp.webGpu->readTexture(color, 0, gApp.rendererCapture) ==
                               GfxError::None &&
                           renderer.stats().draws == 1 && renderer.stats().instances == 1;
            }
        }
    }

    if (rendered) {
        for (std::size_t offset = 0; offset < gApp.rendererCapture.size(); offset += 4) {
            const auto red = std::to_integer<std::uint8_t>(gApp.rendererCapture[offset + 0]);
            const auto green = std::to_integer<std::uint8_t>(gApp.rendererCapture[offset + 1]);
            const auto blue = std::to_integer<std::uint8_t>(gApp.rendererCapture[offset + 2]);
            if (red > 20 || green > 20 || blue > 20) ++litPixels;
        }
        for (const std::byte value : gApp.rendererCapture) {
            digest ^= std::to_integer<std::uint8_t>(value);
            digest *= 16777619u;
        }
        rendered = litPixels > 100 && gApp.webGpu->validationErrorCount() == 0;
    }
    cleanup();
    if (rendered) {
        char script[256]{};
        std::snprintf(
            script, sizeof(script),
            "document.body.dataset.f4Renderer='true';"
            "document.body.dataset.f4RendererPixels='%zu';"
            "document.body.dataset.f4RendererHash='%08x';",
            litPixels, digest);
        emscripten_run_script(script);
    }
    return rendered;
}

auto setValidationScene(std::uint32_t index) -> bool {
    if (index > 8) return false;
    if (index == gApp.validationSceneIndex) return true;
    gApp.validationSceneIndex = index;
    if (f4::demo::IDemoScene* scene = validationScene(index)) {
        scene->onResize(gApp.extent.width, gApp.extent.height);
    }
    gApp.frameCount = 0;
    char script[256]{};
    std::snprintf(
        script, sizeof(script),
        "delete document.body.dataset.f4Ready;"
        "delete document.body.dataset.f4ValidationScene;"
        "document.body.dataset.f4SceneIndex='%u';"
        "document.getElementById('status').textContent='loading WebGPU scene %u...';",
        index, index);
    emscripten_run_script(script);
    return true;
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE auto f4RendererCaptureData() -> const std::byte* {
    return gApp.rendererCapture.data();
}

extern "C" EMSCRIPTEN_KEEPALIVE auto f4RendererCaptureSize() -> std::size_t {
    return gApp.rendererCapture.size();
}

extern "C" EMSCRIPTEN_KEEPALIVE auto f4SetValidationScene(std::uint32_t index) -> int {
    return setValidationScene(index) ? 1 : 0;
}

int main() {
    emscripten_set_canvas_element_size("#canvas", static_cast<int>(kCanvasExtent.width),
                                       static_cast<int>(kCanvasExtent.height));

    gApp.device = createDevice();
    if (gApp.device == nullptr || gApp.device->backend() != Backend::WebGPU) {
        fail("Browser did not provide a WebGPU device");
        return 1;
    }
    gApp.webGpu = static_cast<WebGpuDevice*>(gApp.device);
    if (!gApp.webGpu->supportsSurfaceFormat(gApp.surfaceFormat)) {
        fail("Browser WebGPU surface does not support RGBA8_UNORM");
        return 1;
    }

    if (!validateNoiseParity()) {
        fail("M13 paired-u32 CPU/WebGPU parity failed");
        return 1;
    }
    EM_ASM({
        document.body.dataset.f4NoiseParity = 'true';
        document.body.dataset.f4Retirement = 'true';
    });

    if (!validatePbrShaderFamily()) {
        fail("M11 WebGPU PBR/shadow shader family validation failed");
        return 1;
    }
    EM_ASM({ document.body.dataset.f4PbrShaders = 'true'; });

    if (!validateIndexedTriangleStrip()) {
        fail("WebGPU indexed triangle-strip conformance failed");
        return 1;
    }
    EM_ASM({ document.body.dataset.f4TriangleStrip = 'true'; });

    if (!validateRendererFrame()) {
        fail("M11 WebGPU engine Renderer frame validation failed");
        return 1;
    }

    ShaderDesc vertexDesc;
    vertexDesc.stage = ShaderStage::Vertex;
    vertexDesc.blob = shaderBlob(f4::web::kPbrVertexWgsl);
    if (!take(gApp.device->createShader(vertexDesc), gApp.vertexShader,
              "WGSL vertex module creation failed")) return 1;

    ShaderDesc fragmentDesc;
    fragmentDesc.stage = ShaderStage::Fragment;
    fragmentDesc.blob = shaderBlob(f4::web::kPbrFragmentWgsl);
    if (!take(gApp.device->createShader(fragmentDesc), gApp.fragmentShader,
              "WGSL fragment module creation failed")) return 1;

    BufferDesc vertexBufferDesc;
    vertexBufferDesc.sizeBytes = sizeof(kCubeVertices);
    vertexBufferDesc.usage = BufferUsage::Vertex;
    if (!take(gApp.device->createBuffer(
                  vertexBufferDesc, std::as_bytes(std::span{kCubeVertices})),
              gApp.vertexBuffer, "WebGPU PBR cube vertex buffer creation failed")) return 1;

    BufferDesc indexBufferDesc;
    indexBufferDesc.sizeBytes = sizeof(kCubeIndices);
    indexBufferDesc.usage = BufferUsage::Index;
    if (!take(gApp.device->createBuffer(
                  indexBufferDesc, std::as_bytes(std::span{kCubeIndices})),
              gApp.indexBuffer, "WebGPU PBR cube index buffer creation failed")) return 1;

    const PbrSceneUbo initialConstants = pbrSceneConstants(kCanvasExtent);
    BufferDesc uniformDesc;
    uniformDesc.sizeBytes = sizeof(initialConstants);
    uniformDesc.usage = BufferUsage::Uniform;
    if (!take(gApp.device->createBuffer(
                  uniformDesc, std::as_bytes(std::span{&initialConstants, 1})),
              gApp.uniformBuffer,
              "WebGPU uniform buffer creation failed")) return 1;

    TextureDesc textureDesc;
    textureDesc.format = TextureFormat::RGBA8_UNORM;
    textureDesc.width = 1;
    textureDesc.height = 1;
    for (std::size_t slot = 0; slot < kPbrPixels.size(); ++slot) {
        if (!take(gApp.device->createTexture(textureDesc), gApp.textures[slot],
                  "WebGPU PBR texture creation failed") ||
            gApp.device->writeTexture(gApp.textures[slot], 0, kPbrPixels[slot]) !=
                GfxError::None) {
            fail("WebGPU PBR texture upload failed");
            return 1;
        }
    }
    textureDesc.format = TextureFormat::R16_FLOAT;
    if (!take(gApp.device->createTexture(textureDesc), gApp.textures[6],
              "WebGPU shadow sample texture creation failed") ||
        gApp.device->writeTexture(gApp.textures[6], 0, kShadowPixel) != GfxError::None) {
        fail("WebGPU shadow sample texture upload failed");
        return 1;
    }

    SamplerDesc samplerDesc;
    if (!take(gApp.device->createSampler(samplerDesc), gApp.sampler,
              "WebGPU sampler creation failed")) return 1;

    PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = gApp.vertexShader;
    pipelineDesc.fragmentShader = gApp.fragmentShader;
    pipelineDesc.raster.cullMode = CullMode::Back;
    pipelineDesc.depthStencil.depthTestEnable = true;
    pipelineDesc.depthStencil.depthWriteEnable = true;
    pipelineDesc.depthStencil.format = TextureFormat::Depth32F;
    pipelineDesc.vertexInput.bindings.push({0, sizeof(PbrVertex), 0});
    pipelineDesc.vertexInput.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    pipelineDesc.vertexInput.attributes.push({0, 1, VertexFormat::Float32x3, 12});
    pipelineDesc.vertexInput.attributes.push({0, 2, VertexFormat::Float32x2, 24});
    pipelineDesc.vertexInput.attributes.push({0, 3, VertexFormat::UNorm8x4, 32});
    BindGroup resources;
    resources.bindPoint = 0;
    resources.slots.push(BindingSlot{
        0, BindingType::UniformBuffer,
        shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment), 1});
    for (std::uint32_t slot = 0; slot < gApp.textures.size(); ++slot) {
        resources.slots.push(BindingSlot{
            slot, BindingType::Texture, shaderStageBit(ShaderStage::Fragment), 1});
        resources.slots.push(BindingSlot{
            slot, BindingType::Sampler, shaderStageBit(ShaderStage::Fragment), 1});
    }
    pipelineDesc.layout.groups.push(std::move(resources));
    ColorAttachmentState colorState;
    colorState.format = gApp.surfaceFormat;
    pipelineDesc.colorAttachments.push(colorState);
    if (!take(gApp.device->createPipeline(pipelineDesc), gApp.pipeline,
              "WebGPU render pipeline creation failed")) return 1;

    if (!recreateTargets(canvasExtent())) {
        fail("WebGPU canvas configuration failed");
        return 1;
    }

    for (std::uint32_t index = 1; index <= 8; ++index) {
        f4::demo::IDemoScene* scene = validationScene(index);
        if (scene == nullptr || !scene->init(gApp.device, nullptr)) {
            fail("WebGPU validation scene family initialization failed");
            return 1;
        }
    }

    emscripten_set_main_loop(renderFrame, 0, true);
    return 0;
}
