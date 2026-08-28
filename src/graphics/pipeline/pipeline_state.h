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
// One binding model abstracts the three native mechanisms:
//   - OpenGL   : uniform blocks + sampler uniforms (M7)
//   - Vulkan   : descriptor sets + set layouts   (M9)
//   - DirectX12: descriptor tables + heaps       (M10)
//
// A PipelineLayout is an ordered list of bind groups (bind points). Each group
// has a set of typed slots. The semantics (which bound resource goes at which
// bind point/slot) are backend-neutral; M9/M10 map groups to sets/tables.
// ---------------------------------------------------------------------------

enum class BindingType : std::uint8_t {
    UniformBuffer,
    Texture,
    Sampler,
    StorageBuffer,
};

struct BindingSlot {
    std::uint32_t slot = 0;
    BindingType type = BindingType::UniformBuffer;
    std::uint32_t stageMask = 0; // OR of shaderStageBit() — 0 means all stages
    std::uint32_t count = 1;     // array of `count` resources
};

struct BindGroup {
    std::uint32_t bindPoint = 0;
    Array<BindingSlot> slots;
};

// Whole-pipeline layout: the ordered bind groups plus optional push-constant
// size hint (ignored by the null device, meaningful to Vulkan/DX12 later).
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
// bindings/locations here so all three backends agree.
struct VertexInputLayout {
    struct VertexBinding {
        std::uint32_t binding = 0; // vertex buffer slot
        std::uint32_t strideBytes = 0;
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
};

struct PipelineStateDesc {
    PipelineLayoutDesc layout;
    VertexInputLayout vertexInput;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    ShaderHandle vertexShader{};
    ShaderHandle fragmentShader{};
    RasterState raster;
    DepthStencilState depthStencil;
    Array<ColorAttachmentState> colorAttachments; // empty -> single default
};

struct GpuPipeline {
    PipelineStateDesc desc;
};

} // namespace f4::gfx
