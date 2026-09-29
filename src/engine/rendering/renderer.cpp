#include "engine/rendering/renderer.h"

#include <algorithm>
#include <chrono>
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
    shadowConstants_.destroy(device_);
    sceneConstants_.destroy(device_);
    sceneInstances_.destroy(device_);
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
    if (const auto error = sceneInstances_.create(device_, gfx::BufferUsage::Vertex);
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
    if (meshArena_.full()) return {};
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
    if (textureArena_.full()) return {};
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
    const auto handle = createMaterial(converted.value());
    if (!handle.isValid()) return MaterialImportError::CapacityExceeded;
    return handle;
}

auto Renderer::ensureMaterialPipeline(resources::MaterialHandle handle) -> gfx::GfxError {
    return materials_.ensurePipeline(device_, handle);
}

auto Renderer::ensureMaterialPipeline(resources::MaterialHandle handle,
                                       resources::MeshHandle mesh, bool mirrored) -> gfx::GfxError {
    const auto* entry = meshMap_.find(mesh);
    if (entry == nullptr) return gfx::GfxError::InvalidHandle;
    return materials_.ensurePipeline(device_, handle, entry->instancedLayout, mirrored);
}

void Renderer::record(gfx::CommandBuffer& commandBuffer, ecs::Entity cameraEntity,
                      float aspect) {
    recordError_ = gfx::GfxError::None;
    recordMain(commandBuffer, cameraEntity, aspect);
}

void Renderer::recordMain(gfx::CommandBuffer& commandBuffer, ecs::Entity cameraEntity,
                          float aspect) {
    stats_ = {};
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
        const bool mirrored = world.matrix.determinant() < 0.0f;
        const auto pipelineStart = pbrSettings_.profileTimings
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto pipelineError = materials_.ensurePipeline(
            device_, renderer.material, entry->instancedLayout, mirrored);
        if (pbrSettings_.profileTimings) {
            stats_.materialPipelineUs += std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - pipelineStart).count();
        }
        if (const auto error = pipelineError;
            error != gfx::GfxError::None) {
            recordError_ = error;
            continue;
        }
        const gfx::PipelineHandle pipeline =
            materials_.pipelineFor(renderer.material, entry->instancedLayout, mirrored);
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
        item.mirrored = mirrored;
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
            materials_.pipelineFor(item.material, entry->instancedLayout, item.mirrored);
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
                                  static_cast<float>(flags), 0.0f, 0.0f};
        const auto slot = sceneConstants_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&constants),
                               sizeof(constants)));
        if (!slot.valid) {
            recordError_ = gfx::GfxError::OutOfMemory;
            continue;
        }

        commandBuffer.bindPipeline(pipeline);
        sceneConstants_.bind(commandBuffer, slot);
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

auto Renderer::render(gfx::FramebufferHandle framebuffer, ecs::Entity cameraEntity,
                      float aspect) -> gfx::GfxError {
    stats_.mainRecordUs = 0;
    stats_.materialPipelineUs = 0;
    stats_.graphExecuteUs = 0;
    if (!inited_) return gfx::GfxError::BadDescriptor;
    if (!framebuffer.isValid()) return gfx::GfxError::InvalidHandle;
    if (const auto error = graph_.clear(device_); error != gfx::GfxError::None) return error;
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

    const auto target = graph_.importFramebuffer(containers::String("main-target"), framebuffer);
    gfx::PassDesc mainPass;
    mainPass.name = containers::String("main");
    mainPass.writes.push(target);
    if (frameShadowsActive_) {
        mainPass.reads.push(shadowImage);
        mainPass.accesses.push({
            shadowImage,
            {gfx::PipelineStageMask::FragmentShader,
             gfx::ResourceAccessMask::ShaderRead,
             gfx::TextureLayout::ShaderRead},
        });
    }
    mainPass.record = [this, cameraEntity, aspect](gfx::CommandBuffer& commandBuffer) {
        const auto start = pbrSettings_.profileTimings
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        recordMain(commandBuffer, cameraEntity, aspect);
        if (pbrSettings_.profileTimings) {
            stats_.mainRecordUs = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
        }
    };
    (void)graph_.declarePass(static_cast<gfx::PassDesc&&>(mainPass));
    gfx::CommandBuffer commandBuffer;
    const auto executeStart = pbrSettings_.profileTimings
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const gfx::GfxError submitError = graph_.execute(device_, commandBuffer);
    if (pbrSettings_.profileTimings) {
        stats_.graphExecuteUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - executeStart).count();
    }
    frameShadowsActive_ = false;
    return submitError != gfx::GfxError::None ? submitError : recordError_;
}

} // namespace f4::engine
