#pragma once

#include <span>

#include "core/containers/array.h"
#include "core/resources/mesh.h"
#include "core/result.h"
#include "graphics/buffer/buffer_desc.h"
#include "graphics/device/device.h"
#include "graphics/gfx_error.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/types.h"

namespace f4::engine {

using containers::Array;

// GPU representation of a resources::Mesh. The renderer owns this; the ECS
// MeshRenderer only holds a logical Handle<MeshTag> and never sees GPU handles.
struct GpuMesh {
    Array<gfx::BufferHandle> vertexBuffers; // one per VertexStream
    gfx::BufferHandle indexBuffer{};
    gfx::VertexInputLayout layout{};
    gfx::IndexFormat indexFormat = gfx::IndexFormat::U16;
    std::uint32_t indexCount = 0;
    std::uint32_t vertexCount = 0;
};

[[nodiscard]] auto translateVertexFormat(resources::VertexFormat fmt)
    -> gfx::VertexFormat;

[[nodiscard]] auto semanticLocation(resources::VertexSemantic sem) -> std::uint32_t;

// Creates GPU buffers for the given logical mesh and fills layout.
// Buffers are created with BufferUsage::Vertex / Index. Returns BadDescriptor
// if the mesh has an unsupported format or inconsistent stream sizes.
[[nodiscard]] auto createGpuMesh(gfx::IDevice& dev, const resources::Mesh& mesh)
    -> Result<GpuMesh, gfx::GfxError>;

// Destroys all buffers owned by the GpuMesh.
void destroyGpuMesh(gfx::IDevice& dev, GpuMesh& gpu);

} // namespace f4::engine
