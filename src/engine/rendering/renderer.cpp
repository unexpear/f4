#include "engine/rendering/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "core/ecs/transform.h"
#include "core/ecs/view.h"
#include "core/math/frustum.h"
#include "core/math/mat4.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/light.h"
#include "engine/lighting/point_shadows.h"
#include "engine/rendering/effect_shaders.h"
#include "graphics/buffer/buffer_desc.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/texture/texture_desc.h"

namespace f4::engine {

namespace {

constexpr std::uint32_t kRenderInstanceStride = 112;

struct SceneUbo {
    math::Mat4 viewProj;
    math::Mat4 model;
    math::Mat4 normalMatrix;
    math::Mat4 lightViewProj;
    math::Vec4 baseColorFactor;
    math::Vec4 emissiveMetallic;
    math::Vec4 cameraExposure;
    math::Vec4 lightDirectionIntensity;
    math::Vec4 lightColorRoughness;
    math::Vec4 materialParams;
    math::Vec4 renderParams;
    math::Vec4 ambientFlags;
};
static_assert(sizeof(SceneUbo) == 384, "SceneUbo must match std140/HLSL layout");

struct ShadowUbo {
    math::Mat4 lightViewProj;
    math::Mat4 model;
    math::Vec4 alphaParams;
};
static_assert(sizeof(ShadowUbo) == 144, "ShadowUbo must match std140/HLSL layout");

struct PointShadowUbo {
    math::Mat4 faceViewProj;
    math::Mat4 reserved; // unused: casters are instanced (model in the instance stream)
    math::Vec4 lightPositionInvRange;
    math::Vec4 tileTransform;
    math::Vec4 alphaParams;
};
static_assert(sizeof(PointShadowUbo) == 176, "PointShadowUbo must match std140/HLSL layout");

struct BloomUbo {
    math::Vec4 params0; // source texel size xy, threshold, knee
    math::Vec4 params1; // mode, intensity, 1 / target size xy
};
static_assert(sizeof(BloomUbo) == 32, "BloomUbo must match std140/HLSL layout");

constexpr std::uint32_t kBloomMaxLevels = 8;

[[nodiscard]] auto floatWord(float value) -> std::uint32_t {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] auto pointShadowGroupKey(resources::MeshHandle mesh,
                                       resources::MaterialHandle material,
                                       std::uint32_t subMesh) -> std::array<std::uint64_t, 3> {
    return {(static_cast<std::uint64_t>(mesh.index) << 32) | mesh.generation,
            (static_cast<std::uint64_t>(material.index) << 32) | material.generation, subMesh};
}

// FNV-1a over the exact bits of everything a shadow tile depends on.
struct SignatureHash {
    std::uint64_t value = 0xcbf29ce484222325ull;
    void add(std::uint32_t word) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            value ^= (word >> shift) & 0xffu;
            value *= 0x100000001b3ull;
        }
    }
    void add(float number) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &number, sizeof(bits));
        add(bits);
    }
    void add(const math::Vec3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(const math::Vec4& v) {
        add(v.x);
        add(v.y);
        add(v.z);
        add(v.w);
    }
};

[[nodiscard]] auto sphereTouchesBox(const math::Vec3& center, float radius,
                                    const math::AABB& box) -> bool {
    const math::Vec3 closest(std::clamp(center.x, box.min.x, box.max.x),
                             std::clamp(center.y, box.min.y, box.max.y),
                             std::clamp(center.z, box.min.z, box.max.z));
    return math::lengthSq(closest - center) <= radius * radius;
}

[[nodiscard]] auto createRenderTexture(gfx::IDevice& device, gfx::TextureFormat format,
                                       gfx::Extent extent, const float (&clear)[4],
                                       float clearDepth)
    -> Result<gfx::TextureHandle, gfx::GfxError> {
    gfx::TextureDesc desc;
    desc.format = format;
    desc.width = extent.width;
    desc.height = extent.height;
    desc.hasOptimizedClear = true;
    for (std::size_t i = 0; i < 4; ++i) desc.optimizedClear.color[i] = clear[i];
    desc.optimizedClear.depth = clearDepth;
    return device.createTexture(desc);
}

inline constexpr std::uint32_t kInstanceModelLocation = 9;
inline constexpr std::uint32_t kInstanceNormalLocation = 13;

struct FrameLight {
    math::Vec3 direction{0.0f, -1.0f, 0.0f};
    math::Vec3 color{1.0f};
    float intensity = 0.0f;
    math::Mat4 viewProj = math::Mat4::identity();
    bool valid = false;
};

[[nodiscard]] auto translateTextureFormat(resources::TextureFormat source,
                                          gfx::TextureFormat& target) -> bool {
    switch (source) {
    case resources::TextureFormat::R8_UNORM: target = gfx::TextureFormat::R8_UNORM; return true;
    case resources::TextureFormat::RG8_UNORM: target = gfx::TextureFormat::RG8_UNORM; return true;
    case resources::TextureFormat::RGBA8_UNORM: target = gfx::TextureFormat::RGBA8_UNORM; return true;
    case resources::TextureFormat::BGRA8_UNORM: target = gfx::TextureFormat::BGRA8_UNORM; return true;
    case resources::TextureFormat::R16_FLOAT: target = gfx::TextureFormat::R16_FLOAT; return true;
    case resources::TextureFormat::RGBA16_FLOAT: target = gfx::TextureFormat::RGBA16_FLOAT; return true;
    case resources::TextureFormat::RGBA32_FLOAT: target = gfx::TextureFormat::RGBA32_FLOAT; return true;
    default: return false;
    }
}

[[nodiscard]] auto createRgba8Pixel(gfx::IDevice& device,
                                    const std::uint8_t (&rgba)[4])
    -> Result<gfx::TextureHandle, gfx::GfxError> {
    gfx::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = gfx::TextureFormat::RGBA8_UNORM;
    auto texture = device.createTexture(desc);
    if (!texture.hasValue()) return texture.error();
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(rgba), sizeof(rgba));
    if (const auto error = device.writeTexture(texture.value(), 0, bytes);
        error != gfx::GfxError::None) {
        (void)device.destroyTexture(texture.value());
        return error;
    }
    return texture.value();
}

struct EnvironmentTexture {
    gfx::TextureHandle handle{};
    std::uint32_t mipCount = 1;
};

[[nodiscard]] auto createDefaultEnvironment(gfx::IDevice& device)
    -> Result<EnvironmentTexture, gfx::GfxError> {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const std::uint32_t mipCount = gfx::maxTextureMipLevels(width, height);
    gfx::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = mipCount;
    desc.format = gfx::TextureFormat::RGBA32_FLOAT;
    auto result = device.createTexture(desc);
    if (!result.hasValue()) return result.error();
    const gfx::TextureHandle texture = result.value();

    constexpr float pi = 3.14159265358979323846f;
    const math::Vec3 sunDirection = math::normalized(math::Vec3(-0.35f, 0.82f, 0.45f));
    for (std::uint32_t mip = 0; mip < mipCount; ++mip) {
        const std::uint32_t mipWidth = std::max(1u, width >> mip);
        const std::uint32_t mipHeight = std::max(1u, height >> mip);
        const float roughness = static_cast<float>(mip) /
                                static_cast<float>(std::max(1u, mipCount - 1));
        containers::Array<float> pixels;
        pixels.resize(static_cast<std::size_t>(mipWidth) * mipHeight * 4u);
        for (std::uint32_t y = 0; y < mipHeight; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(mipHeight);
            const float theta = v * pi;
            for (std::uint32_t x = 0; x < mipWidth; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) /
                                static_cast<float>(mipWidth);
                const float phi = (u - 0.5f) * 2.0f * pi;
                const math::Vec3 direction(std::sin(theta) * std::sin(phi),
                                           std::cos(theta),
                                           std::sin(theta) * std::cos(phi));
                const float sky = std::clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);
                math::Vec3 color = math::lerp(math::Vec3(0.22f, 0.18f, 0.14f),
                                              math::Vec3(0.16f, 0.42f, 0.82f), sky);
                const float horizon = std::pow(1.0f - std::abs(direction.y), 8.0f);
                color += math::Vec3(0.24f, 0.15f, 0.08f) * horizon;
                const float sunPower = 2.0f + (1.0f - roughness) * 510.0f;
                const float sun = std::pow(std::max(math::dot(direction, sunDirection), 0.0f),
                                           sunPower);
                color += math::Vec3(7.0f, 5.2f, 3.2f) * sun * (1.0f - roughness * 0.75f);
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * mipWidth + x) * 4u;
                pixels[offset + 0] = color.x;
                pixels[offset + 1] = color.y;
                pixels[offset + 2] = color.z;
                pixels[offset + 3] = 1.0f;
            }
        }
        const auto bytes = std::span(
            reinterpret_cast<const std::byte*>(pixels.data()),
            pixels.size() * sizeof(float));
        if (const auto error = device.writeTexture(texture, mip, bytes);
            error != gfx::GfxError::None) {
            (void)device.destroyTexture(texture);
            return error;
        }
    }
    return EnvironmentTexture{texture, mipCount};
}

[[nodiscard]] auto findFrameLight(ecs::Registry& registry, const Camera& camera,
                                  const PbrRenderSettings& settings) -> FrameLight {
    FrameLight output;
    for (auto [entity, light] : ecs::View<DirectionalLight>(registry)) {
        (void)entity;
        if (!output.valid) {
            output.direction = math::normalized(light.direction);
            if (math::lengthSq(output.direction) == 0.0f) {
                output.direction = {0.0f, -1.0f, 0.0f};
            }
            output.color = math::maxComponentwise(light.color, math::Vec3(0.0f));
            output.intensity = std::max(light.intensity, 0.0f);
            output.valid = true;
        }
    }
    if (!output.valid) return output;

    const float nearPlane = std::max(settings.shadowNear, 0.001f);
    const float farPlane = std::max(settings.shadowFar, nearPlane + 0.001f);
    const float extent = std::max(settings.shadowExtent, 0.01f);
    const math::Vec3 target = camera.center;
    const math::Vec3 eye = target - output.direction * (farPlane * 0.5f);
    const math::Vec3 worldUp = std::abs(math::dot(output.direction, math::Vec3(0.0f, 1.0f, 0.0f))) >
                                       0.98f
                                   ? math::Vec3(0.0f, 0.0f, 1.0f)
                                   : math::Vec3(0.0f, 1.0f, 0.0f);
    const math::Mat4 view = math::Mat4::lookAtRH(eye, target, worldUp);
    const math::Mat4 projection = math::Mat4::orthoRH_ZO(
        -extent, extent, -extent, extent, nearPlane, farPlane);
    output.viewProj = projection.mul(view);
    return output;
}

[[nodiscard]] auto isBlended(const StandardMaterialDesc& desc) -> bool {
    return desc.transparent || desc.alphaMode == MaterialAlphaMode::Blend;
}

[[nodiscard]] auto hasSemantic(const resources::Mesh& mesh,
                               resources::VertexSemantic semantic) -> bool {
    return mesh.attributeFor(semantic) != nullptr;
}

[[nodiscard]] auto completePbrVertexInputs(gfx::IDevice& device,
                                           const resources::Mesh& mesh,
                                           GpuMesh& gpu) -> gfx::GfxError {
    const bool needsNormal = !hasSemantic(mesh, resources::VertexSemantic::Normal);
    const bool needsUv = !hasSemantic(mesh, resources::VertexSemantic::TexCoord0);
    const bool needsColor = !hasSemantic(mesh, resources::VertexSemantic::Color0);
    if (!needsNormal && !needsUv && !needsColor) return gfx::GfxError::None;

    struct FallbackVertex {
        float normal[3];
        float uv[2];
        std::uint8_t color[4];
    };
    containers::Array<FallbackVertex> vertices;
    vertices.resize(gpu.vertexCount);
    for (FallbackVertex& vertex : vertices) {
        vertex.normal[0] = 0.0f;
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 1.0f;
        vertex.uv[0] = 0.0f;
        vertex.uv[1] = 0.0f;
        vertex.color[0] = 255;
        vertex.color[1] = 255;
        vertex.color[2] = 255;
        vertex.color[3] = 255;
    }
    const std::size_t byteCount = vertices.size() * sizeof(FallbackVertex);
    if (byteCount > std::numeric_limits<std::uint32_t>::max()) {
        return gfx::GfxError::BadDescriptor;
    }
    gfx::BufferDesc desc;
    desc.sizeBytes = static_cast<std::uint32_t>(byteCount);
    desc.usage = gfx::BufferUsage::Vertex;
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(vertices.data()),
                                 byteCount);
    auto buffer = device.createBuffer(desc, bytes);
    if (!buffer.hasValue()) return buffer.error();

    const std::uint32_t binding = static_cast<std::uint32_t>(gpu.vertexBuffers.size());
    gpu.vertexBuffers.push(buffer.value());
    gpu.layout.bindings.push(
        {binding, static_cast<std::uint32_t>(sizeof(FallbackVertex)), 0});
    if (needsNormal) {
        gpu.layout.attributes.push({binding, 1, gfx::VertexFormat::Float32x3, 0});
    }
    if (needsUv) {
        gpu.layout.attributes.push({binding, 2, gfx::VertexFormat::Float32x2, 12});
    }
    if (needsColor) {
        gpu.layout.attributes.push({binding, 3, gfx::VertexFormat::UNorm8x4, 20});
    }
    return gfx::GfxError::None;
}

void bindMesh(gfx::CommandBuffer& commandBuffer, const GpuMesh& mesh) {
    for (std::size_t bindingIndex = 0; bindingIndex < mesh.vertexBuffers.size();
         ++bindingIndex) {
        const auto& binding = mesh.layout.bindings[bindingIndex];
        commandBuffer.bindVertexBuffer(static_cast<std::uint32_t>(bindingIndex),
                                       mesh.vertexBuffers[bindingIndex],
                                       binding.strideBytes);
    }
    if (mesh.indexBuffer.isValid()) {
        commandBuffer.bindIndexBuffer(mesh.indexBuffer, mesh.indexFormat);
    }
}

[[nodiscard]] auto makeInstancedLayout(const GpuMesh& mesh,
                                       std::uint32_t& instanceBinding)
    -> gfx::VertexInputLayout {
    gfx::VertexInputLayout layout = mesh.layout;
    instanceBinding = 0;
    for (const auto& binding : layout.bindings) {
        instanceBinding = std::max(instanceBinding, binding.binding + 1u);
    }
    layout.bindings.push({instanceBinding, kRenderInstanceStride, 1});
    for (std::uint32_t column = 0; column < 4; ++column) {
        layout.attributes.push({instanceBinding, kInstanceModelLocation + column,
                                gfx::VertexFormat::Float32x4, column * 16u});
    }
    for (std::uint32_t column = 0; column < 3; ++column) {
        layout.attributes.push({instanceBinding, kInstanceNormalLocation + column,
                                gfx::VertexFormat::Float32x4, 64u + column * 16u});
    }
    return layout;
}

void drawMesh(gfx::CommandBuffer& commandBuffer, const resources::Mesh& mesh,
              const GpuMesh& gpu, std::uint32_t subMeshIndex,
              std::uint32_t instanceCount = 1) {
    if (!gpu.indexBuffer.isValid()) {
        commandBuffer.draw(gpu.vertexCount, instanceCount);
        return;
    }
    if (subMeshIndex < mesh.submeshes.size()) {
        const auto& subMesh = mesh.submeshes[subMeshIndex];
        if (subMesh.firstIndex <= std::numeric_limits<std::uint32_t>::max() &&
            subMesh.indexCount <= std::numeric_limits<std::uint32_t>::max() &&
            subMesh.firstIndex <= mesh.indexCount() &&
            subMesh.indexCount <= mesh.indexCount() - subMesh.firstIndex) {
            commandBuffer.drawIndexed(static_cast<std::uint32_t>(subMesh.indexCount),
                                      instanceCount,
                                      static_cast<std::uint32_t>(subMesh.firstIndex));
            return;
        }
    }
    commandBuffer.drawIndexed(gpu.indexCount, instanceCount);
}

} // namespace

Renderer::Renderer(ecs::Registry& registry, gfx::IDevice& device)
    : registry_(registry), device_(device), meshArena_(64), textureArena_(128) {}

void Renderer::releaseInitializationResources() {
    if (shadowFramebuffer_.isValid()) (void)device_.destroyFramebuffer(shadowFramebuffer_);
    if (shadowColorTex_.isValid()) (void)device_.destroyTexture(shadowColorTex_);
    if (shadowDepthTex_.isValid()) (void)device_.destroyTexture(shadowDepthTex_);
    if (defaultEnvironmentTex_.isValid()) (void)device_.destroyTexture(defaultEnvironmentTex_);
    if (blackTex_.isValid()) (void)device_.destroyTexture(blackTex_);
    if (neutralNormalTex_.isValid()) (void)device_.destroyTexture(neutralNormalTex_);
    if (whiteTex_.isValid()) (void)device_.destroyTexture(whiteTex_);
    if (shadowSampler_.isValid()) (void)device_.destroySampler(shadowSampler_);
    if (environmentSampler_.isValid()) (void)device_.destroySampler(environmentSampler_);
    if (surfaceSampler_.isValid()) (void)device_.destroySampler(surfaceSampler_);
    shadowFramebuffer_ = {};
    shadowColorTex_ = {};
    shadowDepthTex_ = {};
    defaultEnvironmentTex_ = {};
    blackTex_ = {};
    neutralNormalTex_ = {};
    whiteTex_ = {};
    shadowSampler_ = {};
    environmentSampler_ = {};
    surfaceSampler_ = {};
    releasePointShadowAtlas();
    releaseBloomTargets();
    if (bloomHdrPipeline_.isValid()) (void)device_.destroyPipeline(bloomHdrPipeline_);
    if (bloomCompositePipeline_.isValid()) (void)device_.destroyPipeline(bloomCompositePipeline_);
    if (bloomVs_.isValid()) (void)device_.destroyShader(bloomVs_);
    if (bloomFs_.isValid()) (void)device_.destroyShader(bloomFs_);
    if (bloomTriangle_.isValid()) (void)device_.destroyBuffer(bloomTriangle_);
    if (bloomSampler_.isValid()) (void)device_.destroySampler(bloomSampler_);
    bloomHdrPipeline_ = {};
    bloomCompositePipeline_ = {};
    bloomVs_ = {};
    bloomFs_ = {};
    bloomTriangle_ = {};
    bloomSampler_ = {};
    bloomConstants_.destroy(device_);
    pointShadowConstants_.destroy(device_);
    pointShadowInstanceBuffers_.destroy(device_);
    shadowConstants_.destroy(device_);
    sceneConstants_.destroy(device_);
    sceneInstances_.destroy(device_);
    pointLightBuffers_.destroy(device_);
    pointLightGpuBuffers_.destroy(device_);
    lightBinningConstants_.destroy(device_);
    if (lightBinningPipeline_.isValid()) (void)device_.destroyPipeline(lightBinningPipeline_);
    if (lightBinningShader_.isValid()) (void)device_.destroyShader(lightBinningShader_);
    lightBinningPipeline_ = {};
    lightBinningShader_ = {};
    lightBinningUnavailable_ = false;
    framePointLightsReady_ = false;
    framePointLightInput_ = {};
    framePointLights_ = {};
    defaultEnvironmentMipCount_ = 1;
    inited_ = false;
}

Renderer::~Renderer() {
    device_.waitIdle();
    (void)graph_.release(device_);
    materials_.clear(device_);
    for (auto& item : meshMap_) destroyGpuMesh(device_, item.second.gpu);
    meshMap_.clear();
    meshArena_.clear();
    for (auto& item : textureMap_) {
        if (item.second.gpu.isValid()) (void)device_.destroyTexture(item.second.gpu);
    }
    textureMap_.clear();
    textureArena_.clear();
    releaseInitializationResources();
}

auto Renderer::init() -> gfx::GfxError {
    if (inited_) return gfx::GfxError::None;
    const auto fail = [this](gfx::GfxError error) {
        releaseInitializationResources();
        return error;
    };
    if (const auto error = sceneConstants_.create(device_, sizeof(SceneUbo), 64);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = shadowConstants_.create(device_, sizeof(ShadowUbo), 64);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = pointShadowConstants_.create(device_, sizeof(PointShadowUbo), 64);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = bloomConstants_.create(device_, sizeof(BloomUbo), 16);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = sceneInstances_.create(device_, gfx::BufferUsage::Vertex);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = pointShadowInstanceBuffers_.create(device_, gfx::BufferUsage::Vertex);
        error != gfx::GfxError::None) {
        return fail(error);
    }
    if (const auto error = pointLightBuffers_.create(
            device_, gfx::BufferUsage::Storage | gfx::BufferUsage::HostWrite);
        error != gfx::GfxError::None) {
        return fail(error);
    }

    const std::uint8_t white[4] = {255, 255, 255, 255};
    const std::uint8_t neutralNormal[4] = {128, 128, 255, 255};
    const std::uint8_t black[4] = {0, 0, 0, 255};
    auto whiteResult = createRgba8Pixel(device_, white);
    if (!whiteResult.hasValue()) return fail(whiteResult.error());
    whiteTex_ = whiteResult.value();
    auto normalResult = createRgba8Pixel(device_, neutralNormal);
    if (!normalResult.hasValue()) return fail(normalResult.error());
    neutralNormalTex_ = normalResult.value();
    auto blackResult = createRgba8Pixel(device_, black);
    if (!blackResult.hasValue()) return fail(blackResult.error());
    blackTex_ = blackResult.value();

    auto environmentResult = createDefaultEnvironment(device_);
    if (!environmentResult.hasValue()) return fail(environmentResult.error());
    defaultEnvironmentTex_ = environmentResult.value().handle;
    defaultEnvironmentMipCount_ = environmentResult.value().mipCount;

    shadowResolution_ = std::max(1u, pbrSettings_.shadowResolution);
    gfx::TextureDesc shadowColorDesc;
    shadowColorDesc.width = shadowResolution_;
    shadowColorDesc.height = shadowResolution_;
    shadowColorDesc.format = gfx::TextureFormat::R16_FLOAT;
    shadowColorDesc.hasOptimizedClear = true;
    shadowColorDesc.optimizedClear.color[0] = 1.0f;
    shadowColorDesc.optimizedClear.color[1] = 1.0f;
    shadowColorDesc.optimizedClear.color[2] = 1.0f;
    shadowColorDesc.optimizedClear.color[3] = 1.0f;
    auto shadowColor = device_.createTexture(shadowColorDesc);
    if (!shadowColor.hasValue()) return fail(shadowColor.error());
    shadowColorTex_ = shadowColor.value();
    containers::Array<std::uint16_t> clearShadow;
    clearShadow.resize(static_cast<std::size_t>(shadowResolution_) * shadowResolution_);
    std::fill(clearShadow.begin(), clearShadow.end(), static_cast<std::uint16_t>(0x3c00u));
    if (const auto error = device_.writeTexture(
            shadowColorTex_, 0,
            std::span(reinterpret_cast<const std::byte*>(clearShadow.data()),
                      clearShadow.size() * sizeof(std::uint16_t)));
        error != gfx::GfxError::None) {
        return fail(error);
    }
    gfx::TextureDesc shadowDepthDesc;
    shadowDepthDesc.width = shadowResolution_;
    shadowDepthDesc.height = shadowResolution_;
    shadowDepthDesc.format = gfx::TextureFormat::Depth32F;
    shadowDepthDesc.hasOptimizedClear = true;
    shadowDepthDesc.optimizedClear.depth = 1.0f;
    auto shadowDepth = device_.createTexture(shadowDepthDesc);
    if (!shadowDepth.hasValue()) return fail(shadowDepth.error());
    shadowDepthTex_ = shadowDepth.value();

    gfx::SamplerDesc surfaceDesc;
    surfaceDesc.mipFilter = gfx::MipFilter::Linear;
    surfaceDesc.addressU = gfx::AddressMode::Repeat;
    surfaceDesc.addressV = gfx::AddressMode::Repeat;
    auto surface = device_.createSampler(surfaceDesc);
    if (!surface.hasValue()) return fail(surface.error());
    surfaceSampler_ = surface.value();
    gfx::SamplerDesc environmentDesc = surfaceDesc;
    environmentDesc.addressV = gfx::AddressMode::ClampToEdge;
    auto environment = device_.createSampler(environmentDesc);
    if (!environment.hasValue()) return fail(environment.error());
    environmentSampler_ = environment.value();
    gfx::SamplerDesc shadowDesc;
    shadowDesc.addressU = gfx::AddressMode::ClampToEdge;
    shadowDesc.addressV = gfx::AddressMode::ClampToEdge;
    auto shadow = device_.createSampler(shadowDesc);
    if (!shadow.hasValue()) return fail(shadow.error());
    shadowSampler_ = shadow.value();

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {shadowResolution_, shadowResolution_};
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.type = gfx::AttachmentType::Color;
    colorAttachment.texture = shadowColorTex_;
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 1.0f;
    colorAttachment.clear.color[1] = 1.0f;
    colorAttachment.clear.color[2] = 1.0f;
    colorAttachment.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = shadowDepthTex_;
    depthAttachment.clearOnLoad = true;
    depthAttachment.clear.depth = 1.0f;
    framebufferDesc.attachments.push(depthAttachment);
    auto framebuffer = device_.createFramebuffer(framebufferDesc);
    if (!framebuffer.hasValue()) return fail(framebuffer.error());
    shadowFramebuffer_ = framebuffer.value();

    inited_ = true;
    return gfx::GfxError::None;
}

auto Renderer::createMesh(const resources::Mesh& mesh) -> resources::MeshHandle {
    auto gpuResult = createGpuMesh(device_, mesh);
    if (!gpuResult.hasValue()) return {};
    GpuMesh gpu = static_cast<GpuMesh&&>(gpuResult.value());
    if (completePbrVertexInputs(device_, mesh, gpu) != gfx::GfxError::None) {
        destroyGpuMesh(device_, gpu);
        return {};
    }
    const auto handle = meshArena_.acquire(mesh);
    MeshEntry entry;
    entry.mesh = mesh;
    entry.mesh.bounds = resources::computeBounds(entry.mesh);
    entry.gpu = static_cast<GpuMesh&&>(gpu);
    entry.instancedLayout = makeInstancedLayout(entry.gpu, entry.instanceBinding);
    meshMap_.set(handle, static_cast<MeshEntry&&>(entry));
    return handle;
}

void Renderer::destroyMesh(resources::MeshHandle handle) {
    auto* entry = meshMap_.find(handle);
    if (entry != nullptr) {
        destroyGpuMesh(device_, entry->gpu);
        meshMap_.erase(handle);
    }
    if (meshArena_.contains(handle)) meshArena_.release(handle);
}

auto Renderer::createTexture(const resources::Texture& texture) -> resources::TextureHandle {
    if (texture.mips.isEmpty() || resources::isBlockCompressed(texture.format)) return {};
    gfx::TextureFormat format{};
    if (!translateTextureFormat(texture.format, format)) return {};
    const auto& first = texture.mips[0];
    if (first.width == 0 || first.height == 0 ||
        texture.mips.size() > gfx::maxTextureMipLevels(first.width, first.height)) {
        return {};
    }
    std::uint32_t expectedWidth = first.width;
    std::uint32_t expectedHeight = first.height;
    for (const auto& mip : texture.mips) {
        if (mip.width != expectedWidth || mip.height != expectedHeight ||
            mip.bytes.size() != resources::textureSliceBytes(mip.width, mip.height,
                                                              texture.format)) {
            return {};
        }
        expectedWidth = std::max(1u, expectedWidth >> 1u);
        expectedHeight = std::max(1u, expectedHeight >> 1u);
    }

    gfx::TextureDesc desc;
    desc.format = format;
    desc.width = first.width;
    desc.height = first.height;
    desc.mipLevels = static_cast<std::uint32_t>(texture.mips.size());
    auto gpuResult = device_.createTexture(desc);
    if (!gpuResult.hasValue()) return {};
    const gfx::TextureHandle gpu = gpuResult.value();
    for (std::uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
        const auto& source = texture.mips[mip];
        if (device_.writeTexture(gpu, mip,
                                 std::span(source.bytes.data(), source.bytes.size())) !=
            gfx::GfxError::None) {
            (void)device_.destroyTexture(gpu);
            return {};
        }
    }
    const auto handle = textureArena_.acquire(texture);
    textureMap_.set(handle, TextureEntry{texture, gpu});
    return handle;
}

void Renderer::destroyTexture(resources::TextureHandle handle) {
    if (pbrSettings_.environment == handle) pbrSettings_.environment = {};
    auto* entry = textureMap_.find(handle);
    if (entry != nullptr) {
        if (entry->gpu.isValid()) (void)device_.destroyTexture(entry->gpu);
        textureMap_.erase(handle);
    }
    if (textureArena_.contains(handle)) textureArena_.release(handle);
}

auto Renderer::resolveTexture(resources::TextureHandle texture,
                              gfx::TextureHandle fallback) const -> gfx::TextureHandle {
    const auto* entry = textureMap_.find(texture);
    return entry != nullptr && entry->gpu.isValid() ? entry->gpu : fallback;
}

auto Renderer::createMaterial(const StandardMaterialDesc& desc) -> resources::MaterialHandle {
    return materials_.getOrCreate(desc);
}

auto Renderer::createMaterialFromGltf(
    const resources::GltfMaterial& material,
    std::span<const resources::TextureHandle> textures)
    -> Result<resources::MaterialHandle, MaterialImportError> {
    auto converted = materialFromGltf(material, textures);
    if (!converted.hasValue()) return converted.error();
    return createMaterial(converted.value());
}

auto Renderer::ensureMaterialPipeline(resources::MaterialHandle handle) -> gfx::GfxError {
    return materials_.ensurePipeline(device_, handle);
}

void Renderer::record(gfx::CommandBuffer& commandBuffer, ecs::Entity cameraEntity,
                      float aspect) {
    stats_ = {};
    recordError_ = gfx::GfxError::None;
    if (!inited_) {
        recordError_ = gfx::GfxError::BadDescriptor;
        return;
    }
    updateCameras(registry_, aspect);
    const Camera* camera = registry_.get<Camera>(cameraEntity);
    if (camera == nullptr) {
        recordError_ = gfx::GfxError::InvalidHandle;
        return;
    }
    const FrameLight light = findFrameLight(registry_, *camera, pbrSettings_);
    if (!frameShadowsActive_) frameLightViewProj_ = light.viewProj;
    const math::Mat4 view = cameraView(*camera);
    const math::Frustum frustum = math::Frustum::fromViewProjection(camera->viewProj);
    sceneConstants_.beginFrame();
    sceneInstances_.beginFrame();
    if (framePointLightsReady_) {
        // render() binned the lights on the GPU; its compute pass ran first.
        framePointLightsReady_ = false;
        stats_.pointLights = frameLightStats_.submitted;
        stats_.pointLightsVisible = frameLightStats_.visible;
        stats_.pointLightReferences = frameLightStats_.indexCount;
        stats_.pointLightReferencesDropped = frameLightStats_.droppedIndices;
        stats_.maxPointLightsPerCluster = 0;
        stats_.pointLightBinningDispatches = 3;
    } else {
        pointLightBuffers_.beginFrame();
        framePointLights_ = preparePointLights(*camera, aspect);
    }
    if (!framePointLights_.isValid()) {
        recordError_ = gfx::GfxError::OutOfMemory;
        return;
    }
    queue_.clear();

    for (auto [entity, renderer, world] :
         ecs::View<MeshRenderer, ecs::WorldMatrix>(registry_)) {
        auto* entry = meshMap_.find(renderer.mesh);
        const StandardMaterial* material = materials_.get(renderer.material);
        if (entry == nullptr || material == nullptr) continue;
        ++stats_.candidates;
        const bool hasBounds = entry->mesh.bounds.isValid();
        const math::AABB worldBounds =
            hasBounds ? entry->mesh.bounds.transformed(world.matrix) : math::AABB{};
        if (pbrSettings_.frustumCullingEnabled && hasBounds && worldBounds.isValid() &&
            !frustum.intersects(worldBounds)) {
            ++stats_.frustumCulled;
            continue;
        }
        if (const auto error = materials_.ensurePipeline(
                device_, renderer.material, entry->instancedLayout, recordColorFormat_);
            error != gfx::GfxError::None) {
            recordError_ = error;
            continue;
        }
        const gfx::PipelineHandle pipeline = materials_.pipelineFor(
            renderer.material, entry->instancedLayout, recordColorFormat_);
        if (!pipeline.isValid()) continue;
        RenderItem item;
        item.entity = entity;
        item.meshRenderer = renderer;
        item.material = renderer.material;
        const math::Vec3 depthPoint =
            hasBounds && worldBounds.isValid() ? worldBounds.center()
                                               : world.matrix.col[3].xyz();
        item.depth = -view.transformPoint(depthPoint).z;
        if (!std::isfinite(item.depth)) item.depth = std::numeric_limits<float>::max();
        item.transparent = renderer.transparent || isBlended(material->desc);
        item.pipelineSortKey = pipeline.index;
        queue_.push(item);
    }
    queue_.sort();
    queue_.buildBatches(batches_, pbrSettings_.batchingEnabled);
    stats_.visible = static_cast<std::uint32_t>(queue_.count());

    gfx::TextureHandle environment = defaultEnvironmentTex_;
    std::uint32_t environmentMipCount = defaultEnvironmentMipCount_;
    if (const auto* entry = textureMap_.find(pbrSettings_.environment); entry != nullptr) {
        environment = entry->gpu;
        environmentMipCount = static_cast<std::uint32_t>(entry->texture.mips.size());
    }

    static_assert(sizeof(RenderInstance) == kRenderInstanceStride,
                  "RenderInstance must match the native instanced vertex shaders");
    const auto& items = queue_.items();
    for (const RenderBatch& batch : batches_) {
        const RenderItem& item = items[batch.firstItem];
        auto* entry = meshMap_.find(item.meshRenderer.mesh);
        const StandardMaterial* material = materials_.get(item.material);
        if (entry == nullptr || material == nullptr) continue;
        const gfx::PipelineHandle pipeline =
            materials_.pipelineFor(item.material, entry->instancedLayout, recordColorFormat_);
        if (!pipeline.isValid()) continue;

        instanceStaging_.clear();
        instanceStaging_.reserve(batch.itemCount);
        for (std::uint32_t offset = 0; offset < batch.itemCount; ++offset) {
            const ecs::WorldMatrix* world =
                registry_.get<ecs::WorldMatrix>(items[batch.firstItem + offset].entity);
            if (world == nullptr) continue;
            const float determinant = world->matrix.determinant();
            const math::Mat4 normalMatrix = std::abs(determinant) > 1.0e-8f
                                                ? world->matrix.inverse().transpose()
                                                : math::Mat4::identity();
            RenderInstance instance;
            instance.model = world->matrix;
            instance.normalColumns[0] = normalMatrix.col[0];
            instance.normalColumns[1] = normalMatrix.col[1];
            instance.normalColumns[2] = normalMatrix.col[2];
            instanceStaging_.push(instance);
        }
        if (instanceStaging_.isEmpty()) continue;
        const auto instanceBytes = std::span(
            reinterpret_cast<const std::byte*>(instanceStaging_.data()),
            instanceStaging_.size() * sizeof(RenderInstance));
        const DynamicBufferRing::Slot instanceSlot =
            sceneInstances_.alloc(device_, instanceBytes);
        if (!instanceSlot.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            continue;
        }

        std::uint32_t flags = 0;
        if (material->desc.alphaMode == MaterialAlphaMode::Mask) flags |= 1u;
        if (material->desc.unlit) flags |= 2u;
        if (hasSemantic(entry->mesh, resources::VertexSemantic::Color0)) flags |= 4u;
        if (hasSemantic(entry->mesh, resources::VertexSemantic::Normal)) flags |= 8u;
        SceneUbo constants;
        constants.viewProj = camera->viewProj;
        constants.model = math::Mat4::identity();
        constants.normalMatrix = math::Mat4::identity();
        constants.lightViewProj = frameLightViewProj_;
        constants.baseColorFactor = material->desc.tint;
        constants.emissiveMetallic = math::Vec4(material->desc.emissiveFactor,
                                                 material->desc.metallicFactor);
        constants.cameraExposure = math::Vec4(camera->eye, std::max(pbrSettings_.exposure, 0.0f));
        constants.lightDirectionIntensity = math::Vec4(
            light.direction, light.valid ? light.intensity : 0.0f);
        constants.lightColorRoughness = math::Vec4(light.color,
                                                   material->desc.roughnessFactor);
        constants.materialParams = {material->desc.normalScale,
                                    material->desc.normalYSign < 0.0f ? -1.0f : 1.0f,
                                    std::clamp(material->desc.occlusionStrength, 0.0f, 1.0f),
                                    std::clamp(material->desc.alphaCutoff, 0.0f, 1.0f)};
        constants.renderParams = {
            1.0f / static_cast<float>(shadowResolution_),
            std::max(pbrSettings_.shadowBias, 0.0f), frameShadowsActive_ ? 1.0f : 0.0f,
            static_cast<float>(environmentMipCount > 0 ? environmentMipCount - 1 : 0)};
        constants.ambientFlags = {std::max(pbrSettings_.ambientIntensity, 0.0f),
                                  static_cast<float>(flags), frameBloomActive_ ? 1.0f : 0.0f,
                                  0.0f};
        const auto slot = sceneConstants_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&constants),
                               sizeof(constants)));
        if (!slot.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            continue;
        }

        commandBuffer.bindPipeline(pipeline);
        sceneConstants_.bind(commandBuffer, slot);
        commandBuffer.bindResource(pointLightStorageSlot(device_.backend()),
                                   gfx::BindingType::StorageBuffer,
                                   framePointLights_.index, framePointLights_.generation);
        const gfx::TextureHandle textures[7] = {
            resolveTexture(material->desc.albedo, whiteTex_),
            resolveTexture(material->desc.normal, neutralNormalTex_),
            resolveTexture(material->desc.metallicRoughness, whiteTex_),
            resolveTexture(material->desc.occlusion, whiteTex_),
            resolveTexture(material->desc.emissive, blackTex_),
            environment,
            shadowColorTex_,
        };
        const gfx::SamplerHandle samplers[7] = {
            surfaceSampler_, surfaceSampler_, surfaceSampler_, surfaceSampler_,
            surfaceSampler_, environmentSampler_, shadowSampler_,
        };
        for (std::uint32_t resourceSlot = 0; resourceSlot < 7; ++resourceSlot) {
            commandBuffer.bindResource(resourceSlot, gfx::BindingType::Texture,
                                       textures[resourceSlot].index,
                                       textures[resourceSlot].generation);
            commandBuffer.bindResource(resourceSlot, gfx::BindingType::Sampler,
                                       samplers[resourceSlot].index,
                                       samplers[resourceSlot].generation);
        }
        const PointShadowBinding atlasBinding = pointShadowAtlasBinding(device_.backend());
        const gfx::TextureHandle atlas =
            framePointShadowsActive_ ? pointShadowColorTex_ : whiteTex_;
        commandBuffer.bindResource(atlasBinding.bindPoint, atlasBinding.slot,
                                   gfx::BindingType::Texture, atlas.index, atlas.generation);
        commandBuffer.bindResource(atlasBinding.bindPoint, atlasBinding.slot,
                                   gfx::BindingType::Sampler, shadowSampler_.index,
                                   shadowSampler_.generation);
        bindMesh(commandBuffer, entry->gpu);
        commandBuffer.bindVertexBuffer(entry->instanceBinding, instanceSlot.handle,
                                       kRenderInstanceStride);
        drawMesh(commandBuffer, entry->mesh, entry->gpu,
                 item.meshRenderer.subMeshIndex,
                 static_cast<std::uint32_t>(instanceStaging_.size()));
        ++stats_.draws;
        stats_.instances += static_cast<std::uint32_t>(instanceStaging_.size());
    }
}

void Renderer::gatherPointLights() {
    pointLightInputs_.clear();
    pointLightCastsShadow_.clear();
    pointLightKeys_.clear();
    if (!pbrSettings_.pointLightsEnabled) return;
    for (auto [entity, light] : ecs::View<PointLight>(registry_)) {
        ClusteredLightInput input;
        input.position = light.position;
        if (const ecs::WorldMatrix* world = registry_.get<ecs::WorldMatrix>(entity)) {
            input.position = world->matrix.transformPoint(light.position);
        }
        input.radiance = math::maxComponentwise(light.color, math::Vec3(0.0f)) *
                         std::max(light.intensity, 0.0f);
        input.range = light.range;
        pointLightInputs_.push(input);
        pointLightCastsShadow_.push(light.castShadows ? 1u : 0u);
        pointLightKeys_.push((static_cast<std::uint64_t>(entity.index) << 32) |
                             entity.generation);
    }
}

auto Renderer::preparePointShadows(const Camera& camera) -> gfx::GfxError {
    framePointShadowsActive_ = false;
    framePointShadowsDirty_ = false;
    framePointShadowsFullClear_ = true;
    frameShadowedLights_.clear();
    frameShadowFaces_.clear();
    pointLightsGathered_ = false;
    const auto invalidateCache = [this] {
        std::fill(pointShadowSlotValid_.begin(), pointShadowSlotValid_.end(), std::uint8_t{0});
    };
    const std::uint32_t budget = std::min(pbrSettings_.pointShadowBudget, kPointShadowMaxBudget);
    if (!pbrSettings_.pointLightsEnabled || !pbrSettings_.pointShadowsEnabled || budget == 0) {
        invalidateCache();
        return gfx::GfxError::None;
    }
    gatherPointLights();
    pointLightsGathered_ = true;

    bool anyCaster = false;
    for (auto [entity, renderer] : ecs::View<MeshRenderer>(registry_)) {
        (void)entity;
        if (renderer.castShadows) {
            anyCaster = true;
            break;
        }
    }
    if (!anyCaster) {
        invalidateCache();
        return gfx::GfxError::None;
    }

    const math::Frustum frustum = math::Frustum::fromViewProjection(camera.viewProj);
    shadowCandidates_.clear();
    shadowCandidateSources_.clear();
    for (std::size_t i = 0; i < pointLightInputs_.size(); ++i) {
        if (pointLightCastsShadow_[i] == 0) continue;
        const ClusteredLightInput& light = pointLightInputs_[i];
        if (!(light.range > 0.0f)) continue;
        const math::Vec3 reach(light.range);
        if (!frustum.intersects(math::AABB{light.position - reach, light.position + reach})) {
            continue;
        }
        shadowCandidates_.push({light.position, light.radiance, light.range});
        shadowCandidateSources_.push(static_cast<std::uint32_t>(i));
    }
    selectShadowedPointLights(
        std::span<const PointShadowCandidate>(shadowCandidates_.data(),
                                              shadowCandidates_.size()),
        camera.eye, budget, shadowSelection_);
    if (shadowSelection_.isEmpty()) {
        invalidateCache();
        return gfx::GfxError::None;
    }
    if (const auto error = ensurePointShadowAtlas(budget); error != gfx::GfxError::None) {
        return error;
    }
    if (const auto error = ensurePointShadowResetMesh(); error != gfx::GfxError::None) {
        return error;
    }

    // Stable slots: a light keeps its atlas row while it stays selected.
    shadowSlotKeys_.clear();
    for (const std::uint32_t candidate : shadowSelection_) {
        shadowSlotKeys_.push(pointLightKeys_[shadowCandidateSources_[candidate]]);
    }
    const containers::Array<std::uint64_t> previousOwners = pointShadowSlotOwners_;
    assignStablePointShadowSlots(
        std::span<const std::uint64_t>(shadowSlotKeys_.data(), shadowSlotKeys_.size()),
        pointShadowSlotOwners_, shadowSlots_);
    for (std::size_t slot = 0; slot < pointShadowSlotOwners_.size(); ++slot) {
        if (pointShadowSlotOwners_[slot] != previousOwners[slot] ||
            pointShadowSlotOwners_[slot] == kPointShadowFreeSlot) {
            pointShadowSlotValid_[slot] = 0;
        }
    }
    std::uint32_t slotCount = 0;
    for (const std::uint32_t slot : shadowSlots_) {
        if (slot < budget) slotCount = std::max(slotCount, slot + 1u);
    }
    frameShadowedLights_.resize(slotCount);
    std::fill(frameShadowedLights_.begin(), frameShadowedLights_.end(), ShadowedPointLight{});
    frameShadowFaces_.resize(static_cast<std::size_t>(slotCount) * kPointShadowFaces);
    std::fill(frameShadowFaces_.begin(), frameShadowFaces_.end(), math::Mat4::identity());
    for (std::size_t i = 0; i < shadowSelection_.size(); ++i) {
        const std::uint32_t slot = shadowSlots_[i];
        if (slot >= slotCount) continue;
        ClusteredLightInput& light =
            pointLightInputs_[shadowCandidateSources_[shadowSelection_[i]]];
        light.shadowSlot = slot + 1u;
        ShadowedPointLight& shadowed = frameShadowedLights_[slot];
        shadowed.position = light.position;
        shadowed.range = light.range;
        shadowed.active = true;
        for (std::uint32_t face = 0; face < kPointShadowFaces; ++face) {
            frameShadowFaces_[slot * kPointShadowFaces + face] =
                pointShadowFaceViewProj(light.position, light.range, face);
        }
    }

    // Dirty tracking: a slot is redrawn unless its signature (light + every
    // caster inside its sphere) matches the one its tiles were drawn with.
    gatherPointShadowCasters();
    for (std::size_t slot = 0; slot < frameShadowedLights_.size(); ++slot) {
        ShadowedPointLight& shadowed = frameShadowedLights_[slot];
        if (!shadowed.active) continue;
        shadowed.signature = pointShadowSignature(shadowed.position, shadowed.range);
        shadowed.dirty = !pbrSettings_.pointShadowCaching || pointShadowSlotValid_[slot] == 0 ||
                         pointShadowSlotSignatures_[slot] != shadowed.signature;
        framePointShadowsDirty_ = framePointShadowsDirty_ || shadowed.dirty;
        framePointShadowsFullClear_ = framePointShadowsFullClear_ && shadowed.dirty;
    }
    framePointShadowsActive_ = true;
    return gfx::GfxError::None;
}

// Point-shadow header words and face matrices; the matrices go at
// `matrixOffset` (the CPU path appends them, the GPU path reserves room after
// the light records).
void Renderer::writePointShadowWords(std::uint32_t matrixOffset) {
    if (!framePointShadowsActive_ || frameShadowedLights_.isEmpty()) return;
    const std::size_t matrixWords = frameShadowFaces_.size() * 16u;
    if (pointLightWords_.size() < matrixOffset + matrixWords) {
        pointLightWords_.resize(matrixOffset + matrixWords);
    }
    pointLightWords_[10] = matrixOffset;
    pointLightWords_[11] = static_cast<std::uint32_t>(frameShadowedLights_.size());
    pointLightWords_[12] = pointShadowRows_;
    pointLightWords_[13] = floatWord(std::max(pbrSettings_.pointShadowBias, 0.0f));
    pointLightWords_[14] = floatWord(2.0f / static_cast<float>(pointShadowTileSize_));
    std::uint32_t* out = pointLightWords_.data() + matrixOffset;
    for (const math::Mat4& face : frameShadowFaces_) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            const math::Vec4& c = face.col[column];
            *out++ = floatWord(c.x);
            *out++ = floatWord(c.y);
            *out++ = floatWord(c.z);
            *out++ = floatWord(c.w);
        }
    }
}

namespace {

struct LightBinningUbo {
    std::uint32_t counts[4];  // pass, clusterCount, visible, copyWords
    std::uint32_t offsets[4]; // rangeOffset, maxIndexCount, gridOffset, indexOffset
    std::uint32_t grid[4];    // tilesX, tilesY, slices, 0
};

constexpr std::uint32_t kLightBinningGroupSize = 64;

} // namespace

auto Renderer::ensureLightBinning() -> bool {
    if (lightBinningPipeline_.isValid()) return true;
    if (lightBinningUnavailable_ || device_.backend() == gfx::Backend::Null) return false;
    const auto unavailable = [this] {
        if (lightBinningShader_.isValid()) (void)device_.destroyShader(lightBinningShader_);
        lightBinningShader_ = {};
        lightBinningConstants_.destroy(device_);
        pointLightGpuBuffers_.destroy(device_);
        lightBinningUnavailable_ = true;
        return false;
    };
    auto shader = loadEffectShader(device_, EffectShader::LightBinningCompute);
    if (!shader.hasValue()) return unavailable();
    lightBinningShader_ = shader.value();
    gfx::PipelineStateDesc desc;
    desc.computeShader = lightBinningShader_;
    gfx::BindGroup group;
    group.bindPoint = 0;
    const std::uint32_t compute = gfx::shaderStageBit(gfx::ShaderStage::Compute);
    group.slots.push({0, gfx::BindingType::UniformBuffer, compute, 1});
    group.slots.push({0, gfx::BindingType::StorageBuffer, compute, 1});
    group.slots.push({1, gfx::BindingType::StorageBuffer, compute, 1});
    desc.layout.groups.push(static_cast<gfx::BindGroup&&>(group));
    auto pipeline = device_.createPipeline(desc);
    if (!pipeline.hasValue()) return unavailable();
    if (lightBinningConstants_.create(device_, sizeof(LightBinningUbo), 3) !=
            gfx::GfxError::None ||
        pointLightGpuBuffers_.create(
            device_, gfx::BufferUsage::Storage | gfx::BufferUsage::TransferSrc) !=
            gfx::GfxError::None) {
        (void)device_.destroyPipeline(pipeline.value());
        return unavailable();
    }
    lightBinningPipeline_ = pipeline.value();
    return true;
}

auto Renderer::preparePointLightsGpu(const Camera& camera, float aspect) -> bool {
    framePointLightsReady_ = false;
    if (!pbrSettings_.gpuLightBinning || !pbrSettings_.pointLightsEnabled) return false;
    const ClusterGridConfig& grid = pbrSettings_.pointLightClusters;
    if (grid.tilesX > 0xffffu || grid.tilesY > 0xffffu || grid.slices > 0xffffu) return false;
    if (!ensureLightBinning()) return false;
    if (!pointLightsGathered_) {
        gatherPointLights();
        pointLightsGathered_ = true;
    }
    const std::uint32_t shadowWords =
        framePointShadowsActive_ && !frameShadowedLights_.isEmpty()
            ? static_cast<std::uint32_t>(frameShadowFaces_.size() * 16u)
            : 0u;
    const float safeAspect = aspect > 0.0f ? aspect : 1.0f;
    frameLightStats_ = buildClusteredLightBinningInput(
        std::span<const ClusteredLightInput>(pointLightInputs_.data(),
                                             pointLightInputs_.size()),
        cameraView(camera), cameraProj(camera, safeAspect), camera.nearPlane, camera.farPlane,
        grid, shadowWords, pointLightWords_, frameBinning_);
    // Nothing to bin: the CPU path writes the header-only buffer.
    if (frameBinning_.visible == 0) return false;
    pointLightsGathered_ = false;
    if (shadowWords != 0) writePointShadowWords(frameBinning_.reservedOffset);

    pointLightBuffers_.beginFrame();
    pointLightGpuBuffers_.beginFrame();
    const DynamicBufferRing::Slot input = pointLightBuffers_.alloc(
        device_, std::span(reinterpret_cast<const std::byte*>(pointLightWords_.data()),
                           pointLightWords_.size() * sizeof(std::uint32_t)));
    const DynamicBufferRing::Slot output = pointLightGpuBuffers_.allocUninitialized(
        device_, frameBinning_.totalWords * static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    if (!input.valid || !output.valid) return false;
    framePointLightInput_ = input.handle;
    framePointLights_ = output.handle;
    framePointLightsReady_ = true;
    return true;
}

void Renderer::recordLightBinning(gfx::CommandBuffer& commandBuffer) {
    const ClusteredLightGpuParams& p = frameBinning_;
    lightBinningConstants_.beginFrame();
    const auto groups = [](std::uint32_t threads) {
        return (threads + kLightBinningGroupSize - 1u) / kLightBinningGroupSize;
    };
    const gfx::ResourceState written{gfx::PipelineStageMask::ComputeShader,
                                     gfx::ResourceAccessMask::ShaderWrite};
    for (std::uint32_t pass = 0; pass < 3; ++pass) {
        LightBinningUbo constants{{pass, p.clusterCount, p.visible, p.copyWords},
                                  {p.rangeOffset, p.maxIndexCount, p.gridOffset, p.indexOffset},
                                  {p.tilesX, p.tilesY, p.slices, 0u}};
        const auto slot = lightBinningConstants_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&constants), sizeof(constants)));
        if (!slot.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            return;
        }
        commandBuffer.bindPipeline(lightBinningPipeline_);
        lightBinningConstants_.bind(commandBuffer, slot);
        commandBuffer.bindResource(0, gfx::BindingType::StorageBuffer, framePointLightInput_.index,
                                   framePointLightInput_.generation);
        commandBuffer.bindResource(1, gfx::BindingType::StorageBuffer, framePointLights_.index,
                                   framePointLights_.generation);
        const std::uint32_t groupCount =
            pass == 0 ? groups(std::max(p.clusterCount, p.copyWords))
                      : (pass == 1 ? 1u : groups(p.clusterCount));
        commandBuffer.dispatch(groupCount);
        if (pass < 2) {
            commandBuffer.bufferBarrier(
                framePointLights_, written,
                {gfx::PipelineStageMask::ComputeShader,
                 gfx::ResourceAccessMask::ShaderRead | gfx::ResourceAccessMask::ShaderWrite});
        }
    }
    commandBuffer.bufferBarrier(
        framePointLights_, written,
        {gfx::PipelineStageMask::FragmentShader, gfx::ResourceAccessMask::ShaderRead});
}

auto Renderer::preparePointLights(const Camera& camera, float aspect) -> gfx::BufferHandle {
    if (!pointLightsGathered_) gatherPointLights();
    pointLightsGathered_ = false;
    const float safeAspect = aspect > 0.0f ? aspect : 1.0f;
    const ClusteredLightStats lightStats = buildClusteredLightBuffer(
        std::span<const ClusteredLightInput>(pointLightInputs_.data(),
                                             pointLightInputs_.size()),
        cameraView(camera), cameraProj(camera, safeAspect), camera.nearPlane,
        camera.farPlane, pbrSettings_.pointLightClusters, pointLightWords_);
    stats_.pointLights = lightStats.submitted;
    stats_.pointLightsVisible = lightStats.visible;
    stats_.pointLightReferences = lightStats.indexCount;
    stats_.pointLightReferencesDropped = lightStats.droppedIndices;
    stats_.maxPointLightsPerCluster = lightStats.maxLightsPerCluster;
    writePointShadowWords(static_cast<std::uint32_t>(pointLightWords_.size()));
    const DynamicBufferRing::Slot slot = pointLightBuffers_.alloc(
        device_, std::span(reinterpret_cast<const std::byte*>(pointLightWords_.data()),
                           pointLightWords_.size() * sizeof(std::uint32_t)));
    return slot.valid ? slot.handle : gfx::BufferHandle{};
}

void Renderer::recordShadow(gfx::CommandBuffer& commandBuffer) {
    shadowConstants_.beginFrame();
    for (auto [entity, renderer, world] :
         ecs::View<MeshRenderer, ecs::WorldMatrix>(registry_)) {
        if (!renderer.castShadows) continue;
        auto* entry = meshMap_.find(renderer.mesh);
        const StandardMaterial* material = materials_.get(renderer.material);
        if (entry == nullptr || material == nullptr || isBlended(material->desc)) continue;
        if (const auto error = materials_.ensureShadowPipeline(device_, entry->gpu.layout);
            error != gfx::GfxError::None) {
            recordError_ = error;
            continue;
        }
        const gfx::PipelineHandle pipeline =
            materials_.shadowPipelineFor(entry->gpu.layout);
        if (!pipeline.isValid()) continue;
        ShadowUbo constants;
        constants.lightViewProj = frameLightViewProj_;
        constants.model = world.matrix;
        constants.alphaParams = {
            material->desc.tint.w, material->desc.alphaCutoff,
            material->desc.alphaMode == MaterialAlphaMode::Mask ? 1.0f : 0.0f,
            hasSemantic(entry->mesh, resources::VertexSemantic::TexCoord0) ? 1.0f : 0.0f};
        const auto slot = shadowConstants_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&constants),
                               sizeof(constants)));
        if (!slot.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            continue;
        }
        commandBuffer.bindPipeline(pipeline);
        shadowConstants_.bind(commandBuffer, slot);
        const gfx::TextureHandle albedo =
            resolveTexture(material->desc.albedo, whiteTex_);
        commandBuffer.bindResource(0, gfx::BindingType::Texture, albedo.index,
                                   albedo.generation);
        commandBuffer.bindResource(0, gfx::BindingType::Sampler,
                                   surfaceSampler_.index, surfaceSampler_.generation);
        bindMesh(commandBuffer, entry->gpu);
        drawMesh(commandBuffer, entry->mesh, entry->gpu, renderer.subMeshIndex);
    }
}

void Renderer::gatherPointShadowCasters() {
    // Casters grouped by (mesh, material, submesh) so each group is one
    // instanced draw per light face, like the main pass.
    pointShadowCasters_.clear();
    for (auto [entity, renderer, world] :
         ecs::View<MeshRenderer, ecs::WorldMatrix>(registry_)) {
        (void)entity;
        if (!renderer.castShadows) continue;
        const auto* entry = meshMap_.find(renderer.mesh);
        const StandardMaterial* material = materials_.get(renderer.material);
        if (entry == nullptr || material == nullptr || isBlended(material->desc)) continue;
        PointShadowCaster caster;
        caster.mesh = renderer.mesh;
        caster.material = renderer.material;
        caster.subMesh = renderer.subMeshIndex;
        caster.model = world.matrix;
        if (entry->mesh.bounds.isValid()) {
            caster.bounds = entry->mesh.bounds.transformed(world.matrix);
            caster.cullable = caster.bounds.isValid();
        }
        pointShadowCasters_.push(caster);
    }
    std::stable_sort(pointShadowCasters_.begin(), pointShadowCasters_.end(),
                     [](const PointShadowCaster& a, const PointShadowCaster& b) {
                         return pointShadowGroupKey(a.mesh, a.material, a.subMesh) <
                                pointShadowGroupKey(b.mesh, b.material, b.subMesh);
                     });
}

auto Renderer::pointShadowSignature(const math::Vec3& position, float range) const
    -> std::uint64_t {
    SignatureHash hash;
    hash.add(position);
    hash.add(range);
    hash.add(pointShadowTileSize_);
    hash.add(pointShadowRows_);
    for (const PointShadowCaster& caster : pointShadowCasters_) {
        if (caster.cullable && !sphereTouchesBox(position, range, caster.bounds)) continue;
        hash.add(caster.mesh.index);
        hash.add(caster.mesh.generation);
        hash.add(caster.material.index);
        hash.add(caster.material.generation);
        hash.add(caster.subMesh);
        hash.add(caster.cullable ? 1u : 0u);
        for (const math::Vec4& column : caster.model.col) hash.add(column);
        if (const StandardMaterial* material = materials_.get(caster.material)) {
            // Everything the shadow shader reads from the material.
            hash.add(material->desc.tint.w);
            hash.add(material->desc.alphaCutoff);
            hash.add(static_cast<std::uint32_t>(material->desc.alphaMode));
            const gfx::TextureHandle albedo = resolveTexture(material->desc.albedo, whiteTex_);
            hash.add(albedo.index);
            hash.add(albedo.generation);
        }
    }
    return hash.value;
}

auto Renderer::ensurePointShadowResetMesh() -> gfx::GfxError {
    if (pointShadowResetMesh_.isValid() && meshMap_.find(pointShadowResetMesh_) != nullptr) {
        return gfx::GfxError::None;
    }
    // One triangle covering clip space at z = 0.5 with the identity face
    // matrix; with the light at the origin and a huge 1/range every fragment
    // stores clamp(|p| * 1e4) = 1.0, the same value the atlas clear writes.
    struct ResetVertex {
        float position[3];
        float uv[2];
    };
    static constexpr ResetVertex kVertices[3] = {
        {{-1.0f, -1.0f, 0.5f}, {0.0f, 0.0f}},
        {{3.0f, -1.0f, 0.5f}, {2.0f, 0.0f}},
        {{-1.0f, 3.0f, 0.5f}, {0.0f, 2.0f}},
    };
    static constexpr std::uint16_t kIndices[3] = {0, 1, 2};
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(ResetVertex);
    stream.data.resize(sizeof(kVertices));
    std::memcpy(stream.data.data(), kVertices, sizeof(kVertices));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position, resources::VertexFormat::F32_3, 0,
                          0});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0, resources::VertexFormat::F32_2,
                          0, 12});
    mesh.indexFormat = resources::IndexFormat::U16;
    mesh.indices.resize(sizeof(kIndices));
    std::memcpy(mesh.indices.data(), kIndices, sizeof(kIndices));
    mesh.submeshes.push({0, 3});
    pointShadowResetMesh_ = createMesh(mesh);
    return pointShadowResetMesh_.isValid() ? gfx::GfxError::None : gfx::GfxError::OutOfMemory;
}

void Renderer::recordPointShadows(gfx::CommandBuffer& commandBuffer) {
    pointShadowConstants_.beginFrame();
    pointShadowInstanceBuffers_.beginFrame();
    pointShadowDraws_ = 0;
    pointShadowRedrawn_ = 0;
    const auto rows = pointShadowRows_;
    const std::uint32_t tile = pointShadowTileSize_;
    // Stored rows of a slot's tiles: the PBR shader reads slot `s` at atlas
    // NDC rows (see pointShadowTileTransform), which is stored row block `s`
    // on OpenGL and `rows - 1 - s` elsewhere.
    const auto selectTile = [&](std::size_t slot, std::uint32_t face) {
        const std::uint32_t rowBlock = device_.backend() == gfx::Backend::OpenGL
                                           ? static_cast<std::uint32_t>(slot)
                                           : rows - 1u - static_cast<std::uint32_t>(slot);
        commandBuffer.setViewport(gfx::Extent{tile, tile}, face * tile, rowBlock * tile);
        commandBuffer.setScissor(gfx::Extent{tile, tile}, face * tile, rowBlock * tile);
    };

    // Tile reset for partial redraws: the pass keeps the cached colour, so a
    // dirty slot first rewrites its six tiles to 1.0 (depth is cleared for
    // the whole atlas every pass, exactly as in the full-clear path).
    gfx::PipelineHandle resetPipeline{};
    const MeshEntry* resetEntry = nullptr;
    DynamicBufferRing::Slot resetInstance{};
    ConstantRing::Slot resetConstants{};
    if (!framePointShadowsFullClear_) {
        resetEntry = meshMap_.find(pointShadowResetMesh_);
        if (resetEntry == nullptr) {
            recordError_ = gfx::GfxError::InvalidHandle;
            return;
        }
        if (const auto error = materials_.ensurePointShadowPipeline(
                device_, resetEntry->instancedLayout, true);
            error != gfx::GfxError::None) {
            recordError_ = error;
            return;
        }
        resetPipeline = materials_.pointShadowPipelineFor(resetEntry->instancedLayout, true);
        RenderInstance identity{};
        identity.model = math::Mat4::identity();
        resetInstance = pointShadowInstanceBuffers_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&identity), sizeof(identity)));
        PointShadowUbo constants;
        constants.faceViewProj = math::Mat4::identity();
        constants.reserved = math::Mat4::identity();
        constants.lightPositionInvRange = math::Vec4(0.0f, 0.0f, 0.0f, 1.0e4f);
        constants.tileTransform = math::Vec4(1.0f, 1.0f, 0.0f, 0.0f);
        constants.alphaParams = math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        resetConstants = pointShadowConstants_.alloc(
            device_,
            std::span(reinterpret_cast<const std::byte*>(&constants), sizeof(constants)));
        if (!resetPipeline.isValid() || !resetInstance.valid || !resetConstants.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            return;
        }
    }

    std::array<math::Frustum, kPointShadowFaces> faceFrusta{};
    std::array<bool, kPointShadowFaces> faceUsed{};
    for (std::size_t slot = 0; slot < frameShadowedLights_.size(); ++slot) {
        const ShadowedPointLight& light = frameShadowedLights_[slot];
        if (!light.active || !light.dirty) continue;
        ++pointShadowRedrawn_;
        if (resetEntry != nullptr) {
            for (std::uint32_t face = 0; face < kPointShadowFaces; ++face) {
                selectTile(slot, face);
                commandBuffer.bindPipeline(resetPipeline);
                pointShadowConstants_.bind(commandBuffer, resetConstants);
                commandBuffer.bindResource(0, gfx::BindingType::Texture, whiteTex_.index,
                                           whiteTex_.generation);
                commandBuffer.bindResource(0, gfx::BindingType::Sampler,
                                           surfaceSampler_.index, surfaceSampler_.generation);
                bindMesh(commandBuffer, resetEntry->gpu);
                commandBuffer.bindVertexBuffer(resetEntry->instanceBinding,
                                               resetInstance.handle, kRenderInstanceStride);
                drawMesh(commandBuffer, resetEntry->mesh, resetEntry->gpu, 0, 1);
            }
        }
        for (std::uint32_t face = 0; face < kPointShadowFaces; ++face) {
            faceFrusta[face] = math::Frustum::fromViewProjection(
                frameShadowFaces_[slot * kPointShadowFaces + face]);
        }
        std::size_t first = 0;
        while (first < pointShadowCasters_.size()) {
            const PointShadowCaster& head = pointShadowCasters_[first];
            const auto headKey = pointShadowGroupKey(head.mesh, head.material, head.subMesh);
            std::size_t last = first + 1;
            while (last < pointShadowCasters_.size() &&
                   pointShadowGroupKey(pointShadowCasters_[last].mesh,
                                       pointShadowCasters_[last].material,
                                       pointShadowCasters_[last].subMesh) == headKey) {
                ++last;
            }
            const std::size_t groupBegin = first;
            first = last;

            // Casters of this group inside the light's sphere, and which of
            // the six faces see at least one of them.
            pointShadowInstanceStaging_.clear();
            faceUsed.fill(false);
            for (std::size_t index = groupBegin; index < last; ++index) {
                const PointShadowCaster& caster = pointShadowCasters_[index];
                if (caster.cullable &&
                    !sphereTouchesBox(light.position, light.range, caster.bounds)) {
                    continue;
                }
                for (std::uint32_t face = 0; face < kPointShadowFaces; ++face) {
                    faceUsed[face] = faceUsed[face] || !caster.cullable ||
                                     faceFrusta[face].intersects(caster.bounds);
                }
                RenderInstance instance{};
                instance.model = caster.model;
                pointShadowInstanceStaging_.push(instance);
            }
            if (pointShadowInstanceStaging_.isEmpty()) continue;

            auto* entry = meshMap_.find(head.mesh);
            const StandardMaterial* material = materials_.get(head.material);
            if (entry == nullptr || material == nullptr) continue;
            if (const auto error =
                    materials_.ensurePointShadowPipeline(device_, entry->instancedLayout);
                error != gfx::GfxError::None) {
                recordError_ = error;
                continue;
            }
            const gfx::PipelineHandle pipeline =
                materials_.pointShadowPipelineFor(entry->instancedLayout);
            if (!pipeline.isValid()) continue;
            const DynamicBufferRing::Slot instances = pointShadowInstanceBuffers_.alloc(
                device_, std::span(reinterpret_cast<const std::byte*>(
                                       pointShadowInstanceStaging_.data()),
                                   pointShadowInstanceStaging_.size() * sizeof(RenderInstance)));
            if (!instances.valid) {
                recordError_ = gfx::GfxError::OutOfMemory;
                continue;
            }
            const gfx::TextureHandle albedo = resolveTexture(material->desc.albedo, whiteTex_);
            for (std::uint32_t face = 0; face < kPointShadowFaces; ++face) {
                if (!faceUsed[face]) continue;
                PointShadowUbo constants;
                constants.faceViewProj = frameShadowFaces_[slot * kPointShadowFaces + face];
                constants.reserved = math::Mat4::identity();
                constants.lightPositionInvRange =
                    math::Vec4(light.position, 1.0f / std::max(light.range, 1.0e-4f));
                // The tile is selected with the viewport + scissor, so the
                // hardware clips each face to its own frustum; the shader's
                // tile transform stays the identity.
                constants.tileTransform = math::Vec4(1.0f, 1.0f, 0.0f, 0.0f);
                constants.alphaParams = {
                    material->desc.tint.w, material->desc.alphaCutoff,
                    material->desc.alphaMode == MaterialAlphaMode::Mask ? 1.0f : 0.0f,
                    hasSemantic(entry->mesh, resources::VertexSemantic::TexCoord0) ? 1.0f
                                                                                   : 0.0f};
                const auto constantSlot = pointShadowConstants_.alloc(
                    device_, std::span(reinterpret_cast<const std::byte*>(&constants),
                                       sizeof(constants)));
                if (!constantSlot.valid) {
                    recordError_ = gfx::GfxError::OutOfMemory;
                    continue;
                }
                selectTile(slot, face);
                commandBuffer.bindPipeline(pipeline);
                pointShadowConstants_.bind(commandBuffer, constantSlot);
                commandBuffer.bindResource(0, gfx::BindingType::Texture, albedo.index,
                                           albedo.generation);
                commandBuffer.bindResource(0, gfx::BindingType::Sampler,
                                           surfaceSampler_.index, surfaceSampler_.generation);
                bindMesh(commandBuffer, entry->gpu);
                commandBuffer.bindVertexBuffer(entry->instanceBinding, instances.handle,
                                               kRenderInstanceStride);
                drawMesh(commandBuffer, entry->mesh, entry->gpu, head.subMesh,
                         static_cast<std::uint32_t>(pointShadowInstanceStaging_.size()));
                ++pointShadowDraws_;
            }
        }
    }
}

void Renderer::releasePointShadowAtlas() {
    if (pointShadowFramebuffer_.isValid() || pointShadowLoadFramebuffer_.isValid() ||
        pointShadowColorTex_.isValid() || pointShadowDepthTex_.isValid()) {
        device_.waitIdle();
    }
    if (pointShadowFramebuffer_.isValid()) (void)device_.destroyFramebuffer(pointShadowFramebuffer_);
    if (pointShadowLoadFramebuffer_.isValid()) {
        (void)device_.destroyFramebuffer(pointShadowLoadFramebuffer_);
    }
    pointShadowLoadFramebuffer_ = {};
    pointShadowSlotOwners_.clear();
    pointShadowSlotSignatures_.clear();
    pointShadowSlotValid_.clear();
    if (pointShadowColorTex_.isValid()) (void)device_.destroyTexture(pointShadowColorTex_);
    if (pointShadowDepthTex_.isValid()) (void)device_.destroyTexture(pointShadowDepthTex_);
    pointShadowFramebuffer_ = {};
    pointShadowColorTex_ = {};
    pointShadowDepthTex_ = {};
    pointShadowTileSize_ = 0;
    pointShadowRows_ = 0;
}

auto Renderer::ensurePointShadowAtlas(std::uint32_t rows) -> gfx::GfxError {
    // Six face columns by `rows` lights. 8192 keeps the atlas inside WebGPU's
    // default texture limit.
    constexpr std::uint32_t kMaxAtlasSide = 8192;
    std::uint32_t tile = std::clamp(pbrSettings_.pointShadowTileSize, 16u, 1024u);
    tile = std::min(tile, kMaxAtlasSide / std::max(rows, kPointShadowFaces));
    if (pointShadowColorTex_.isValid() && tile == pointShadowTileSize_ &&
        rows == pointShadowRows_) {
        return gfx::GfxError::None;
    }
    releasePointShadowAtlas();
    const gfx::Extent extent{tile * kPointShadowFaces, tile * rows};
    const float far[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    auto color = createRenderTexture(device_, gfx::TextureFormat::R16_FLOAT, extent, far, 1.0f);
    if (!color.hasValue()) return color.error();
    pointShadowColorTex_ = color.value();
    auto depth = createRenderTexture(device_, gfx::TextureFormat::Depth32F, extent, far, 1.0f);
    if (!depth.hasValue()) {
        releasePointShadowAtlas();
        return depth.error();
    }
    pointShadowDepthTex_ = depth.value();
    gfx::FramebufferDesc desc;
    desc.extent = extent;
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.type = gfx::AttachmentType::Color;
    colorAttachment.texture = pointShadowColorTex_;
    colorAttachment.clearOnLoad = true;
    for (std::size_t i = 0; i < 4; ++i) colorAttachment.clear.color[i] = 1.0f;
    desc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = pointShadowDepthTex_;
    depthAttachment.clearOnLoad = true;
    depthAttachment.clear.depth = 1.0f;
    desc.attachments.push(depthAttachment);
    auto framebuffer = device_.createFramebuffer(desc);
    if (!framebuffer.hasValue()) {
        releasePointShadowAtlas();
        return framebuffer.error();
    }
    pointShadowFramebuffer_ = framebuffer.value();
    // Same attachments, but the colour is loaded so cached tiles survive.
    desc.attachments[0].clearOnLoad = false;
    auto loadFramebuffer = device_.createFramebuffer(desc);
    if (!loadFramebuffer.hasValue()) {
        releasePointShadowAtlas();
        return loadFramebuffer.error();
    }
    pointShadowLoadFramebuffer_ = loadFramebuffer.value();
    pointShadowTileSize_ = tile;
    pointShadowRows_ = rows;
    pointShadowSlotOwners_.resize(rows);
    std::fill(pointShadowSlotOwners_.begin(), pointShadowSlotOwners_.end(),
              kPointShadowFreeSlot);
    pointShadowSlotSignatures_.resize(rows);
    std::fill(pointShadowSlotSignatures_.begin(), pointShadowSlotSignatures_.end(), 0ull);
    pointShadowSlotValid_.resize(rows);
    std::fill(pointShadowSlotValid_.begin(), pointShadowSlotValid_.end(), std::uint8_t{0});
    return gfx::GfxError::None;
}

void Renderer::releaseBloomTargets() {
    const bool any = hdrColorTex_.isValid() || !bloomDown_.isEmpty();
    if (any) device_.waitIdle();
    const auto releaseTarget = [this](BloomTarget& target) {
        if (target.framebuffer.isValid()) (void)device_.destroyFramebuffer(target.framebuffer);
        if (target.texture.isValid()) (void)device_.destroyTexture(target.texture);
        target = {};
    };
    for (BloomTarget& target : bloomDown_) releaseTarget(target);
    for (BloomTarget& target : bloomUp_) releaseTarget(target);
    bloomDown_.clear();
    bloomUp_.clear();
    if (hdrFramebuffer_.isValid()) (void)device_.destroyFramebuffer(hdrFramebuffer_);
    if (hdrColorTex_.isValid()) (void)device_.destroyTexture(hdrColorTex_);
    if (hdrDepthTex_.isValid()) (void)device_.destroyTexture(hdrDepthTex_);
    hdrFramebuffer_ = {};
    hdrColorTex_ = {};
    hdrDepthTex_ = {};
    bloomExtent_ = {};
    bloomLevelCount_ = 0;
}

auto Renderer::ensureBloomResources(gfx::Extent extent) -> gfx::GfxError {
    if (!bloomVs_.isValid()) {
        auto shader = loadEffectShader(device_, EffectShader::BloomVertex);
        if (!shader.hasValue()) return shader.error();
        bloomVs_ = shader.value();
    }
    if (!bloomFs_.isValid()) {
        auto shader = loadEffectShader(device_, EffectShader::BloomFragment);
        if (!shader.hasValue()) return shader.error();
        bloomFs_ = shader.value();
    }
    if (!bloomTriangle_.isValid()) {
        // One triangle covering the whole viewport, already in clip space.
        const float positions[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        gfx::BufferDesc desc;
        desc.sizeBytes = sizeof(positions);
        desc.usage = gfx::BufferUsage::Vertex;
        auto buffer = device_.createBuffer(
            desc, std::span(reinterpret_cast<const std::byte*>(positions), sizeof(positions)));
        if (!buffer.hasValue()) return buffer.error();
        bloomTriangle_ = buffer.value();
    }
    if (!bloomSampler_.isValid()) {
        gfx::SamplerDesc desc; // bilinear, clamp to edge
        auto sampler = device_.createSampler(desc);
        if (!sampler.hasValue()) return sampler.error();
        bloomSampler_ = sampler.value();
    }
    const auto makePipeline = [this](gfx::TextureFormat format, bool depthCompatible)
        -> Result<gfx::PipelineHandle, gfx::GfxError> {
        gfx::PipelineStateDesc desc;
        desc.vertexShader = bloomVs_;
        desc.fragmentShader = bloomFs_;
        desc.vertexInput.bindings.push({0, 8, 0});
        desc.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x2, 0});
        desc.topology = gfx::PrimitiveTopology::TriangleList;
        desc.raster.cullMode = gfx::CullMode::None;
        // The composite draws into the caller's framebuffer, which carries a
        // depth attachment; declare it without testing or writing.
        desc.depthStencil.depthAttachmentEnable = depthCompatible;
        desc.depthStencil.format = gfx::TextureFormat::Depth32F;
        gfx::ColorAttachmentState color;
        color.format = format;
        desc.colorAttachments.push(color);
        gfx::BindGroup group;
        group.bindPoint = 0;
        group.slots.push({0, gfx::BindingType::UniformBuffer,
                          gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            group.slots.push({slot, gfx::BindingType::Texture,
                              gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
            group.slots.push({slot, gfx::BindingType::Sampler,
                              gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
        }
        desc.layout.groups.push(static_cast<gfx::BindGroup&&>(group));
        return device_.createPipeline(desc);
    };
    if (!bloomHdrPipeline_.isValid()) {
        auto pipeline = makePipeline(gfx::TextureFormat::RGBA16_FLOAT, false);
        if (!pipeline.hasValue()) return pipeline.error();
        bloomHdrPipeline_ = pipeline.value();
    }
    if (!bloomCompositePipeline_.isValid()) {
        auto pipeline = makePipeline(gfx::TextureFormat::RGBA8_UNORM, true);
        if (!pipeline.hasValue()) return pipeline.error();
        bloomCompositePipeline_ = pipeline.value();
    }

    const std::uint32_t levels = std::clamp(pbrSettings_.bloomLevels, 1u, kBloomMaxLevels);
    const math::Vec4 clearColor = pbrSettings_.bloomClearColor;
    if (hdrColorTex_.isValid() && extent == bloomExtent_ && levels == bloomLevelCount_ &&
        clearColor == hdrClearColor_) {
        return gfx::GfxError::None;
    }
    releaseBloomTargets();
    const float clear[4] = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    auto color = createRenderTexture(device_, gfx::TextureFormat::RGBA16_FLOAT, extent, clear,
                                     1.0f);
    if (!color.hasValue()) return color.error();
    hdrColorTex_ = color.value();
    auto depth = createRenderTexture(device_, gfx::TextureFormat::Depth32F, extent, black, 1.0f);
    if (!depth.hasValue()) {
        releaseBloomTargets();
        return depth.error();
    }
    hdrDepthTex_ = depth.value();
    {
        gfx::FramebufferDesc desc;
        desc.extent = extent;
        gfx::AttachmentDesc colorAttachment;
        colorAttachment.type = gfx::AttachmentType::Color;
        colorAttachment.texture = hdrColorTex_;
        colorAttachment.clearOnLoad = true;
        for (std::size_t i = 0; i < 4; ++i) colorAttachment.clear.color[i] = clear[i];
        desc.attachments.push(colorAttachment);
        gfx::AttachmentDesc depthAttachment;
        depthAttachment.type = gfx::AttachmentType::Depth;
        depthAttachment.texture = hdrDepthTex_;
        depthAttachment.clearOnLoad = true;
        depthAttachment.clear.depth = 1.0f;
        desc.attachments.push(depthAttachment);
        auto framebuffer = device_.createFramebuffer(desc);
        if (!framebuffer.hasValue()) {
            releaseBloomTargets();
            return framebuffer.error();
        }
        hdrFramebuffer_ = framebuffer.value();
    }
    const auto makeTarget = [&](gfx::Extent size, BloomTarget& target) -> gfx::GfxError {
        auto texture =
            createRenderTexture(device_, gfx::TextureFormat::RGBA16_FLOAT, size, black, 1.0f);
        if (!texture.hasValue()) return texture.error();
        target.texture = texture.value();
        target.extent = size;
        gfx::FramebufferDesc desc;
        desc.extent = size;
        gfx::AttachmentDesc attachment;
        attachment.type = gfx::AttachmentType::Color;
        attachment.texture = target.texture;
        // Every bloom pass overwrites its whole target with a fullscreen
        // triangle; clearing (to the optimized black) instead of loading keeps
        // the pass from reading the previous contents, which Vulkan sync
        // validation flags after the layout transition.
        attachment.clearOnLoad = true;
        for (std::size_t i = 0; i < 4; ++i) attachment.clear.color[i] = black[i];
        desc.attachments.push(attachment);
        auto framebuffer = device_.createFramebuffer(desc);
        if (!framebuffer.hasValue()) return framebuffer.error();
        target.framebuffer = framebuffer.value();
        return gfx::GfxError::None;
    };
    for (std::uint32_t level = 0; level < levels; ++level) {
        const gfx::Extent size{std::max(1u, extent.width >> (level + 1u)),
                               std::max(1u, extent.height >> (level + 1u))};
        bloomDown_.push(BloomTarget{});
        if (const auto error = makeTarget(size, bloomDown_[bloomDown_.size() - 1]);
            error != gfx::GfxError::None) {
            releaseBloomTargets();
            return error;
        }
        if (level + 1u < levels) {
            bloomUp_.push(BloomTarget{});
            if (const auto error = makeTarget(size, bloomUp_[bloomUp_.size() - 1]);
                error != gfx::GfxError::None) {
                releaseBloomTargets();
                return error;
            }
        }
    }
    bloomExtent_ = extent;
    bloomLevelCount_ = levels;
    hdrClearColor_ = clearColor;
    return gfx::GfxError::None;
}

void Renderer::declareBloomPasses(std::uint32_t hdrImage, std::uint32_t target,
                                  gfx::Extent extent) {
    using ResourceId = gfx::RenderGraph::ResourceId;
    const std::uint32_t levels = static_cast<std::uint32_t>(bloomDown_.size());
    containers::Array<ResourceId> downImages;
    containers::Array<ResourceId> downTargets;
    containers::Array<ResourceId> upImages;
    containers::Array<ResourceId> upTargets;
    for (std::uint32_t level = 0; level < levels; ++level) {
        downImages.push(graph_.importTexture(containers::String("bloom-down"),
                                             bloomDown_[level].texture));
        downTargets.push(graph_.importFramebuffer(containers::String("bloom-down-target"),
                                                  bloomDown_[level].framebuffer));
    }
    for (std::size_t level = 0; level < bloomUp_.size(); ++level) {
        upImages.push(graph_.importTexture(containers::String("bloom-up"),
                                           bloomUp_[level].texture));
        upTargets.push(graph_.importFramebuffer(containers::String("bloom-up-target"),
                                                bloomUp_[level].framebuffer));
    }
    const float threshold = std::max(pbrSettings_.bloomThreshold, 0.0f);
    const float knee = std::max(pbrSettings_.bloomKnee, 1.0e-4f);
    // up[0] is the sum of every blurred level (wide + narrow glow), weighted
    // once by the intensity in the composite.
    const float intensity = std::max(pbrSettings_.bloomIntensity, 0.0f);
    const auto texel = [](gfx::Extent size) {
        return math::Vec4(1.0f / static_cast<float>(size.width),
                          1.0f / static_cast<float>(size.height), 0.0f, 0.0f);
    };
    struct Source {
        ResourceId image;
        gfx::TextureHandle texture;
    };
    const auto addPass = [&](const char* name, ResourceId targetFramebuffer,
                             const ResourceId* targetImage, gfx::Extent targetExtent,
                             gfx::PipelineHandle pipeline, Source first, Source second,
                             BloomUbo constants) {
        gfx::PassDesc pass;
        pass.name = containers::String(name);
        pass.writes.push(targetFramebuffer);
        if (targetImage != nullptr) {
            pass.writes.push(*targetImage);
            pass.accesses.push({*targetImage,
                                {gfx::PipelineStageMask::ColorAttachment,
                                 gfx::ResourceAccessMask::ColorWrite,
                                 gfx::TextureLayout::ColorAttachment}});
        }
        for (const Source& source : {first, second}) {
            pass.reads.push(source.image);
            pass.accesses.push({source.image,
                                {gfx::PipelineStageMask::FragmentShader,
                                 gfx::ResourceAccessMask::ShaderRead,
                                 gfx::TextureLayout::ShaderRead}});
        }
        constants.params1.z = 1.0f / static_cast<float>(targetExtent.width);
        constants.params1.w = 1.0f / static_cast<float>(targetExtent.height);
        const gfx::TextureHandle firstTexture = first.texture;
        const gfx::TextureHandle secondTexture = second.texture;
        pass.record = [this, pipeline, firstTexture, secondTexture, constants,
                       targetExtent](gfx::CommandBuffer& commandBuffer) {
            const auto slot = bloomConstants_.alloc(
                device_, std::span(reinterpret_cast<const std::byte*>(&constants),
                                   sizeof(constants)));
            if (!slot.valid) {
                recordError_ = gfx::GfxError::OutOfMemory;
                return;
            }
            commandBuffer.setViewport(targetExtent);
            commandBuffer.bindPipeline(pipeline);
            bloomConstants_.bind(commandBuffer, slot);
            const gfx::TextureHandle textures[2] = {firstTexture, secondTexture};
            for (std::uint32_t resourceSlot = 0; resourceSlot < 2; ++resourceSlot) {
                commandBuffer.bindResource(resourceSlot, gfx::BindingType::Texture,
                                           textures[resourceSlot].index,
                                           textures[resourceSlot].generation);
                commandBuffer.bindResource(resourceSlot, gfx::BindingType::Sampler,
                                           bloomSampler_.index, bloomSampler_.generation);
            }
            commandBuffer.bindVertexBuffer(0, bloomTriangle_, 8);
            commandBuffer.draw(3);
        };
        (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(pass));
        ++bloomPasses_;
    };

    const Source hdr{hdrImage, hdrColorTex_};
    BloomUbo prefilter;
    prefilter.params0 = texel(extent);
    prefilter.params0.z = threshold;
    prefilter.params0.w = knee;
    prefilter.params1 = {0.0f, 0.0f, 0.0f, 0.0f};
    addPass("bloom-prefilter", downTargets[0], &downImages[0], bloomDown_[0].extent,
            bloomHdrPipeline_, hdr, hdr, prefilter);
    for (std::uint32_t level = 1; level < levels; ++level) {
        BloomUbo down;
        down.params0 = texel(bloomDown_[level - 1].extent);
        down.params1 = {1.0f, 0.0f, 0.0f, 0.0f};
        const Source source{downImages[level - 1], bloomDown_[level - 1].texture};
        addPass("bloom-down", downTargets[level], &downImages[level], bloomDown_[level].extent,
                bloomHdrPipeline_, source, source, down);
    }
    for (std::uint32_t level = levels - 1; level-- > 0;) {
        const bool coarsest = level + 2u == levels;
        const Source coarse = coarsest
                                  ? Source{downImages[level + 1], bloomDown_[level + 1].texture}
                                  : Source{upImages[level + 1], bloomUp_[level + 1].texture};
        BloomUbo up;
        up.params0 = texel(bloomDown_[level + 1].extent);
        up.params1 = {2.0f, 0.0f, 0.0f, 0.0f};
        addPass("bloom-up", upTargets[level], &upImages[level], bloomUp_[level].extent,
                bloomHdrPipeline_, coarse, Source{downImages[level], bloomDown_[level].texture},
                up);
    }
    const Source bloom = levels > 1 ? Source{upImages[0], bloomUp_[0].texture}
                                    : Source{downImages[0], bloomDown_[0].texture};
    BloomUbo composite;
    composite.params0 = texel(extent);
    composite.params1 = {3.0f, intensity, 0.0f, 0.0f};
    addPass("bloom-composite", target, nullptr, extent, bloomCompositePipeline_, hdr, bloom,
            composite);
}

auto Renderer::render(gfx::FramebufferHandle framebuffer, ecs::Entity cameraEntity,
                      float aspect, gfx::Extent extent) -> gfx::GfxError {
    if (!inited_) return gfx::GfxError::BadDescriptor;
    if (!framebuffer.isValid()) return gfx::GfxError::InvalidHandle;
    const bool bloom = pbrSettings_.bloomEnabled;
    if (bloom && (extent.width == 0 || extent.height == 0)) return gfx::GfxError::BadDescriptor;
    if (const auto error = graph_.clear(device_); error != gfx::GfxError::None) return error;
    updateCameras(registry_, aspect);
    const Camera* camera = registry_.get<Camera>(cameraEntity);
    if (camera == nullptr) return gfx::GfxError::InvalidHandle;
    const FrameLight light = findFrameLight(registry_, *camera, pbrSettings_);
    frameLightViewProj_ = light.viewProj;
    frameShadowsActive_ = false;
    if (pbrSettings_.shadowsEnabled && light.valid) {
        for (auto [entity, renderer] : ecs::View<MeshRenderer>(registry_)) {
            (void)entity;
            if (renderer.castShadows) {
                frameShadowsActive_ = true;
                break;
            }
        }
    }
    recordError_ = gfx::GfxError::None;
    pointShadowDraws_ = 0;
    pointShadowRedrawn_ = 0;
    bloomPasses_ = 0;
    const auto finish = [this](gfx::GfxError error) {
        frameShadowsActive_ = false;
        framePointShadowsActive_ = false;
        frameBloomActive_ = false;
        pointLightsGathered_ = false;
        framePointLightsReady_ = false;
        recordColorFormat_ = gfx::TextureFormat::RGBA8_UNORM;
        return error;
    };
    if (const auto error = preparePointShadows(*camera); error != gfx::GfxError::None) {
        return finish(error);
    }
    if (bloom) {
        if (const auto error = ensureBloomResources(extent); error != gfx::GfxError::None) {
            return finish(error);
        }
        bloomConstants_.beginFrame();
    }

    // GPU light binning runs first; record() then binds its output.
    if (preparePointLightsGpu(*camera, aspect)) {
        gfx::PassDesc binningPass;
        binningPass.name = containers::String("light-binning");
        binningPass.record = [this](gfx::CommandBuffer& commandBuffer) {
            recordLightBinning(commandBuffer);
        };
        (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(binningPass));
    }

    gfx::RenderGraph::ResourceId shadowImage = 0;
    if (frameShadowsActive_) {
        shadowImage = graph_.importTexture(containers::String("shadow-map"), shadowColorTex_);
        const auto shadowTarget = graph_.importFramebuffer(
            containers::String("shadow-target"), shadowFramebuffer_);
        gfx::PassDesc shadowPass;
        shadowPass.name = containers::String("shadow");
        shadowPass.writes.push(shadowTarget);
        shadowPass.writes.push(shadowImage);
        shadowPass.accesses.push({
            shadowImage,
            {gfx::PipelineStageMask::ColorAttachment,
             gfx::ResourceAccessMask::ColorWrite,
             gfx::TextureLayout::ColorAttachment},
        });
        shadowPass.record = [this](gfx::CommandBuffer& commandBuffer) {
            recordShadow(commandBuffer);
        };
        (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(shadowPass));
    }

    gfx::RenderGraph::ResourceId pointShadowImage = 0;
    if (framePointShadowsActive_) {
        pointShadowImage = graph_.importTexture(containers::String("point-shadow-atlas"),
                                                pointShadowColorTex_);
    }
    // With every shadowed light cached the atlas pass is skipped entirely and
    // the main pass samples last frame's tiles.
    if (framePointShadowsActive_ && framePointShadowsDirty_ && !framePointShadowsFullClear_) {
        // A partial redraw loads the cached tiles. Start from the state last
        // frame left the atlas in (sampled by the main pass) so the barrier
        // into the atlas pass orders after those reads and keeps the texels.
        gfx::PassDesc keepPass;
        keepPass.name = containers::String("point-shadow-atlas-keep");
        keepPass.reads.push(pointShadowImage);
        keepPass.accesses.push({
            pointShadowImage,
            {gfx::PipelineStageMask::FragmentShader,
             gfx::ResourceAccessMask::ShaderRead,
             gfx::TextureLayout::ShaderRead},
        });
        (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(keepPass));
    }
    if (framePointShadowsActive_ && framePointShadowsDirty_) {
        const auto atlasTarget = graph_.importFramebuffer(
            containers::String("point-shadow-target"),
            framePointShadowsFullClear_ ? pointShadowFramebuffer_ : pointShadowLoadFramebuffer_);
        gfx::PassDesc atlasPass;
        atlasPass.name = containers::String("point-shadows");
        atlasPass.writes.push(atlasTarget);
        atlasPass.writes.push(pointShadowImage);
        atlasPass.accesses.push({
            pointShadowImage,
            {gfx::PipelineStageMask::ColorAttachment,
             framePointShadowsFullClear_
                 ? gfx::ResourceAccessMask::ColorWrite
                 : gfx::ResourceAccessMask::ColorRead | gfx::ResourceAccessMask::ColorWrite,
             gfx::TextureLayout::ColorAttachment},
        });
        atlasPass.record = [this](gfx::CommandBuffer& commandBuffer) {
            recordPointShadows(commandBuffer);
        };
        (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(atlasPass));
    }

    const auto target = graph_.importFramebuffer(containers::String("main-target"), framebuffer);
    gfx::RenderGraph::ResourceId hdrImage = 0;
    gfx::PassDesc mainPass;
    mainPass.name = containers::String("main");
    if (bloom) {
        hdrImage = graph_.importTexture(containers::String("hdr-color"), hdrColorTex_);
        const auto hdrTarget =
            graph_.importFramebuffer(containers::String("hdr-target"), hdrFramebuffer_);
        mainPass.writes.push(hdrTarget);
        mainPass.writes.push(hdrImage);
        mainPass.accesses.push({
            hdrImage,
            {gfx::PipelineStageMask::ColorAttachment,
             gfx::ResourceAccessMask::ColorWrite,
             gfx::TextureLayout::ColorAttachment},
        });
    } else {
        mainPass.writes.push(target);
    }
    if (frameShadowsActive_) {
        mainPass.reads.push(shadowImage);
        mainPass.accesses.push({
            shadowImage,
            {gfx::PipelineStageMask::FragmentShader,
             gfx::ResourceAccessMask::ShaderRead,
             gfx::TextureLayout::ShaderRead},
        });
    }
    if (framePointShadowsActive_) {
        mainPass.reads.push(pointShadowImage);
        mainPass.accesses.push({
            pointShadowImage,
            {gfx::PipelineStageMask::FragmentShader,
             gfx::ResourceAccessMask::ShaderRead,
             gfx::TextureLayout::ShaderRead},
        });
    }
    mainPass.record = [this, cameraEntity, aspect](gfx::CommandBuffer& commandBuffer) {
        record(commandBuffer, cameraEntity, aspect);
    };
    (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(mainPass));
    if (bloom) declareBloomPasses(hdrImage, target, extent);

    frameBloomActive_ = bloom;
    recordColorFormat_ =
        bloom ? gfx::TextureFormat::RGBA16_FLOAT : gfx::TextureFormat::RGBA8_UNORM;
    gfx::CommandBuffer commandBuffer;
    const gfx::GfxError submitError = graph_.execute(device_, commandBuffer);
    std::uint32_t activeShadowLights = 0;
    if (framePointShadowsActive_) {
        const bool drawn = submitError == gfx::GfxError::None &&
                           recordError_ == gfx::GfxError::None;
        for (std::size_t slot = 0; slot < frameShadowedLights_.size(); ++slot) {
            const ShadowedPointLight& shadowed = frameShadowedLights_[slot];
            if (!shadowed.active) continue;
            ++activeShadowLights;
            if (!shadowed.dirty) continue;
            pointShadowSlotSignatures_[slot] = shadowed.signature;
            pointShadowSlotValid_[slot] =
                static_cast<std::uint8_t>(drawn && pbrSettings_.pointShadowCaching ? 1u : 0u);
        }
        if (!drawn) {
            std::fill(pointShadowSlotValid_.begin(), pointShadowSlotValid_.end(),
                      std::uint8_t{0});
        }
    }
    stats_.pointShadowLights = activeShadowLights;
    stats_.pointShadowDraws = pointShadowDraws_;
    stats_.pointShadowLightsRedrawn =
        framePointShadowsActive_ && framePointShadowsDirty_ ? pointShadowRedrawn_ : 0u;
    stats_.bloomPasses = bloomPasses_;
    return finish(submitError != gfx::GfxError::None ? submitError : recordError_);
}

} // namespace f4::engine
