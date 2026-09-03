#include "engine/rendering/material.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

#include "graphics/shader/shader_desc.h"

namespace f4::engine {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hashWord(std::uint64_t& hash, std::uint32_t value) {
    hash ^= value;
    hash *= kFnvPrime;
}

void hashFloat(std::uint64_t& hash, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hashWord(hash, bits);
}

void hashHandle(std::uint64_t& hash, resources::TextureHandle value) {
    hashWord(hash, value.index);
    hashWord(hash, value.generation);
}

[[nodiscard]] auto layoutsEqual(const gfx::VertexInputLayout& a,
                                const gfx::VertexInputLayout& b) -> bool {
    if (a.bindings.size() != b.bindings.size() ||
        a.attributes.size() != b.attributes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        if (a.bindings[i].binding != b.bindings[i].binding ||
            a.bindings[i].strideBytes != b.bindings[i].strideBytes ||
            a.bindings[i].divisor != b.bindings[i].divisor) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.attributes.size(); ++i) {
        if (a.attributes[i].binding != b.attributes[i].binding ||
            a.attributes[i].location != b.attributes[i].location ||
            a.attributes[i].format != b.attributes[i].format ||
            a.attributes[i].offsetBytes != b.attributes[i].offsetBytes) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto hashLayout(const gfx::VertexInputLayout& layout) -> std::uint64_t {
    std::uint64_t hash = kFnvOffset;
    hashWord(hash, static_cast<std::uint32_t>(layout.bindings.size()));
    for (const auto& binding : layout.bindings) {
        hashWord(hash, binding.binding);
        hashWord(hash, binding.strideBytes);
        hashWord(hash, binding.divisor);
    }
    hashWord(hash, static_cast<std::uint32_t>(layout.attributes.size()));
    for (const auto& attribute : layout.attributes) {
        hashWord(hash, attribute.binding);
        hashWord(hash, attribute.location);
        hashWord(hash, static_cast<std::uint32_t>(attribute.format));
        hashWord(hash, attribute.offsetBytes);
    }
    return hash;
}

[[nodiscard]] auto canonicalVertexInput() -> gfx::VertexInputLayout {
    gfx::VertexInputLayout layout;
    layout.bindings.push({0, 36, 0});
    layout.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    layout.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    layout.attributes.push({0, 2, gfx::VertexFormat::Float32x2, 24});
    layout.attributes.push({0, 3, gfx::VertexFormat::UNorm8x4, 32});
    return layout;
}

[[nodiscard]] auto shaderPaths(gfx::Backend backend, const char*& vertex,
                               const char*& instancedVertex, const char*& fragment,
                               const char*& shadowVertex,
                               const char*& shadowFragment) -> bool {
    switch (backend) {
    case gfx::Backend::OpenGL:
#if defined(F4_PBR_OPENGL_VERTEX) && defined(F4_PBR_OPENGL_FRAGMENT) && \
    defined(F4_PBR_OPENGL_INSTANCED_VERTEX) && \
    defined(F4_SHADOW_OPENGL_VERTEX) && defined(F4_SHADOW_OPENGL_FRAGMENT)
        vertex = F4_PBR_OPENGL_VERTEX;
        instancedVertex = F4_PBR_OPENGL_INSTANCED_VERTEX;
        fragment = F4_PBR_OPENGL_FRAGMENT;
        shadowVertex = F4_SHADOW_OPENGL_VERTEX;
        shadowFragment = F4_SHADOW_OPENGL_FRAGMENT;
        return true;
#else
        return false;
#endif
    case gfx::Backend::Vulkan:
#if defined(F4_PBR_VULKAN_VERTEX) && defined(F4_PBR_VULKAN_FRAGMENT) && \
    defined(F4_PBR_VULKAN_INSTANCED_VERTEX) && \
    defined(F4_SHADOW_VULKAN_VERTEX) && defined(F4_SHADOW_VULKAN_FRAGMENT)
        vertex = F4_PBR_VULKAN_VERTEX;
        instancedVertex = F4_PBR_VULKAN_INSTANCED_VERTEX;
        fragment = F4_PBR_VULKAN_FRAGMENT;
        shadowVertex = F4_SHADOW_VULKAN_VERTEX;
        shadowFragment = F4_SHADOW_VULKAN_FRAGMENT;
        return true;
#else
        return false;
#endif
    case gfx::Backend::DirectX12:
#if defined(F4_PBR_DX12_VERTEX) && defined(F4_PBR_DX12_FRAGMENT) && \
    defined(F4_PBR_DX12_INSTANCED_VERTEX) && \
    defined(F4_SHADOW_DX12_VERTEX) && defined(F4_SHADOW_DX12_FRAGMENT)
        vertex = F4_PBR_DX12_VERTEX;
        instancedVertex = F4_PBR_DX12_INSTANCED_VERTEX;
        fragment = F4_PBR_DX12_FRAGMENT;
        shadowVertex = F4_SHADOW_DX12_VERTEX;
        shadowFragment = F4_SHADOW_DX12_FRAGMENT;
        return true;
#else
        return false;
#endif
    case gfx::Backend::Null:
        return true;
    }
    return false;
}

[[nodiscard]] auto hasInstancedTransforms(const gfx::VertexInputLayout& layout) -> bool {
    std::uint32_t instanceBinding = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < 7; ++index) {
        const std::uint32_t location = 9u + index;
        const std::uint32_t offset = index * 16u;
        const auto attribute = std::find_if(
            layout.attributes.begin(), layout.attributes.end(),
            [&](const auto& candidate) {
                return candidate.location == location &&
                       candidate.format == gfx::VertexFormat::Float32x4 &&
                       candidate.offsetBytes == offset;
            });
        if (attribute == layout.attributes.end()) return false;
        if (index == 0) {
            instanceBinding = attribute->binding;
        } else if (attribute->binding != instanceBinding) {
            return false;
        }
    }
    const auto binding = std::find_if(
        layout.bindings.begin(), layout.bindings.end(),
        [&](const auto& candidate) { return candidate.binding == instanceBinding; });
    return binding != layout.bindings.end() && binding->divisor == 1 &&
           binding->strideBytes >= 112;
}

[[nodiscard]] auto loadShader(const char* path, gfx::ShaderStage stage,
                              gfx::Backend backend, gfx::ShaderDesc& output) -> bool {
    output.stage = stage;
    if (backend == gfx::Backend::Null) {
        output.blob.push(std::byte{0});
        return true;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    output.blob.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(output.blob.data()), size));
}

void addPbrBindings(gfx::PipelineStateDesc& pipeline) {
    gfx::BindGroup group;
    group.bindPoint = 0;
    group.slots.push({0, gfx::BindingType::UniformBuffer,
                      gfx::shaderStageBit(gfx::ShaderStage::Vertex) |
                          gfx::shaderStageBit(gfx::ShaderStage::Fragment),
                      1});
    for (std::uint32_t slot = 0; slot < 7; ++slot) {
        group.slots.push({slot, gfx::BindingType::Texture,
                          gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
        group.slots.push({slot, gfx::BindingType::Sampler,
                          gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    }
    pipeline.layout.groups.push(static_cast<gfx::BindGroup&&>(group));
}

} // namespace

auto materialFromGltf(const resources::GltfMaterial& source,
                      std::span<const resources::TextureHandle> textures)
    -> Result<StandardMaterialDesc, MaterialImportError> {
    StandardMaterialDesc result;
    result.tint = source.baseColorFactor;
    result.emissiveFactor = source.emissiveFactor;
    result.metallicFactor = source.metallicFactor;
    result.roughnessFactor = source.roughnessFactor;
    result.normalScale = source.normalTexture.scale;
    result.occlusionStrength = source.occlusionTexture.scale;
    result.alphaCutoff = source.alphaCutoff;
    result.doubleSided = source.doubleSided;
    result.unlit = source.unlit;
    switch (source.alphaMode) {
    case resources::GltfAlphaMode::Opaque:
        result.alphaMode = MaterialAlphaMode::Opaque;
        break;
    case resources::GltfAlphaMode::Mask:
        result.alphaMode = MaterialAlphaMode::Mask;
        break;
    case resources::GltfAlphaMode::Blend:
        result.alphaMode = MaterialAlphaMode::Blend;
        break;
    }

    const auto mapTexture = [textures](const resources::GltfTextureRef& ref,
                                       resources::TextureHandle& destination)
        -> MaterialImportError {
        if (!ref.hasTexture()) return MaterialImportError::None;
        if (ref.texCoord != 0) return MaterialImportError::UnsupportedTexCoord;
        if (ref.textureIndex >= textures.size()) {
            return MaterialImportError::TextureIndexOutOfRange;
        }
        destination = textures[ref.textureIndex];
        if (!destination.isValid()) return MaterialImportError::InvalidTextureHandle;
        return MaterialImportError::None;
    };

    const resources::GltfTextureRef* refs[] = {
        &source.baseColorTexture,
        &source.normalTexture,
        &source.metallicRoughnessTexture,
        &source.occlusionTexture,
        &source.emissiveTexture,
    };
    resources::TextureHandle* destinations[] = {
        &result.albedo,
        &result.normal,
        &result.metallicRoughness,
        &result.occlusion,
        &result.emissive,
    };
    for (std::size_t i = 0; i < 5; ++i) {
        if (!refs[i]->hasTexture()) continue;
        const MaterialImportError error = mapTexture(*refs[i], *destinations[i]);
        if (error != MaterialImportError::None) return error;
    }
    return result;
}

auto MaterialCache::hashDesc(const StandardMaterialDesc& desc) -> std::uint64_t {
    std::uint64_t hash = kFnvOffset;
    hashFloat(hash, desc.tint.x);
    hashFloat(hash, desc.tint.y);
    hashFloat(hash, desc.tint.z);
    hashFloat(hash, desc.tint.w);
    hashFloat(hash, desc.emissiveFactor.x);
    hashFloat(hash, desc.emissiveFactor.y);
    hashFloat(hash, desc.emissiveFactor.z);
    hashFloat(hash, desc.metallicFactor);
    hashFloat(hash, desc.roughnessFactor);
    hashFloat(hash, desc.normalScale);
    hashFloat(hash, desc.normalYSign);
    hashFloat(hash, desc.occlusionStrength);
    hashFloat(hash, desc.alphaCutoff);
    hashHandle(hash, desc.albedo);
    hashHandle(hash, desc.normal);
    hashHandle(hash, desc.metallicRoughness);
    hashHandle(hash, desc.occlusion);
    hashHandle(hash, desc.emissive);
    hashWord(hash, static_cast<std::uint32_t>(desc.alphaMode));
    hashWord(hash, desc.doubleSided ? 1u : 0u);
    hashWord(hash, desc.unlit ? 1u : 0u);
    hashWord(hash, desc.transparent ? 1u : 0u);
    return hash;
}

auto MaterialCache::find(const StandardMaterialDesc& desc) const -> resources::MaterialHandle {
    const std::uint64_t hash = hashDesc(desc);
    const auto* slot = lookup_.find(hash);
    if (slot == nullptr) return {};
    const StandardMaterial* material = arena_.get(*slot);
    return material != nullptr && material->desc == desc ? *slot : resources::MaterialHandle{};
}

auto MaterialCache::getOrCreate(const StandardMaterialDesc& desc) -> resources::MaterialHandle {
    if (const auto existing = find(desc); existing.isValid()) return existing;
    StandardMaterial material;
    material.desc = desc;
    const auto handle = arena_.acquire(static_cast<StandardMaterial&&>(material));
    lookup_.set(hashDesc(desc), handle);
    return handle;
}

auto MaterialCache::ensureShaders(gfx::IDevice& dev) -> gfx::GfxError {
    if (owner_ != nullptr && owner_ != &dev) return gfx::GfxError::InvalidHandle;
    if (pbrVs_.isValid() && pbrInstancedVs_.isValid() && pbrFs_.isValid() && shadowVs_.isValid() &&
        shadowFs_.isValid()) {
        return gfx::GfxError::None;
    }

    const char* vertex = nullptr;
    const char* instancedVertex = nullptr;
    const char* fragment = nullptr;
    const char* shadowVertex = nullptr;
    const char* shadowFragment = nullptr;
    if (!shaderPaths(dev.backend(), vertex, instancedVertex, fragment, shadowVertex,
                     shadowFragment)) {
        return gfx::GfxError::BackendUnavailable;
    }

    gfx::ShaderDesc descriptions[5];
    if (!loadShader(vertex, gfx::ShaderStage::Vertex, dev.backend(), descriptions[0]) ||
        !loadShader(instancedVertex, gfx::ShaderStage::Vertex, dev.backend(), descriptions[1]) ||
        !loadShader(fragment, gfx::ShaderStage::Fragment, dev.backend(), descriptions[2]) ||
        !loadShader(shadowVertex, gfx::ShaderStage::Vertex, dev.backend(), descriptions[3]) ||
        !loadShader(shadowFragment, gfx::ShaderStage::Fragment, dev.backend(), descriptions[4])) {
        return gfx::GfxError::BackendUnavailable;
    }

    gfx::ShaderHandle handles[5];
    for (std::size_t i = 0; i < 5; ++i) {
        auto result = dev.createShader(descriptions[i]);
        if (!result.hasValue()) {
            for (std::size_t j = 0; j < i; ++j) (void)dev.destroyShader(handles[j]);
            return result.error();
        }
        handles[i] = result.value();
    }
    pbrVs_ = handles[0];
    pbrInstancedVs_ = handles[1];
    pbrFs_ = handles[2];
    shadowVs_ = handles[3];
    shadowFs_ = handles[4];
    owner_ = &dev;
    return gfx::GfxError::None;
}

auto MaterialCache::ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle handle)
    -> gfx::GfxError {
    return ensurePipeline(dev, handle, canonicalVertexInput());
}

auto MaterialCache::ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle handle,
                                   const gfx::VertexInputLayout& vertexInput)
    -> gfx::GfxError {
    StandardMaterial* material = arena_.get(handle);
    if (material == nullptr) return gfx::GfxError::InvalidHandle;
    if (pipelineFor(handle, vertexInput).isValid()) return gfx::GfxError::None;
    if (const auto error = ensureShaders(dev); error != gfx::GfxError::None) return error;

    gfx::PipelineStateDesc pipeline;
    pipeline.vertexShader = hasInstancedTransforms(vertexInput) ? pbrInstancedVs_ : pbrVs_;
    pipeline.fragmentShader = pbrFs_;
    pipeline.vertexInput = vertexInput;
    pipeline.topology = gfx::PrimitiveTopology::TriangleList;
    pipeline.raster.cullMode = material->desc.doubleSided ? gfx::CullMode::None
                                                          : gfx::CullMode::Back;
    pipeline.depthStencil.depthTestEnable = true;
    const bool blended = material->desc.transparent ||
                         material->desc.alphaMode == MaterialAlphaMode::Blend;
    pipeline.depthStencil.depthWriteEnable = !blended;
    pipeline.depthStencil.depthCompare = gfx::CompareOp::Less;
    gfx::ColorAttachmentState color;
    color.blendEnable = blended;
    if (blended) {
        color.srcColor = gfx::BlendFactor::SrcAlpha;
        color.dstColor = gfx::BlendFactor::OneMinusSrcAlpha;
        color.srcAlpha = gfx::BlendFactor::One;
        color.dstAlpha = gfx::BlendFactor::OneMinusSrcAlpha;
    }
    pipeline.colorAttachments.push(color);
    addPbrBindings(pipeline);

    auto result = dev.createPipeline(pipeline);
    if (!result.hasValue()) return result.error();
    MaterialPipelineVariant variant;
    variant.vertexLayoutHash = hashLayout(vertexInput);
    variant.vertexInput = vertexInput;
    variant.pipeline = result.value();
    material->variants.push(static_cast<MaterialPipelineVariant&&>(variant));
    if (!material->pipeline.isValid()) material->pipeline = result.value();
    return gfx::GfxError::None;
}

auto MaterialCache::pipelineFor(resources::MaterialHandle handle,
                                const gfx::VertexInputLayout& vertexInput) const
    -> gfx::PipelineHandle {
    const StandardMaterial* material = arena_.get(handle);
    if (material == nullptr) return {};
    const std::uint64_t hash = hashLayout(vertexInput);
    for (const MaterialPipelineVariant& variant : material->variants) {
        if (variant.vertexLayoutHash == hash && layoutsEqual(variant.vertexInput, vertexInput)) {
            return variant.pipeline;
        }
    }
    return {};
}

auto MaterialCache::ensureShadowPipeline(gfx::IDevice& dev,
                                         const gfx::VertexInputLayout& vertexInput)
    -> gfx::GfxError {
    if (shadowPipelineFor(vertexInput).isValid()) return gfx::GfxError::None;
    if (const auto error = ensureShaders(dev); error != gfx::GfxError::None) return error;

    gfx::PipelineStateDesc pipeline;
    pipeline.vertexShader = shadowVs_;
    pipeline.fragmentShader = shadowFs_;
    for (const auto& binding : vertexInput.bindings) pipeline.vertexInput.bindings.push(binding);
    for (const auto& attribute : vertexInput.attributes) {
        if (attribute.location == 0 || attribute.location == 2) {
            pipeline.vertexInput.attributes.push(attribute);
        }
    }
    pipeline.topology = gfx::PrimitiveTopology::TriangleList;
    pipeline.raster.cullMode = gfx::CullMode::None;
    pipeline.depthStencil.depthTestEnable = true;
    pipeline.depthStencil.depthWriteEnable = true;
    pipeline.depthStencil.depthCompare = gfx::CompareOp::Less;
    pipeline.depthStencil.format = gfx::TextureFormat::Depth32F;
    gfx::ColorAttachmentState color;
    color.format = gfx::TextureFormat::R16_FLOAT;
    pipeline.colorAttachments.push(color);
    gfx::BindGroup group;
    group.bindPoint = 0;
    group.slots.push({0, gfx::BindingType::UniformBuffer,
                      gfx::shaderStageBit(gfx::ShaderStage::Vertex), 1});
    group.slots.push({0, gfx::BindingType::Texture,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    group.slots.push({0, gfx::BindingType::Sampler,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    pipeline.layout.groups.push(static_cast<gfx::BindGroup&&>(group));

    auto result = dev.createPipeline(pipeline);
    if (!result.hasValue()) return result.error();
    MaterialPipelineVariant variant;
    variant.vertexLayoutHash = hashLayout(vertexInput);
    variant.vertexInput = vertexInput;
    variant.pipeline = result.value();
    shadowVariants_.push(static_cast<MaterialPipelineVariant&&>(variant));
    return gfx::GfxError::None;
}

auto MaterialCache::shadowPipelineFor(const gfx::VertexInputLayout& vertexInput) const
    -> gfx::PipelineHandle {
    const std::uint64_t hash = hashLayout(vertexInput);
    for (const MaterialPipelineVariant& variant : shadowVariants_) {
        if (variant.vertexLayoutHash == hash && layoutsEqual(variant.vertexInput, vertexInput)) {
            return variant.pipeline;
        }
    }
    return {};
}

auto MaterialCache::destroy(resources::MaterialHandle handle, gfx::IDevice& dev)
    -> gfx::GfxError {
    StandardMaterial* material = arena_.get(handle);
    if (material == nullptr) return gfx::GfxError::InvalidHandle;
    for (const MaterialPipelineVariant& variant : material->variants) {
        if (variant.pipeline.isValid()) (void)dev.destroyPipeline(variant.pipeline);
    }
    lookup_.erase(hashDesc(material->desc));
    arena_.release(handle);
    return gfx::GfxError::None;
}

void MaterialCache::clear(gfx::IDevice& dev) {
    arena_.forEach([&](StandardMaterial* material) {
        for (const MaterialPipelineVariant& variant : material->variants) {
            if (variant.pipeline.isValid()) (void)dev.destroyPipeline(variant.pipeline);
        }
    });
    for (const MaterialPipelineVariant& variant : shadowVariants_) {
        if (variant.pipeline.isValid()) (void)dev.destroyPipeline(variant.pipeline);
    }
    shadowVariants_.clear();
    if (pbrVs_.isValid()) (void)dev.destroyShader(pbrVs_);
    if (pbrInstancedVs_.isValid()) (void)dev.destroyShader(pbrInstancedVs_);
    if (pbrFs_.isValid()) (void)dev.destroyShader(pbrFs_);
    if (shadowVs_.isValid()) (void)dev.destroyShader(shadowVs_);
    if (shadowFs_.isValid()) (void)dev.destroyShader(shadowFs_);
    pbrVs_ = {};
    pbrInstancedVs_ = {};
    pbrFs_ = {};
    shadowVs_ = {};
    shadowFs_ = {};
    owner_ = nullptr;
    lookup_.clear();
    arena_.clear();
}

} // namespace f4::engine
