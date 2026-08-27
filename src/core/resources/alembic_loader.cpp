#include "core/resources/alembic_loader.h"

#include <cstring>
#include <istream>
#include <streambuf>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4457) // parameter shadows member
#pragma warning(disable : 4458) // parameter shadows member
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/Util/Exception.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace f4::resources {

namespace {

// Seekable std::streambuf backed directly by a std::span<const std::byte>.
// Avoids copying the archive buffer. Implements seekoff/seekpos/xsgetn to
// satisfy Alembic's Ogawa reader, which uses seekg/read on the stream.
class SpanStreamBuf final : public std::streambuf {
public:
    explicit SpanStreamBuf(std::span<const std::byte> bytes)
        : data_(bytes) {
        char* p = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        setg(p, p, p + bytes.size());
    }

protected:
    auto seekoff(std::streamoff off, std::ios_base::seekdir way,
                 std::ios_base::openmode which) -> std::streampos override {
        if (!(which & std::ios_base::in)) {
            return std::streampos(-1);
        }
        char* base = const_cast<char*>(reinterpret_cast<const char*>(data_.data()));
        char* end = base + static_cast<std::streamoff>(data_.size());
        char* anchor = base;
        switch (way) {
        case std::ios_base::beg:
            anchor = base + off;
            break;
        case std::ios_base::cur:
            anchor = gptr() + off;
            break;
        case std::ios_base::end:
            anchor = end + off;
            break;
        default:
            return std::streampos(-1);
        }
        if (anchor < base || anchor > end) {
            return std::streampos(-1);
        }
        setg(base, anchor, end);
        return std::streampos(anchor - base);
    }

    auto seekpos(std::streampos pos, std::ios_base::openmode which) -> std::streampos override {
        if (!(which & std::ios_base::in)) {
            return std::streampos(-1);
        }
        auto off = static_cast<std::streamoff>(pos);
        char* base = const_cast<char*>(reinterpret_cast<const char*>(data_.data()));
        char* end = base + static_cast<std::streamoff>(data_.size());
        char* anchor = base + off;
        if (anchor < base || anchor > end) {
            return std::streampos(-1);
        }
        setg(base, anchor, end);
        return pos;
    }

private:
    std::span<const std::byte> data_;
};

// Emit the three interleaved vertices for one triangle of a fan-triangulated
// convex polygon. `src` provides per-corner source data via accessor lambdas.
// Appends 3 * stride bytes to `out` and 3 indices to `outIndices`.
template <typename PosAt>
void emitTriangle(int64_t a, int64_t b, int64_t c,
                  const PosAt& posAt, std::uint32_t stride, std::uint32_t offNrm,
                  std::uint32_t offUV, bool hasNormals, bool hasUVs,
                  const std::vector<Imath::V3f>* normals,
                  const std::vector<Imath::V2f>* uvs,
                  std::vector<std::byte>& out, std::vector<std::uint32_t>& outIndices) {
    int64_t corners[3] = {a, b, c};
    for (int k = 0; k < 3; ++k) {
        std::uint32_t idx = static_cast<std::uint32_t>(out.size() / stride);
        outIndices.push_back(idx);
        std::size_t old = out.size();
        out.resize(old + stride);
        const Imath::V3f& p = posAt(corners[k]);
        float f[3] = {p.x, p.y, p.z};
        std::memcpy(out.data() + old, f, 12);
        if (hasNormals && !normals->empty()) {
            std::size_t nIdx = static_cast<std::size_t>(corners[k]) % normals->size();
            const Imath::V3f& n = (*normals)[nIdx];
            float nf[3] = {n.x, n.y, n.z};
            std::memcpy(out.data() + old + offNrm, nf, 12);
        }
        if (hasUVs && !uvs->empty()) {
            std::size_t tIdx = static_cast<std::size_t>(corners[k]) % uvs->size();
            const Imath::V2f& t = (*uvs)[tIdx];
            float tf[2] = {t.x, t.y};
            std::memcpy(out.data() + old + offUV, tf, 8);
        }
    }
}

} // namespace

auto loadAlembic(std::span<const std::byte> bytes) -> Result<AlembicScene, LoadError> {
    if (bytes.empty()) {
        return LoadError::Truncated;
    }

    AlembicScene scene;

    try {
        SpanStreamBuf buf(bytes);
        std::istream stream(&buf);

        Alembic::AbcCoreFactory::IFactory factory;
        Alembic::AbcCoreFactory::IFactory::CoreType coreType;
        Alembic::Abc::IArchive archive = factory.getArchive({&stream}, coreType);

        if (!archive.valid()) {
            return LoadError::BadSignature;
        }

        Alembic::Abc::IObject top = archive.getTop();
        const std::size_t childCount = top.getNumChildren();

        for (std::size_t ci = 0; ci < childCount; ++ci) {
            Alembic::Abc::IObject child = top.getChild(ci);
            if (!child.valid()) {
                continue;
            }

            if (child.getMetaData().get("schema") != "AbcGeom_PolyMesh_v1") {
                continue;
            }

            Alembic::AbcGeom::IPolyMesh mesh(child);
            if (!mesh.valid()) {
                continue;
            }

            Alembic::AbcGeom::IPolyMeshSchema::Sample sample;
            mesh.getSchema().get(sample);

            Alembic::Abc::P3fArraySamplePtr positions = sample.getPositions();
            Alembic::Abc::Int32ArraySamplePtr faceIndices = sample.getFaceIndices();
            Alembic::Abc::Int32ArraySamplePtr faceCounts = sample.getFaceCounts();

            if (!positions || !faceIndices || !faceCounts) {
                continue;
            }

            const std::size_t posCount = positions->size();
            const std::size_t idxCount = faceIndices->size();
            if (posCount == 0 || idxCount == 0) {
                continue;
            }

            const Imath::V3f* posData = positions->get();

            const bool hasNormals = mesh.getSchema().getNormalsParam().valid();
            const bool hasUVs = mesh.getSchema().getUVsParam().valid();

            std::uint32_t stride = 12; // position
            std::uint32_t offNrm = 0, offUV = 0;
            if (hasNormals) {
                offNrm = stride;
                stride += 12;
            }
            if (hasUVs) {
                offUV = stride;
                stride += 8;
            }

            std::vector<Imath::V3f> normalsData;
            std::vector<Imath::V2f> uvsData;
            if (hasNormals) {
                Alembic::AbcGeom::IN3fGeomParam param = mesh.getSchema().getNormalsParam();
                Alembic::AbcGeom::IN3fGeomParam::Sample vs = param.getExpandedValue();
                if (vs.getVals()) {
                    const Imath::V3f* src = vs.getVals()->get();
                    normalsData.assign(src, src + vs.getVals()->size());
                }
            }
            if (hasUVs) {
                Alembic::AbcGeom::IV2fGeomParam param = mesh.getSchema().getUVsParam();
                Alembic::AbcGeom::IV2fGeomParam::Sample us = param.getExpandedValue();
                if (us.getVals()) {
                    const Imath::V2f* src = us.getVals()->get();
                    uvsData.assign(src, src + us.getVals()->size());
                }
            }

            const int32_t* counts = faceCounts->get();
            const int32_t* inds = faceIndices->get();
            const std::size_t faceCount = faceCounts->size();

            // Verify index range up front.
            for (std::size_t i = 0; i < idxCount; ++i) {
                if (inds[i] < 0 || static_cast<std::size_t>(inds[i]) >= posCount) {
                    return LoadError::BadSignature;
                }
            }

            Mesh outMesh;
            std::vector<std::byte> vertData;
            std::vector<std::uint32_t> indices;
            vertData.reserve(idxCount * stride);
            indices.reserve(idxCount);

            auto posAt = [&](int64_t src) -> const Imath::V3f& {
                return posData[src];
            };

            std::size_t base = 0;
            for (std::size_t fi = 0; fi < faceCount; ++fi) {
                const std::size_t count = static_cast<std::size_t>(counts[fi]);
                if (count < 3) {
                    base += count;
                    continue;
                }
                // Fan triangulation: (v0,v1,v2), (v0,v2,v3), ...
                const int64_t v0 = inds[base];
                for (std::size_t t = 1; t + 1 < count; ++t) {
                    const int64_t vi = inds[base + t];
                    const int64_t vj = inds[base + t + 1];
                    emitTriangle(v0, vi, vj, posAt, stride, offNrm, offUV,
                                 hasNormals, hasUVs, &normalsData, &uvsData,
                                 vertData, indices);
                }
                base += count;
            }

            if (vertData.empty() || indices.empty()) {
                continue;
            }

            VertexStream vstream;
            vstream.strideBytes = stride;
            vstream.data.resize(vertData.size());
            std::memcpy(vstream.data.data(), vertData.data(), vertData.size());
            outMesh.streams.push(std::move(vstream));

            VertexAttribute posAttr;
            posAttr.semantic = VertexSemantic::Position;
            posAttr.format = VertexFormat::F32_3;
            posAttr.stream = 0;
            posAttr.offsetBytes = 0;
            outMesh.attributes.push(posAttr);

            if (hasNormals) {
                VertexAttribute nAttr;
                nAttr.semantic = VertexSemantic::Normal;
                nAttr.format = VertexFormat::F32_3;
                nAttr.stream = 0;
                nAttr.offsetBytes = offNrm;
                outMesh.attributes.push(nAttr);
            }
            if (hasUVs) {
                VertexAttribute uvAttr;
                uvAttr.semantic = VertexSemantic::TexCoord0;
                uvAttr.format = VertexFormat::F32_2;
                uvAttr.stream = 0;
                uvAttr.offsetBytes = offUV;
                outMesh.attributes.push(uvAttr);
            }

            outMesh.indexFormat = IndexFormat::U32;
            outMesh.indices.resize(indices.size() * sizeof(std::uint32_t));
            std::memcpy(outMesh.indices.data(), indices.data(), outMesh.indices.size());

            outMesh.bounds = computeBounds(outMesh);
            scene.meshes.push(std::move(outMesh));
        }
    } catch (const Alembic::Util::Exception&) {
        return LoadError::UnsupportedCompression;
    } catch (const std::exception&) {
        return LoadError::BadSignature;
    }

    return scene;
}

} // namespace f4::resources
