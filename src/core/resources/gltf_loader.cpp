#include "core/resources/gltf_loader.h"

#include <cgltf.h>
#include <cstring>
#include <limits>
#include <memory>

namespace f4::resources {

namespace {
auto rangeFits(std::size_t offset, std::size_t size, std::size_t containerSize) -> bool {
    return offset <= containerSize && size <= containerSize - offset;
}

auto readSparseIndex(const std::uint8_t* data, cgltf_component_type type)
    -> std::size_t {
    switch (type) {
    case cgltf_component_type_r_8u:
        return *data;
    case cgltf_component_type_r_16u: {
        std::uint16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    case cgltf_component_type_r_32u: {
        std::uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        return kInvalidGltfIndex;
    }
}

// cgltf validation currently checks accessor ranges before checking each
// buffer view against its loaded buffer. Validate those actual ranges first.
auto preflightAccessorRanges(const cgltf_data& data) -> bool {
    for (std::size_t i = 0; i < data.buffer_views_count; ++i) {
        const cgltf_buffer_view& view = data.buffer_views[i];
        if (view.data == nullptr &&
            (view.buffer == nullptr || view.buffer->data == nullptr ||
             !rangeFits(view.offset, view.size, view.buffer->size))) {
            return false;
        }
    }

    for (std::size_t i = 0; i < data.accessors_count; ++i) {
        const cgltf_accessor& accessor = data.accessors[i];
        const std::size_t elementSize =
            cgltf_calc_size(accessor.type, accessor.component_type);
        if (accessor.count == 0 || elementSize == 0) {
            return false;
        }

        if (accessor.buffer_view != nullptr) {
            const std::size_t stride = accessor.stride;
            if (stride < elementSize || accessor.offset > accessor.buffer_view->size) {
                return false;
            }
            const std::size_t remaining = accessor.buffer_view->size - accessor.offset;
            if (remaining < elementSize ||
                accessor.count - 1 > (remaining - elementSize) / stride) {
                return false;
            }
        }

        if (!accessor.is_sparse) {
            continue;
        }

        const cgltf_accessor_sparse& sparse = accessor.sparse;
        const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
        const bool validIndexType =
            sparse.indices_component_type == cgltf_component_type_r_8u ||
            sparse.indices_component_type == cgltf_component_type_r_16u ||
            sparse.indices_component_type == cgltf_component_type_r_32u;
        if (sparse.count > accessor.count || !validIndexType || indexSize == 0 ||
            sparse.indices_buffer_view == nullptr || sparse.values_buffer_view == nullptr ||
            sparse.count > std::numeric_limits<std::size_t>::max() / indexSize ||
            sparse.count > std::numeric_limits<std::size_t>::max() / elementSize ||
            !rangeFits(sparse.indices_byte_offset, sparse.count * indexSize,
                       sparse.indices_buffer_view->size) ||
            !rangeFits(sparse.values_byte_offset, sparse.count * elementSize,
                       sparse.values_buffer_view->size)) {
            return false;
        }

        const std::uint8_t* indices =
            cgltf_buffer_view_data(sparse.indices_buffer_view) + sparse.indices_byte_offset;
        std::size_t previous = 0;
        for (std::size_t sparseIndex = 0; sparseIndex < sparse.count; ++sparseIndex) {
            const std::size_t index =
                readSparseIndex(indices + sparseIndex * indexSize,
                                sparse.indices_component_type);
            if (index >= accessor.count || (sparseIndex > 0 && index <= previous)) {
                return false;
            }
            previous = index;
        }
    }
    return true;
}

auto unpackAccessorFloats(const cgltf_accessor& accessor, Array<float>& output) -> bool {
    const std::size_t componentCount = cgltf_num_components(accessor.type);
    const std::size_t floatCount = accessor.count * componentCount;

    cgltf_accessor base = accessor;
    base.is_sparse = 0;
    if (cgltf_accessor_unpack_floats(&base, output.data(), floatCount) != floatCount) {
        return false;
    }

    if (!accessor.is_sparse) {
        return true;
    }

    const cgltf_accessor_sparse& sparse = accessor.sparse;
    const std::size_t sparseFloatCount = sparse.count * componentCount;
    Array<float> sparseValues(sparseFloatCount);
    sparseValues.resize(sparseFloatCount);

    cgltf_accessor values = accessor;
    values.buffer_view = sparse.values_buffer_view;
    values.offset = sparse.values_byte_offset;
    values.count = sparse.count;
    values.stride = cgltf_calc_size(accessor.type, accessor.component_type);
    values.is_sparse = 0;
    if (cgltf_accessor_unpack_floats(&values, sparseValues.data(), sparseFloatCount) !=
        sparseFloatCount) {
        return false;
    }

    const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
    const std::uint8_t* indices =
        cgltf_buffer_view_data(sparse.indices_buffer_view) + sparse.indices_byte_offset;
    for (std::size_t i = 0; i < sparse.count; ++i) {
        const std::size_t destination =
            readSparseIndex(indices + i * indexSize, sparse.indices_component_type);
        std::memcpy(output.data() + destination * componentCount,
                    sparseValues.data() + i * componentCount,
                    componentCount * sizeof(float));
    }
    return true;
}

auto mapAttributeSemantic(const cgltf_attribute& attribute, VertexSemantic& semantic) -> bool {
    switch (attribute.type) {
    case cgltf_attribute_type_position:
        semantic = VertexSemantic::Position;
        return attribute.index == 0;
    case cgltf_attribute_type_normal:
        semantic = VertexSemantic::Normal;
        return attribute.index == 0;
    case cgltf_attribute_type_tangent:
        semantic = VertexSemantic::Tangent;
        return attribute.index == 0;
    case cgltf_attribute_type_color:
        semantic = VertexSemantic::Color0;
        return attribute.index == 0;
    case cgltf_attribute_type_texcoord:
        switch (attribute.index) {
        case 0: semantic = VertexSemantic::TexCoord0; return true;
        case 1: semantic = VertexSemantic::TexCoord1; return true;
        case 2: semantic = VertexSemantic::TexCoord2; return true;
        case 3: semantic = VertexSemantic::TexCoord3; return true;
        default: return false;
        }
    default:
        return false;
    }
}

auto mapAccessorFormat(cgltf_type type, VertexFormat& format) -> bool {
    switch (type) {
    case cgltf_type_scalar: format = VertexFormat::F32_1; return true;
    case cgltf_type_vec2:   format = VertexFormat::F32_2; return true;
    case cgltf_type_vec3:   format = VertexFormat::F32_3; return true;
    case cgltf_type_vec4:   format = VertexFormat::F32_4; return true;
    default:                return false;
    }
}

auto mapAlphaMode(cgltf_alpha_mode mode) -> GltfAlphaMode {
    switch (mode) {
    case cgltf_alpha_mode_mask:  return GltfAlphaMode::Mask;
    case cgltf_alpha_mode_blend: return GltfAlphaMode::Blend;
    default:                     return GltfAlphaMode::Opaque;
    }
}

auto copyTextureRef(const cgltf_data& data, const cgltf_texture_view& view)
    -> GltfTextureRef {
    GltfTextureRef result;
    if (view.texture == nullptr) {
        return result;
    }
    result.textureIndex = static_cast<std::uint32_t>(cgltf_texture_index(&data, view.texture));
    result.texCoord = static_cast<std::uint32_t>(view.texcoord);
    result.scale = view.scale;
    return result;
}

auto copyMaterial(const cgltf_data& data, const cgltf_material& source) -> GltfMaterial {
    GltfMaterial result;
    if (source.name != nullptr) {
        result.name = source.name;
    }

    const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
    result.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                              pbr.base_color_factor[2], pbr.base_color_factor[3]};
    result.metallicFactor = pbr.metallic_factor;
    result.roughnessFactor = pbr.roughness_factor;
    result.emissiveFactor = {source.emissive_factor[0], source.emissive_factor[1],
                             source.emissive_factor[2]};
    result.baseColorTexture = copyTextureRef(data, pbr.base_color_texture);
    result.metallicRoughnessTexture = copyTextureRef(data, pbr.metallic_roughness_texture);
    result.normalTexture = copyTextureRef(data, source.normal_texture);
    result.occlusionTexture = copyTextureRef(data, source.occlusion_texture);
    result.emissiveTexture = copyTextureRef(data, source.emissive_texture);
    result.alphaMode = mapAlphaMode(source.alpha_mode);
    result.alphaCutoff = source.alpha_cutoff;
    result.doubleSided = source.double_sided != 0;
    result.unlit = source.unlit != 0;
    return result;
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

    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> dataOwner(data, &cgltf_free);

    // Load embedded/buffer data. We pass nullptr for file read callbacks
    // since all buffer URIs in memory-parsed glTF are data: URIs or
    // the GLB BIN chunk.
    res = cgltf_load_buffers(&options, data, nullptr);
    if (res != cgltf_result_success) {
        return LoadError::Truncated;
    }

    if (!preflightAccessorRanges(*data) || cgltf_validate(data) != cgltf_result_success) {
        return LoadError::Truncated;
    }

    if (data->materials_count >= kInvalidGltfIndex ||
        data->textures_count >= kInvalidGltfIndex) {
        return LoadError::BadDimensions;
    }

    GltfScene scene;
    scene.materials.reserve(data->materials_count);
    for (std::size_t i = 0; i < data->materials_count; ++i) {
        scene.materials.push(copyMaterial(*data, data->materials[i]));
    }

    for (std::size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& gltfMesh = data->meshes[m];

        for (std::size_t p = 0; p < gltfMesh.primitives_count; ++p) {
            const cgltf_primitive& prim = gltfMesh.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;
            }
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

                VertexSemantic semantic{};
                VertexFormat format{};
                if (!mapAttributeSemantic(attr, semantic) ||
                    !mapAccessorFormat(acc->type, format)) {
                    continue;
                }

                const std::size_t componentCount = cgltf_num_components(acc->type);
                if (acc->count > std::numeric_limits<std::size_t>::max() / componentCount) {
                    return LoadError::BadDimensions;
                }
                const std::size_t floatCount = acc->count * componentCount;
                if (floatCount > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
                    return LoadError::BadDimensions;
                }

                VertexStream stream;
                stream.strideBytes = static_cast<std::uint32_t>(componentCount * sizeof(float));
                stream.data.resize(floatCount * sizeof(float));
                Array<float> unpacked(floatCount);
                unpacked.resize(floatCount);
                if (!unpackAccessorFloats(*acc, unpacked)) {
                    return LoadError::Truncated;
                }
                std::memcpy(stream.data.data(), unpacked.data(), stream.data.size());

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
                if (idxCount > std::numeric_limits<std::size_t>::max() / elemSize) {
                    return LoadError::BadDimensions;
                }
                mesh.indices.resize(idxCount * elemSize);

                for (std::size_t i = 0; i < idxCount; ++i) {
                    const std::size_t sourceIndex = cgltf_accessor_read_index(idxAcc, i);
                    if (sourceIndex > std::numeric_limits<std::uint32_t>::max()) {
                        return LoadError::BadDimensions;
                    }
                    const auto idx = static_cast<std::uint32_t>(sourceIndex);
                    if (useU32) {
                        std::memcpy(mesh.indices.data() + i * elemSize, &idx, 4);
                    } else {
                        const auto v = static_cast<std::uint16_t>(idx);
                        std::memcpy(mesh.indices.data() + i * elemSize, &v, 2);
                    }
                }
            } else {
                if (vertexCount > std::numeric_limits<std::uint32_t>::max()) {
                    return LoadError::BadDimensions;
                }
                const bool useU32 = vertexCount >
                    static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1u;
                mesh.indexFormat = useU32 ? IndexFormat::U32 : IndexFormat::U16;
                const std::size_t elemSize = useU32 ? 4u : 2u;
                if (vertexCount > std::numeric_limits<std::size_t>::max() / elemSize) {
                    return LoadError::BadDimensions;
                }
                mesh.indices.resize(vertexCount * elemSize);
                for (std::size_t i = 0; i < vertexCount; ++i) {
                    if (useU32) {
                        const auto index = static_cast<std::uint32_t>(i);
                        std::memcpy(mesh.indices.data() + i * elemSize, &index, 4);
                    } else {
                        const auto index = static_cast<std::uint16_t>(i);
                        std::memcpy(mesh.indices.data() + i * elemSize, &index, 2);
                    }
                }
            }

            mesh.submeshes.push({0, mesh.indexCount()});
            mesh.bounds = computeBounds(mesh);
            scene.meshes.push(static_cast<Mesh&&>(mesh));
            scene.meshMaterialIndices.push(
                prim.material == nullptr
                    ? kInvalidGltfIndex
                    : static_cast<std::uint32_t>(cgltf_material_index(data, prim.material)));
        }
    }

    return static_cast<GltfScene&&>(scene);
}

} // namespace f4::resources
