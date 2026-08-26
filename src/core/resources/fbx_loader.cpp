#include "core/resources/fbx_loader.h"

#include <cstring>
#include <vector>

#include <ufbx.h>

namespace f4::resources {

auto loadFbx(std::span<const std::byte> bytes) -> Result<FbxScene, LoadError> {
    if (bytes.empty()) return LoadError::Truncated;

    ufbx_error ufbxErr{};
    ufbx_load_opts opts{};
    opts.generate_missing_normals = true;

    ufbx_scene* ufbxScene = ufbx_load_memory(
        bytes.data(), bytes.size(), &opts, &ufbxErr);

    if (!ufbxScene) {
        return LoadError::UnsupportedCompression;
    }

    FbxScene scene;

    for (std::size_t mi = 0; mi < ufbxScene->meshes.count; ++mi) {
        const ufbx_mesh* um = ufbxScene->meshes[mi];

        Mesh mesh;

        const bool hasPos = um->vertex_position.exists;
        const bool hasNrm = um->vertex_normal.exists;
        const bool hasUV  = um->vertex_uv.exists;

        std::uint32_t stride = 0;
        std::uint32_t offPos = 0, offNrm = 0, offUV = 0;
        if (hasPos) { offPos = stride; stride += 12; }
        if (hasNrm) { offNrm = stride; stride += 12; }
        if (hasUV)  { offUV  = stride; stride += 8;  }
        if (stride == 0) continue;

        // Build a flat triangle mesh by iterating faces.
        std::vector<std::uint8_t> vertData;
        std::vector<std::uint32_t> indices;

        for (std::size_t fi = 0; fi < um->num_faces; ++fi) {
            const ufbx_face& face = um->faces[fi];
            if (face.num_indices < 3) continue;

            for (std::size_t ti = 1; ti + 1 < face.num_indices; ++ti) {
                std::size_t fvi[3] = {
                    face.index_begin,
                    face.index_begin + ti,
                    face.index_begin + ti + 1,
                };

                for (std::size_t v = 0; v < 3; ++v) {
                    auto idx = static_cast<std::uint32_t>(vertData.size() / stride);
                    indices.push_back(idx);

                    std::size_t base = vertData.size();
                    vertData.resize(base + stride);

                    if (hasPos) {
                        ufbx_vec3 p = um->vertices[um->vertex_position.indices[fvi[v]]];
                        float pos[3] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
                        std::memcpy(vertData.data() + base + offPos, pos, 12);
                    }
                    if (hasNrm) {
                        ufbx_vec3 n = um->vertex_normal.values[um->vertex_normal.indices[fvi[v]]];
                        float nrm[3] = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
                        std::memcpy(vertData.data() + base + offNrm, nrm, 12);
                    }
                    if (hasUV) {
                        ufbx_vec2 t = um->vertex_uv.values[um->vertex_uv.indices[fvi[v]]];
                        float uv[2] = {static_cast<float>(t.x), static_cast<float>(t.y)};
                        std::memcpy(vertData.data() + base + offUV, uv, 8);
                    }
                }
            }
        }

        if (vertData.empty()) continue;

        VertexStream stream;
        stream.strideBytes = stride;
        stream.data.resize(vertData.size());
        std::memcpy(stream.data.data(), vertData.data(), vertData.size());
        mesh.streams.push(std::move(stream));

        if (hasPos) {
            VertexAttribute attr;
            attr.semantic = VertexSemantic::Position;
            attr.format = VertexFormat::F32_3;
            attr.stream = 0;
            attr.offsetBytes = offPos;
            mesh.attributes.push(attr);
        }
        if (hasNrm) {
            VertexAttribute attr;
            attr.semantic = VertexSemantic::Normal;
            attr.format = VertexFormat::F32_3;
            attr.stream = 0;
            attr.offsetBytes = offNrm;
            mesh.attributes.push(attr);
        }
        if (hasUV) {
            VertexAttribute attr;
            attr.semantic = VertexSemantic::TexCoord0;
            attr.format = VertexFormat::F32_2;
            attr.stream = 0;
            attr.offsetBytes = offUV;
            mesh.attributes.push(attr);
        }

        mesh.indexFormat = IndexFormat::U32;
        mesh.indices.resize(indices.size() * 4);
        std::memcpy(mesh.indices.data(), indices.data(), mesh.indices.size());

        mesh.bounds = computeBounds(mesh);
        scene.meshes.push(std::move(mesh));
    }

    ufbx_free_scene(ufbxScene);
    return scene;
}

} // namespace f4::resources
