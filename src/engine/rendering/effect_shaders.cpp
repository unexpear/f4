#include "engine/rendering/effect_shaders.h"

#include <cstring>
#include <fstream>

#include "graphics/shader/shader_desc.h"

#if defined(F4_WEBGPU_SHADER_HEADER)
#include F4_WEBGPU_SHADER_HEADER
#endif

namespace f4::engine {

namespace {

[[nodiscard]] auto lightBinningSource(gfx::Backend backend) -> const char* {
    switch (backend) {
    case gfx::Backend::OpenGL:
#if defined(F4_LIGHT_BINNING_OPENGL_COMPUTE)
        return F4_LIGHT_BINNING_OPENGL_COMPUTE;
#else
        return nullptr;
#endif
    case gfx::Backend::Vulkan:
#if defined(F4_LIGHT_BINNING_VULKAN_COMPUTE)
        return F4_LIGHT_BINNING_VULKAN_COMPUTE;
#else
        return nullptr;
#endif
    case gfx::Backend::DirectX12:
#if defined(F4_LIGHT_BINNING_DX12_COMPUTE)
        return F4_LIGHT_BINNING_DX12_COMPUTE;
#else
        return nullptr;
#endif
    case gfx::Backend::WebGPU:
#if defined(F4_WEBGPU_SHADER_HEADER)
        return web::kLightBinningComputeWgsl;
#else
        return nullptr;
#endif
    case gfx::Backend::Metal:
        return nullptr;
    case gfx::Backend::Null:
        return "";
    }
    return nullptr;
}

[[nodiscard]] auto effectSource(gfx::Backend backend, EffectShader shader) -> const char* {
    if (shader == EffectShader::LightBinningCompute) return lightBinningSource(backend);
    const auto pick = [shader](const char* pointVertex, const char* pointFragment,
                               const char* bloomVertex, const char* bloomFragment) {
        switch (shader) {
        case EffectShader::PointShadowVertex: return pointVertex;
        case EffectShader::PointShadowFragment: return pointFragment;
        case EffectShader::BloomVertex: return bloomVertex;
        case EffectShader::BloomFragment: return bloomFragment;
        case EffectShader::LightBinningCompute: return static_cast<const char*>(nullptr);
        }
        return static_cast<const char*>(nullptr);
    };
    switch (backend) {
    case gfx::Backend::OpenGL:
#if defined(F4_POINT_SHADOW_OPENGL_VERTEX) && defined(F4_BLOOM_OPENGL_FRAGMENT)
        return pick(F4_POINT_SHADOW_OPENGL_VERTEX, F4_POINT_SHADOW_OPENGL_FRAGMENT,
                    F4_BLOOM_OPENGL_VERTEX, F4_BLOOM_OPENGL_FRAGMENT);
#else
        return nullptr;
#endif
    case gfx::Backend::Vulkan:
#if defined(F4_POINT_SHADOW_VULKAN_VERTEX) && defined(F4_BLOOM_VULKAN_FRAGMENT)
        return pick(F4_POINT_SHADOW_VULKAN_VERTEX, F4_POINT_SHADOW_VULKAN_FRAGMENT,
                    F4_BLOOM_VULKAN_VERTEX, F4_BLOOM_VULKAN_FRAGMENT);
#else
        return nullptr;
#endif
    case gfx::Backend::DirectX12:
#if defined(F4_POINT_SHADOW_DX12_VERTEX) && defined(F4_BLOOM_DX12_FRAGMENT)
        return pick(F4_POINT_SHADOW_DX12_VERTEX, F4_POINT_SHADOW_DX12_FRAGMENT,
                    F4_BLOOM_DX12_VERTEX, F4_BLOOM_DX12_FRAGMENT);
#else
        return nullptr;
#endif
    case gfx::Backend::Metal:
#if defined(F4_POINT_SHADOW_METAL_VERTEX) && defined(F4_BLOOM_METAL_FRAGMENT)
        return pick(F4_POINT_SHADOW_METAL_VERTEX, F4_POINT_SHADOW_METAL_FRAGMENT,
                    F4_BLOOM_METAL_VERTEX, F4_BLOOM_METAL_FRAGMENT);
#else
        return nullptr;
#endif
    case gfx::Backend::WebGPU:
#if defined(F4_WEBGPU_SHADER_HEADER)
        return pick(web::kPointShadowVertexWgsl, web::kPointShadowFragmentWgsl,
                    web::kBloomVertexWgsl, web::kBloomFragmentWgsl);
#else
        return nullptr;
#endif
    case gfx::Backend::Null:
        return "";
    }
    return nullptr;
}

} // namespace

auto loadEffectShader(gfx::IDevice& device, EffectShader shader)
    -> Result<gfx::ShaderHandle, gfx::GfxError> {
    gfx::ShaderDesc desc;
    desc.stage = shader == EffectShader::PointShadowVertex || shader == EffectShader::BloomVertex
                     ? gfx::ShaderStage::Vertex
                     : gfx::ShaderStage::Fragment;
    if (shader == EffectShader::LightBinningCompute) desc.stage = gfx::ShaderStage::Compute;
    const gfx::Backend backend = device.backend();
    const char* source = effectSource(backend, shader);
    if (source == nullptr) return gfx::GfxError::BackendUnavailable;
    if (backend == gfx::Backend::Null) {
        desc.blob.push(std::byte{0});
    } else if (backend == gfx::Backend::WebGPU) {
        const std::size_t size = std::strlen(source);
        if (size == 0) return gfx::GfxError::BackendUnavailable;
        desc.blob.resize(size);
        std::memcpy(desc.blob.data(), source, size);
    } else {
        std::ifstream file(source, std::ios::binary | std::ios::ate);
        if (!file) return gfx::GfxError::BackendUnavailable;
        const std::streamsize size = file.tellg();
        if (size <= 0) return gfx::GfxError::BackendUnavailable;
        desc.blob.resize(static_cast<std::size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(desc.blob.data()), size)) {
            return gfx::GfxError::BackendUnavailable;
        }
    }
    return device.createShader(desc);
}

} // namespace f4::engine
