#include "core/resources/obj_loader.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::resources {

// ── Line iterator ───────────────────────────────────────────────────────────

class LineReader {
public:
    explicit LineReader(std::span<const std::byte> data)
        : m_begin(reinterpret_cast<const char*>(data.data()))
        , m_end(reinterpret_cast<const char*>(data.data() + data.size()))
        , m_cur(m_begin) {}

    [[nodiscard]] auto next() -> bool {
        if (m_cur >= m_end) return false;
        m_lineStart = m_cur;
        while (m_cur < m_end && *m_cur != '\n') ++m_cur;
        m_lineEnd = m_cur;
        if (m_cur < m_end) ++m_cur; // skip \n
        // Trim \r
        while (m_lineEnd > m_lineStart && *(m_lineEnd - 1) == '\r') --m_lineEnd;
        return true;
    }

    [[nodiscard]] auto line() const -> std::string_view {
        return {m_lineStart, static_cast<std::size_t>(m_lineEnd - m_lineStart)};
    }

private:
    const char* m_begin;
    const char* m_end;
    const char* m_cur;
    const char* m_lineStart = nullptr;
    const char* m_lineEnd = nullptr;
};

// ── Skip whitespace, return pointer to first non-space ──────────────────────

static auto skipws(const char* p, const char* end) -> const char* {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

// ── Parse one float from a line, advance pointer ────────────────────────────

static auto parseFloat(const char*& p, const char* end) -> float {
    p = skipws(p, end);
    const char* start = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '/' && *p != '\r') ++p;
    float val = 0.0f;
    std::from_chars(start, p, val);
    return val;
}

// ── Parse one int from a line, advance pointer ──────────────────────────────

static auto parseInt(const char*& p, const char* end) -> int {
    p = skipws(p, end);
    const char* start = p;
    bool neg = false;
    if (p < end && *p == '-') { neg = true; ++p; }
    while (p < end && *p >= '0' && *p <= '9') ++p;
    int val = 0;
    std::from_chars(neg ? start + 1 : start, p, val);
    return neg ? -val : val;
}

// ── Skip to next non-whitespace after expected delimiter ────────────────────

    // ── Face vertex index triplet ───────────────────────────────────────────

struct FaceVert {
    int vi = 0; // position index (1-based, 0 = missing)
    int ti = 0; // texcoord index
    int ni = 0; // normal index
};

// ── Parse a single face vertex: v, v/vt, v/vt/vn, v//vn ────────────────────

static auto parseFaceVert(const char*& p, const char* end) -> FaceVert {
    FaceVert fv;
    p = skipws(p, end);

    // v
    fv.vi = parseInt(p, end);
    if (p >= end || *p != '/') {
        return fv;
    }
    ++p; // skip /

    // Check for v//vn
    if (p < end && *p == '/') {
        ++p;
        fv.ni = parseInt(p, end);
        return fv;
    }

    // v/vt...
    fv.ti = parseInt(p, end);
    if (p >= end || *p != '/') {
        return fv;
    }
    ++p; // skip /
    fv.ni = parseInt(p, end);
    return fv;
}

// ── Dedup key ───────────────────────────────────────────────────────────────

struct VertKey {
    int vi, ti, ni;
    bool operator==(const VertKey& o) const {
        return vi == o.vi && ti == o.ti && ni == o.ni;
    }
};

struct VertKeyHash {
    auto operator()(const VertKey& k) const -> std::size_t {
        auto h = std::hash<int>{}(k.vi);
        h ^= std::hash<int>{}(k.ti) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.ni) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ── Mesh builder: accumulates vertices/indices for one group ────────────────

struct MeshBuilder {
    bool hasPositions = false;
    bool hasTexcoords = false;
    bool hasNormals = false;

    // Raw pools from the file
    std::vector<float> positions; // x,y,z triples
    std::vector<float> texcoords; // u,v pairs
    std::vector<float> normals;   // x,y,z triples

    // Deduplicated output
    std::vector<float> outPositions;
    std::vector<float> outTexcoords;
    std::vector<float> outNormals;
    std::vector<std::uint32_t> outIndices;
    std::unordered_map<VertKey, std::uint32_t, VertKeyHash> vertMap;

    auto addVertex(const FaceVert& fv) -> std::uint32_t {
        // Resolve 1-based / negative indices
        auto resolvePos = [&](int idx) -> int {
            if (idx == 0) return -1;
            if (idx < 0) {
                int pool = static_cast<int>(positions.size() / 3);
                return pool + idx; // negative indices count from end
            }
            return idx - 1; // convert to 0-based
        };
        auto resolveTex = [&](int idx) -> int {
            if (idx == 0) return -1;
            if (idx < 0) {
                int pool = static_cast<int>(texcoords.size() / 2);
                return pool + idx;
            }
            return idx - 1;
        };
        auto resolveNorm = [&](int idx) -> int {
            if (idx == 0) return -1;
            if (idx < 0) {
                int pool = static_cast<int>(normals.size() / 3);
                return pool + idx;
            }
            return idx - 1;
        };

        int pi = resolvePos(fv.vi);
        int ti = resolveTex(fv.ti);
        int ni = resolveNorm(fv.ni);

        VertKey key{pi, ti, ni};
        auto it = vertMap.find(key);
        if (it != vertMap.end()) return it->second;

        auto idx = static_cast<std::uint32_t>(outPositions.size() / 3);

        if (pi >= 0 && (pi + 1) * 3 <= static_cast<int>(positions.size())) {
            outPositions.push_back(positions[pi * 3 + 0]);
            outPositions.push_back(positions[pi * 3 + 1]);
            outPositions.push_back(positions[pi * 3 + 2]);
            hasPositions = true;
        } else {
            outPositions.insert(outPositions.end(), {0.0f, 0.0f, 0.0f});
        }

        if (ti >= 0 && (ti + 1) * 2 <= static_cast<int>(texcoords.size())) {
            outTexcoords.push_back(texcoords[ti * 2 + 0]);
            outTexcoords.push_back(texcoords[ti * 2 + 1]);
            hasTexcoords = true;
        } else {
            outTexcoords.insert(outTexcoords.end(), {0.0f, 0.0f});
        }

        if (ni >= 0 && (ni + 1) * 3 <= static_cast<int>(normals.size())) {
            outNormals.push_back(normals[ni * 3 + 0]);
            outNormals.push_back(normals[ni * 3 + 1]);
            outNormals.push_back(normals[ni * 3 + 2]);
            hasNormals = true;
        } else {
            outNormals.insert(outNormals.end(), {0.0f, 0.0f, 1.0f});
        }

        vertMap[key] = idx;
        return idx;
    }

    auto buildMesh() -> Mesh {
        Mesh mesh;

        if (outPositions.empty()) return mesh;

        const auto vertCount = static_cast<std::uint32_t>(outPositions.size() / 3);

        // Pack into a single interleaved stream: pos(12) + norm(12) + uv(8) = 32 bytes
        // Only include attributes that have data.
        struct AttrDesc { VertexSemantic sem; VertexFormat fmt; std::uint32_t offset; };

        std::vector<AttrDesc> attrs;
        std::uint32_t stride = 0;

        if (hasPositions) {
            attrs.push_back({VertexSemantic::Position, VertexFormat::F32_3, stride});
            stride += 12;
        }
        if (hasNormals) {
            attrs.push_back({VertexSemantic::Normal, VertexFormat::F32_3, stride});
            stride += 12;
        }
        if (hasTexcoords) {
            attrs.push_back({VertexSemantic::TexCoord0, VertexFormat::F32_2, stride});
            stride += 8;
        }

        VertexStream stream;
        stream.strideBytes = stride;
        stream.data.resize(static_cast<std::size_t>(vertCount) * stride);

        for (std::uint32_t i = 0; i < vertCount; ++i) {
            auto* dst = reinterpret_cast<std::uint8_t*>(stream.data.data()) + i * stride;
            for (auto& a : attrs) {
                if (a.sem == VertexSemantic::Position) {
                    std::memcpy(dst + a.offset, &outPositions[i * 3], 12);
                } else if (a.sem == VertexSemantic::Normal) {
                    std::memcpy(dst + a.offset, &outNormals[i * 3], 12);
                } else if (a.sem == VertexSemantic::TexCoord0) {
                    std::memcpy(dst + a.offset, &outTexcoords[i * 2], 8);
                }
            }
        }

        mesh.streams.push(std::move(stream));
        for (auto& a : attrs) {
            VertexAttribute attr;
            attr.semantic = a.sem;
            attr.format = a.fmt;
            attr.stream = 0;
            attr.offsetBytes = a.offset;
            mesh.attributes.push(std::move(attr));
        }

        // Indices
        mesh.indexFormat = IndexFormat::U32;
        mesh.indices.resize(outIndices.size() * 4);
        std::memcpy(mesh.indices.data(), outIndices.data(), outIndices.size() * 4);

        // Bounds
        mesh.bounds = computeBounds(mesh);
        return mesh;
    }
};

// ── Public entry point ──────────────────────────────────────────────────────

auto loadObj(std::span<const std::byte> bytes) -> Result<ObjScene, LoadError> {
    if (bytes.empty()) return LoadError::Truncated;

    ObjScene scene;
    MeshBuilder builder;
    bool hasFaces = false;

    LineReader reader(bytes);
    while (reader.next()) {
        auto line = reader.line();
        if (line.empty() || line[0] == '#') continue;

        const char* p = line.data();
        const char* e = line.data() + line.size();

        if (line[0] == 'v' && line.size() > 1 && line[1] == ' ') {
            p += 2;
            float x = parseFloat(p, e);
            float y = parseFloat(p, e);
            float z = parseFloat(p, e);
            builder.positions.push_back(x);
            builder.positions.push_back(y);
            builder.positions.push_back(z);
        } else if (line[0] == 'v' && line[1] == 't' && line.size() > 2 && line[2] == ' ') {
            p += 3;
            float u = parseFloat(p, e);
            float v = parseFloat(p, e);
            builder.texcoords.push_back(u);
            builder.texcoords.push_back(v);
        } else if (line[0] == 'v' && line[1] == 'n' && line.size() > 2 && line[2] == ' ') {
            p += 3;
            float x = parseFloat(p, e);
            float y = parseFloat(p, e);
            float z = parseFloat(p, e);
            builder.normals.push_back(x);
            builder.normals.push_back(y);
            builder.normals.push_back(z);
        } else if (line[0] == 'f' && line.size() > 1 && line[1] == ' ') {
            p += 2;

            // Parse all face vertices on this line
            std::vector<FaceVert> faceVerts;
            while (p < e) {
                p = skipws(p, e);
                if (p >= e) break;
                if (*p < '0' && *p != '-') break;
                if (*p > '9') break;
                faceVerts.push_back(parseFaceVert(p, e));
            }

            // Triangulate: fan from first vertex
            if (faceVerts.size() >= 3) {
                for (std::size_t i = 1; i + 1 < faceVerts.size(); ++i) {
                    builder.outIndices.push_back(builder.addVertex(faceVerts[0]));
                    builder.outIndices.push_back(builder.addVertex(faceVerts[i]));
                    builder.outIndices.push_back(builder.addVertex(faceVerts[i + 1]));
                    hasFaces = true;
                }
            }
        } else if ((line[0] == 'g' || line[0] == 'o') && line.size() > 1 && line[1] == ' ') {
            // Flush current group if it has faces
            if (hasFaces) {
                auto mesh = builder.buildMesh();
                if (mesh.vertexCount() > 0) {
                    scene.meshes.push(std::move(mesh));
                }
                // Reset dedup state but keep global pools
                builder.outPositions.clear();
                builder.outTexcoords.clear();
                builder.outNormals.clear();
                builder.outIndices.clear();
                builder.vertMap.clear();
                builder.hasPositions = false;
                builder.hasTexcoords = false;
                builder.hasNormals = false;
                hasFaces = false;
            }
        }
        // usemtl, mtllib, s — recognized but not acted upon in v1
    }

    // Flush final group
    if (hasFaces) {
        auto mesh = builder.buildMesh();
        if (mesh.vertexCount() > 0) {
            scene.meshes.push(std::move(mesh));
        }
    }

    if (scene.meshes.isEmpty()) return LoadError::Truncated;
    return scene;
}

} // namespace f4::resources
