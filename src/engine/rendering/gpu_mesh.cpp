#include "engine/rendering/gpu_mesh.h"

#include <cstdint>
#include <limits>
#include <span>

#include "core/assert.h"
#include "graphics/buffer/buffer_desc.h"

namespace f4::engine {

auto translateVertexFormat(resources::VertexFormat fmt) -> gfx::VertexFormat {
    switch (fmt) {
    case resources::VertexFormat::F32_1: return gfx::VertexFormat::Float32x2; // promoted; scalar not native
    case resources::VertexFormat::F32_2: return gfx::VertexFormat::Float32x2;
    case resources::VertexFormat::F32_3: return gfx::VertexFormat::Float32x3;
    case resources::VertexFormat::F32_4: return gfx::VertexFormat::Float32x4;
    case resources::VertexFormat::UNORM8_4: return gfx::VertexFormat::UNorm8x4;
    }
    return gfx::VertexFormat::Float32x3;
}

auto semanticLocation(resources::VertexSemantic sem) -> std::uint32_t {
    switch (sem) {
    case resources::VertexSemantic::Position: return 0;
    case resources::VertexSemantic::Normal: return 1;
    case resources::VertexSemantic::TexCoord0: return 2;
    case resources::VertexSemantic::Color0: return 3;
    case resources::VertexSemantic::Tangent: return 4;
    case resources::VertexSemantic::Bitangent: return 5;
    case resources::VertexSemantic::TexCoord1: return 6;
    case resources::VertexSemantic::TexCoord2: return 7;
    case resources::VertexSemantic::TexCoord3: return 8;
    }
    return 0;
}

auto createGpuMesh(gfx::IDevice& dev, const resources::Mesh& mesh) -> Result<GpuMesh, gfx::GfxError> {
    if (mesh.streams.isEmpty()) {
        return gfx::GfxError::BadDescriptor;
    }
    if (mesh.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
        mesh.indices.size() % resources::indexFormatSize(mesh.indexFormat) != 0) {
        return gfx::GfxError::BadDescriptor;
    }
    const resources::VertexAttribute* position =
        mesh.attributeFor(resources::VertexSemantic::Position);
    if (position == nullptr || position->stream >= mesh.streams.size()) {
        return gfx::GfxError::BadDescriptor;
    }
    const std::uint64_t vertexCount = mesh.vertexCount(position->stream);
    if (vertexCount == 0 || vertexCount > std::numeric_limits<std::uint32_t>::max() ||
        mesh.indexCount() > std::numeric_limits<std::uint32_t>::max()) {
        return gfx::GfxError::BadDescriptor;
    }
    // Validate attributes reference valid streams and offsets.
    for (const auto& stream : mesh.streams) {
        if (stream.strideBytes == 0 || stream.data.isEmpty() ||
            stream.data.size() % stream.strideBytes != 0) {
            return gfx::GfxError::BadDescriptor;
        }
    }
    for (const auto& attr : mesh.attributes) {
        if (attr.stream >= mesh.streams.size()) {
            return gfx::GfxError::BadDescriptor;
        }
        const auto& stream = mesh.streams[attr.stream];
        const std::uint32_t sz = resources::vertexFormatSize(attr.format);
        if (attr.offsetBytes > stream.strideBytes ||
            sz > stream.strideBytes - attr.offsetBytes) {
            return gfx::GfxError::BadDescriptor;
        }
        if (mesh.vertexCount(attr.stream) < vertexCount) {
            return gfx::GfxError::BadDescriptor;
        }
        if (attr.format == resources::VertexFormat::F32_1) {
            // F32_1 promoted to Float32x2 — needs 8 bytes but source is 4.
            // Only allowed if stride can accommodate promotion; for now reject
            // to avoid silent widening. Caller should use F32_2.
            return gfx::GfxError::BadDescriptor;
        }
    }

    GpuMesh out;
    out.indexFormat = (mesh.indexFormat == resources::IndexFormat::U16)
                          ? gfx::IndexFormat::U16
                          : gfx::IndexFormat::U32;
    out.indexCount = static_cast<std::uint32_t>(mesh.indexCount());
    out.vertexCount = static_cast<std::uint32_t>(vertexCount);

    // Build vertex input layout from mesh attributes.
    // One binding per stream (stream index == binding index).
    for (std::uint32_t si = 0; si < mesh.streams.size(); ++si) {
        const auto& s = mesh.streams[si];
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = si;
        b.strideBytes = s.strideBytes;
        b.divisor = 0;
        out.layout.bindings.push(b);
    }
    for (const auto& attr : mesh.attributes) {
        gfx::VertexInputLayout::VertexAttribute a;
        a.binding = attr.stream;
        a.location = semanticLocation(attr.semantic);
        a.format = translateVertexFormat(attr.format);
        a.offsetBytes = attr.offsetBytes;
        out.layout.attributes.push(a);
    }

    // Create vertex buffers.
    for (const auto& s : mesh.streams) {
        if (s.data.size() > std::numeric_limits<std::uint32_t>::max()) {
            for (auto h : out.vertexBuffers) dev.destroyBuffer(h);
            return gfx::GfxError::BadDescriptor;
        }
        gfx::BufferDesc d;
        d.sizeBytes = static_cast<std::uint32_t>(s.data.size());
        d.usage = gfx::BufferUsage::Vertex;
        if (d.sizeBytes == 0) {
            return gfx::GfxError::BadDescriptor;
        }
        auto r = dev.createBuffer(d, std::span(s.data.data(), s.data.size()));
        if (!r.hasValue()) {
            for (auto h : out.vertexBuffers) dev.destroyBuffer(h);
            return r.error();
        }
        out.vertexBuffers.push(r.value());
    }

    // Create index buffer if indices present.
    if (!mesh.indices.isEmpty()) {
        gfx::BufferDesc d;
        d.sizeBytes = static_cast<std::uint32_t>(mesh.indices.size());
        d.usage = gfx::BufferUsage::Index;
        auto r = dev.createBuffer(d, std::span(mesh.indices.data(), mesh.indices.size()));
        if (!r.hasValue()) {
            for (auto h : out.vertexBuffers) dev.destroyBuffer(h);
            return r.error();
        }
        out.indexBuffer = r.value();
    }

    return out;
}

void destroyGpuMesh(gfx::IDevice& dev, GpuMesh& gpu) {
    for (auto h : gpu.vertexBuffers) {
        if (h.isValid()) dev.destroyBuffer(h);
    }
    gpu.vertexBuffers.clear();
    if (gpu.indexBuffer.isValid()) {
        dev.destroyBuffer(gpu.indexBuffer);
        gpu.indexBuffer = {};
    }
    gpu.layout.bindings.clear();
    gpu.layout.attributes.clear();
}

} // namespace f4::engine
