#pragma once

#include <cstdint>

#include "core/containers/array.h"

#include "graphics/handle.h"
#include "graphics/types.h"
#include "graphics/shader/shader_desc.h"

namespace f4::gfx {

using containers::Array;

// ---------------------------------------------------------------------------
// Binding model — the API-divergence-critical abstraction.
//
// One binding model abstracts each backend mechanism:
//   - OpenGL   : uniform blocks + sampler uniforms (M7)
//   - Vulkan   : descriptor sets + set layouts   (M9)
//   - DirectX12: descriptor tables + heaps       (M10)
//   - Metal    : stage resource-index namespaces (M17)
//   - WebGPU   : bind groups + bind-group layouts (M18)
//
// A PipelineLayout is an ordered list of bind groups (bind points). Each group
// has a set of typed slots. The semantics (which bound resource goes at which
// bind point/slot) are backend-neutral; M9/M10 map groups to sets/tables.
// ---------------------------------------------------------------------------

enum class BindingType : std::uint8_t {
    UniformBuffer,
    Texture,
    Sampler,
    // Read/write in compute pipelines; read-only in current graphics pipelines.
    StorageBuffer,
};

// Backends with one descriptor-binding namespace use this stable flattening of
// the RHI's per-type slot namespaces. Shader build rules use the same mapping.
[[nodiscard]] constexpr auto bindingIndex(BindingType type, std::uint32_t slot)
    -> std::uint32_t {
    return slot * 4u + static_cast<std::uint32_t>(type);
}

struct BindingSlot {
    std::uint32_t slot = 0;
    BindingType type = BindingType::UniformBuffer;
    std::uint32_t stageMask = 0; // OR of shaderStageBit() — 0 means all stages
    std::uint32_t count = 1;     // reserved for arrays; current backends require 1
};

struct BindGroup {
    std::uint32_t bindPoint = 0;
    Array<BindingSlot> slots;
};

// Whole-pipeline layout. Push-constant uploads are not yet command-buffer
// operations, so real backends reject a nonzero size until that API lands.
struct PipelineLayoutDesc {
    Array<BindGroup> groups;
    std::uint32_t pushConstantSizeBytes = 0;
};

enum class VertexFormat : std::uint8_t {
    Float32x2,
    Float32x3,
    Float32x4,
    UNorm8x4,
};

[[nodiscard]] constexpr auto vertexFormatSize(VertexFormat format) -> std::uint32_t {
    switch (format) {
    case VertexFormat::Float32x2: return 8;
    case VertexFormat::Float32x3: return 12;
    case VertexFormat::Float32x4: return 16;
    case VertexFormat::UNorm8x4: return 4;
    }
    return 0;
}

// Vertex input expressed as the M5 Mesh records are laid out. `attributes`
// parallel the asset layer's VertexAttribute list but are resolved to explicit
// bindings/locations here so all backends agree.
struct VertexInputLayout {
    struct VertexBinding {
        std::uint32_t binding = 0; // vertex buffer slot
        std::uint32_t strideBytes = 0;
        // Instance step rate: 0 = per-vertex (default), 1 = advance once per
        // instance. Bindings with divisor >= 1 carry per-instance data for
        // hardware instancing (glVertexBindingDivisor / equivalent).
        std::uint32_t divisor = 0;
    };
    struct VertexAttribute {
        std::uint32_t binding = 0; // which VertexBinding
        std::uint32_t location = 0;
        VertexFormat format = VertexFormat::Float32x3;
        std::uint32_t offsetBytes = 0;
    };
    Array<VertexBinding> bindings;
    Array<VertexAttribute> attributes;
};

struct DepthStencilState {
    bool depthTestEnable = false;
    bool depthWriteEnable = false;
    CompareOp depthCompare = CompareOp::Less;
    TextureFormat format = TextureFormat::Depth32F;
    // Declare pass attachment compatibility even when testing and writes are disabled.
    bool depthAttachmentEnable = false;

    [[nodiscard]] constexpr auto usesAttachment() const -> bool {
        return depthAttachmentEnable || depthTestEnable || depthWriteEnable;
    }
};

struct RasterState {
    CullMode cullMode = CullMode::Back;
    bool depthClampEnable = false;
};

struct ColorAttachmentState {
    bool blendEnable = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    std::uint32_t writeMask = ColorWriteMask::All;
    TextureFormat format = TextureFormat::RGBA8_UNORM;
};

struct PipelineStateDesc {
    PipelineLayoutDesc layout;
    VertexInputLayout vertexInput;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    // WebGPU requires indexed strip pipelines to pin the primitive-restart format.
    IndexFormat stripIndexFormat = IndexFormat::U16;
    ShaderHandle vertexShader{};
    ShaderHandle fragmentShader{};
    // A pipeline is either graphics (vertex + fragment) or compute, never both.
    ShaderHandle computeShader{};
    RasterState raster;
    DepthStencilState depthStencil;
    Array<ColorAttachmentState> colorAttachments; // empty -> single default
};

struct GpuPipeline {
    PipelineStateDesc desc;
};

} // namespace f4::gfx
