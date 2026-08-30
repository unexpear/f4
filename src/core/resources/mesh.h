#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/math/aabb.h"
#include "core/math/vec3.h"

namespace f4::resources {

using containers::Array;

enum class VertexSemantic : std::uint8_t {
    Position,
    Normal,
    Tangent,
    Bitangent,
    TexCoord0,
    TexCoord1,
    TexCoord2,
    TexCoord3,
    Color0,
};

// CPU-side vertex component formats (v1 set; loaders may grow this list).
enum class VertexFormat : std::uint8_t {
    F32_1,
    F32_2,
    F32_3,
    F32_4,
    UNORM8_4,
};

[[nodiscard]] constexpr auto vertexFormatSize(VertexFormat format) -> std::uint32_t {
    switch (format) {
    case VertexFormat::F32_1: return 4;
    case VertexFormat::F32_2: return 8;
    case VertexFormat::F32_3: return 12;
    case VertexFormat::F32_4: return 16;
    case VertexFormat::UNORM8_4: return 4;
    }
    return 0;
}

// One attribute of one stream: readers locate a semantic via attributeFor()
// then walk stream data in strideBytes steps, reading vertexFormatSize bytes
// at offsetBytes. Validation invariant: offset + size <= stride.
struct VertexAttribute {
    VertexSemantic semantic = VertexSemantic::Position;
    VertexFormat format = VertexFormat::F32_3;
    std::uint32_t stream = 0;
    std::uint32_t offsetBytes = 0;
};

struct VertexStream {
    std::uint32_t strideBytes = 0;
    Array<std::byte> data; // size() must be a multiple of strideBytes
};

struct SubMesh {
    std::uint64_t firstIndex = 0;
    std::uint64_t indexCount = 0;
};

enum class IndexFormat : std::uint8_t { U16, U32 };

[[nodiscard]] constexpr auto indexFormatSize(IndexFormat format) -> std::uint32_t {
    return format == IndexFormat::U16 ? 2u : 4u;
}

struct Mesh {
    Array<VertexStream> streams;
    Array<VertexAttribute> attributes;
    Array<SubMesh> submeshes;
    IndexFormat indexFormat = IndexFormat::U16;
    Array<std::byte> indices;
    math::AABB bounds{}; // computed by computeBounds at load time

    [[nodiscard]] auto vertexCount(std::uint32_t streamIndex = 0) const -> std::uint64_t {
        if (streamIndex >= streams.size()) {
            return 0;
        }
        const VertexStream& s = streams[streamIndex];
        return s.strideBytes == 0 ? 0 : s.data.size() / s.strideBytes;
    }

    [[nodiscard]] auto indexCount() const -> std::uint64_t {
        return indices.size() / indexFormatSize(indexFormat);
    }

    [[nodiscard]] auto attributeFor(VertexSemantic semantic) const -> const VertexAttribute* {
        for (const VertexAttribute& a : attributes) {
            if (a.semantic == semantic) {
                return &a;
            }
        }
        return nullptr;
    }
};

// Bounds from the Position attribute; an invalid AABB (min > max) means the
// mesh carries no positions.
[[nodiscard]] auto computeBounds(const Mesh& mesh) -> math::AABB;

// CPU-side backing-store bytes held by a mesh: every vertex stream plus the
// index buffer. (Topology/attribute metadata overhead is not counted.)
[[nodiscard]] inline auto meshBytes(const Mesh& mesh) -> std::uint64_t {
    std::uint64_t total = 0;
    for (const VertexStream& s : mesh.streams) {
        total += static_cast<std::uint64_t>(s.data.size());
    }
    total += static_cast<std::uint64_t>(mesh.indices.size());
    return total;
}

// Number of vertices actually addressed by the index buffer (vertex range used
// by geometry), distinct from the raw stream capacity.
[[nodiscard]] inline auto meshVertexBytes(const Mesh& mesh) -> std::uint64_t {
    std::uint64_t total = 0;
    for (const VertexStream& s : mesh.streams) {
        total += static_cast<std::uint64_t>(s.data.size());
    }
    return total;
}

[[nodiscard]] inline auto meshIndexBytes(const Mesh& mesh) -> std::uint64_t {
    return static_cast<std::uint64_t>(mesh.indices.size());
}

} // namespace f4::resources