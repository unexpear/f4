#include "core/resources/gltf_loader.h"

#include <cgltf.h>
#include <cstring>

#include "core/math/vec3.h"

namespace f4::resources {

namespace {
auto mapAttributeSemantic(cgltf_attribute_type type) -> VertexSemantic {
    switch (type) {
    case cgltf_attribute_type_position:    return VertexSemantic::Position;
    case cgltf_attribute_type_normal:      return VertexSemantic::Normal;
    case cgltf_attribute_type_texcoord:    return VertexSemantic::TexCoord0;
    case cgltf_attribute_type_tangent:     return VertexSemantic::Tangent;
    case cgltf_attribute_type_color:       return VertexSemantic::Color0;
    default:                               return VertexSemantic::Position;
    }
}

auto mapComponentType(cgltf_component_type ct) -> VertexFormat {
    switch (ct) {
    case cgltf_component_type_r_32f:  return VertexFormat::F32_3;
    case cgltf_component_type_r_8u:   return VertexFormat::UNORM8_4;
    default:                          return VertexFormat::F32_3;
    }
}

// cgltf accessor component size in bytes.
auto componentBytes(cgltf_component_type ct) -> std::size_t {
    switch (ct) {
    case cgltf_component_type_r_8u:   return 1;
    case cgltf_component_type_r_16u:  return 2;
    case cgltf_component_type_r_16:   return 2;
    case cgltf_component_type_r_32u:  return 4;
    case cgltf_component_type_r_32f:  return 4;
    default:                          return 0;
    }
}

auto accessorComponentCount(cgltf_type type) -> std::size_t {
    switch (type) {
    case cgltf_type_scalar:   return 1;
    case cgltf_type_vec2:     return 2;
    case cgltf_type_vec3:     return 3;
    case cgltf_type_vec4:     return 4;
    case cgltf_type_mat2:     return 4;
    case cgltf_type_mat3:     return 9;
    case cgltf_type_mat4:     return 16;
    default:                  return 0;
    }
}

auto accessorStride(const cgltf_accessor* acc) -> std::size_t {
    return componentBytes(acc->component_type) * accessorComponentCount(acc->type);
}

auto accessorByteLength(const cgltf_accessor* acc) -> std::size_t {
    return acc->count * accessorStride(acc);
}

auto readIndex(const cgltf_accessor* acc, std::size_t i) -> std::uint32_t {
    cgltf_uint out[4] = {};
    if (cgltf_accessor_read_uint(acc, i, out, acc->type == cgltf_type_scalar ? 1 : 4)) {
        return out[0];
    }
    return 0;
}
} // anonymous namespace

auto loadGltf(std::span<const std::byte> bytes) -> Result<GltfScene, LoadError> {
    if (bytes.empty()) {
        return LoadError::Truncated;
    }

    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result res = cgltf_parse(&options,
        bytes.data(), bytes.size(), &data);
    if (res != cgltf_result_success || data == nullptr) {
        return LoadError::Truncated;
    }

    // Load embedded/buffer data. We pass nullptr for file read callbacks
    // since all buffer URIs in memory-parsed glTF are data: URIs or
    // the GLB BIN chunk.
    res = cgltf_load_buffers(&options, data, nullptr);
    if (res != cgltf_result_success) {
        cgltf_free(data);
        return LoadError::Truncated;
    }

    GltfScene scene;

    for (std::size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& gltfMesh = data->meshes[m];

        for (std::size_t p = 0; p < gltfMesh.primitives_count; ++p) {
            const cgltf_primitive& prim = gltfMesh.primitives[p];
            Mesh mesh;

            // Determine max vertex count from POSITION accessor.
            std::size_t vertexCount = 0;
            const cgltf_accessor* posAcc = nullptr;
            for (std::size_t a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) {
                    posAcc = prim.attributes[a].data;
                    vertexCount = posAcc->count;
                    break;
                }
            }

            if (vertexCount == 0 || posAcc == nullptr) {
                continue;
            }

            // Build per-attribute streams (one stream per attribute).
            for (std::size_t a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                const cgltf_accessor* acc = attr.data;

                const VertexSemantic semantic = mapAttributeSemantic(attr.type);
                const VertexFormat format = mapComponentType(acc->component_type);
                const std::size_t stride = accessorStride(acc);
                const std::size_t byteLen = accessorByteLength(acc);

                VertexStream stream;
                stream.strideBytes = static_cast<std::uint32_t>(stride);
                stream.data.resize(byteLen);

                // Copy raw accessor data.
                const std::uint8_t* src = static_cast<const std::uint8_t*>(acc->buffer_view->buffer->data);
                const std::size_t srcOffset = acc->buffer_view->offset + acc->offset;
                std::memcpy(stream.data.data(), src + srcOffset, byteLen);

                VertexAttribute vertAttr;
                vertAttr.semantic = semantic;
                vertAttr.format = format;
                vertAttr.stream = static_cast<std::uint32_t>(mesh.streams.size());
                vertAttr.offsetBytes = 0;

                mesh.streams.push(static_cast<VertexStream&&>(stream));
                mesh.attributes.push(static_cast<VertexAttribute&&>(vertAttr));
            }

            // Indices.
            if (prim.indices != nullptr) {
                const cgltf_accessor* idxAcc = prim.indices;
                const std::size_t idxCount = idxAcc->count;

                const bool useU32 = (idxAcc->component_type == cgltf_component_type_r_32u);
                mesh.indexFormat = useU32 ? IndexFormat::U32 : IndexFormat::U16;
                const std::size_t elemSize = useU32 ? 4u : 2u;
                mesh.indices.resize(idxCount * elemSize);

                for (std::size_t i = 0; i < idxCount; ++i) {
                    const std::uint32_t idx = readIndex(idxAcc, i);
                    if (useU32) {
                        const auto v = static_cast<std::uint32_t>(idx);
                        std::memcpy(mesh.indices.data() + i * elemSize, &v, 4);
                    } else {
                        const auto v = static_cast<std::uint16_t>(idx);
                        std::memcpy(mesh.indices.data() + i * elemSize, &v, 2);
                    }
                }
            }

            mesh.bounds = computeBounds(mesh);
            scene.meshes.push(static_cast<Mesh&&>(mesh));
        }
    }

    cgltf_free(data);
    return static_cast<GltfScene&&>(scene);
}

} // namespace f4::resources
