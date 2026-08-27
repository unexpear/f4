#include "core/resources/usd_loader.h"

#include <cstring>
#include <vector>

#include <tinyusdz.hh>

namespace f4::resources {

namespace {

// Recursively walk the prim tree, extracting every GeomMesh into `meshes`.
void walkPrims(const tinyusdz::Prim& prim, Array<Mesh>& meshes) {
    if (prim.is<tinyusdz::GeomMesh>()) {
        const tinyusdz::GeomMesh* gm = prim.as<tinyusdz::GeomMesh>();

        const std::vector<tinyusdz::value::point3f> points = gm->get_points();
        const std::vector<int32_t> fvc = gm->get_faceVertexCounts();
        const std::vector<int32_t> fvi = gm->get_faceVertexIndices();
        const std::vector<tinyusdz::value::normal3f> normals = gm->get_normals();

        if (points.empty() || fvi.empty() || fvc.empty()) {
            for (const auto& child : prim.children()) {
                walkPrims(child, meshes);
            }
            return;
        }

        const std::size_t posCount = points.size();
        const std::size_t idxCount = fvi.size();
        const std::size_t faceCount = fvc.size();

        const bool hasNormals = normals.size() == posCount;

        std::uint32_t stride = 12; // position
        std::uint32_t offNrm = 0;
        if (hasNormals) {
            offNrm = stride;
            stride += 12;
        }

        std::vector<std::byte> vertData;
        std::vector<std::uint32_t> indices;
        vertData.reserve(idxCount * stride);
        indices.reserve(idxCount);

        auto emitTri = [&](int64_t a, int64_t b, int64_t c) {
            int64_t corners[3] = {a, b, c};
            for (int k = 0; k < 3; ++k) {
                std::uint32_t idx = static_cast<std::uint32_t>(vertData.size() / stride);
                indices.push_back(idx);
                std::size_t old = vertData.size();
                vertData.resize(old + stride);
                const tinyusdz::value::point3f& p = points[static_cast<std::size_t>(corners[k])];
                float f[3] = {p.x, p.y, p.z};
                std::memcpy(vertData.data() + old, f, 12);
                if (hasNormals) {
                    const tinyusdz::value::normal3f& n =
                        normals[static_cast<std::size_t>(corners[k])];
                    float nf[3] = {n.x, n.y, n.z};
                    std::memcpy(vertData.data() + old + offNrm, nf, 12);
                }
            }
        };

        std::size_t base = 0;
        for (std::size_t fi = 0; fi < faceCount; ++fi) {
            const std::size_t count = static_cast<std::size_t>(fvc[fi]);
            if (count < 3) {
                base += count;
                continue;
            }
            const int64_t v0 = fvi[base];
            for (std::size_t t = 1; t + 1 < count; ++t) {
                emitTri(v0, fvi[base + t], fvi[base + t + 1]);
            }
            base += count;
        }

        if (!vertData.empty() && !indices.empty()) {
            Mesh mesh;
            VertexStream stream;
            stream.strideBytes = stride;
            stream.data.resize(vertData.size());
            std::memcpy(stream.data.data(), vertData.data(), vertData.size());
            mesh.streams.push(std::move(stream));

            VertexAttribute posAttr;
            posAttr.semantic = VertexSemantic::Position;
            posAttr.format = VertexFormat::F32_3;
            posAttr.stream = 0;
            posAttr.offsetBytes = 0;
            mesh.attributes.push(posAttr);

            if (hasNormals) {
                VertexAttribute nAttr;
                nAttr.semantic = VertexSemantic::Normal;
                nAttr.format = VertexFormat::F32_3;
                nAttr.stream = 0;
                nAttr.offsetBytes = offNrm;
                mesh.attributes.push(nAttr);
            }

            mesh.indexFormat = IndexFormat::U32;
            mesh.indices.resize(indices.size() * sizeof(std::uint32_t));
            std::memcpy(mesh.indices.data(), indices.data(), mesh.indices.size());

            mesh.bounds = computeBounds(mesh);
            meshes.push(std::move(mesh));
        }
    }

    for (const auto& child : prim.children()) {
        walkPrims(child, meshes);
    }
}

} // namespace

auto loadUsd(std::span<const std::byte> bytes) -> Result<UsdScene, LoadError> {
    if (bytes.empty()) {
        return LoadError::Truncated;
    }

    tinyusdz::Stage stage;
    std::string warn;
    std::string err;

    const bool ok = tinyusdz::LoadUSDFromMemory(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size_bytes(),
        "model.usd", &stage, &warn, &err);

    if (!ok) {
        return LoadError::UnsupportedCompression;
    }

    UsdScene scene;
    for (const auto& root : stage.root_prims()) {
        walkPrims(root, scene.meshes);
    }

    return scene;
}

} // namespace f4::resources
