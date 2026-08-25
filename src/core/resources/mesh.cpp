#include "core/resources/mesh.h"

#include <cstring>

#include "core/assert.h"

namespace f4::resources {

auto computeBounds(const Mesh& mesh) -> math::AABB {
    const VertexAttribute* attr = mesh.attributeFor(VertexSemantic::Position);
    if (attr == nullptr) {
        // min > max marks "no positions".
        math::AABB empty{};
        empty.min = math::Vec3(1.0f, 1.0f, 1.0f);
        empty.max = math::Vec3(-1.0f, -1.0f, -1.0f);
        return empty;
    }

    F4_ASSERT(attr->stream < mesh.streams.size(), "attribute references missing stream");
    F4_ASSERT(attr->offsetBytes + vertexFormatSize(attr->format) <=
                  mesh.streams[attr->stream].strideBytes,
              "position attribute exceeds stream stride");

    math::AABB bounds{};
    bool first = true;
    const VertexStream& stream = mesh.streams[attr->stream];
    const std::uint64_t count = mesh.vertexCount(attr->stream);
    for (std::uint64_t i = 0; i < count; ++i) {
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        std::memcpy(xyz, stream.data.data() + (i * stream.strideBytes) + attr->offsetBytes,
                    sizeof(xyz));
        if (first) {
            bounds.min = math::Vec3(xyz[0], xyz[1], xyz[2]);
            bounds.max = bounds.min;
            first = false;
        } else {
            bounds.expand(math::Vec3(xyz[0], xyz[1], xyz[2]));
        }
    }
    return bounds;
}

} // namespace f4::resources