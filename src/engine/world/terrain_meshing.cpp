#include "engine/world/terrain_meshing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace f4::world {

namespace {

struct PackedTerrainVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint8_t color[4];
};

static_assert(sizeof(PackedTerrainVertex) == 36);

struct MeshPart {
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class MeshAssembler {
public:
    void triangle(std::uint16_t material, const TerrainVertex& a,
                  const TerrainVertex& b, const TerrainVertex& c) {
        MeshPart& part = parts_[material];
        const auto base = static_cast<std::uint32_t>(part.vertices.size());
        part.vertices.push_back(a);
        part.vertices.push_back(b);
        part.vertices.push_back(c);
        part.indices.push_back(base);
        part.indices.push_back(base + 1u);
        part.indices.push_back(base + 2u);
    }

    void quad(std::uint16_t material, const std::array<TerrainVertex, 4>& vertices,
              bool flipDiagonal = false) {
        MeshPart& part = parts_[material];
        const auto base = static_cast<std::uint32_t>(part.vertices.size());
        for (const TerrainVertex& vertex : vertices) part.vertices.push_back(vertex);
        if (flipDiagonal) {
            const std::uint32_t local[] = {0, 1, 3, 1, 2, 3};
            for (std::uint32_t index : local) part.indices.push_back(base + index);
        } else {
            const std::uint32_t local[] = {0, 1, 2, 2, 3, 0};
            for (std::uint32_t index : local) part.indices.push_back(base + index);
        }
    }

    [[nodiscard]] auto finish() -> MeshData {
        MeshData output;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
        for (const auto& [material, part] : parts_) {
            (void)material;
            vertexCount += part.vertices.size();
            indexCount += part.indices.size();
        }
        output.vertices.reserve(vertexCount);
        output.indices.reserve(indexCount);
        output.surfaces.reserve(parts_.size());
        for (const auto& [material, part] : parts_) {
            const auto vertexBase = static_cast<std::uint32_t>(output.vertices.size());
            const auto firstIndex = static_cast<std::uint32_t>(output.indices.size());
            for (const TerrainVertex& vertex : part.vertices) output.vertices.push(vertex);
            for (std::uint32_t index : part.indices) output.indices.push(vertexBase + index);
            output.surfaces.push({firstIndex,
                                  static_cast<std::uint32_t>(part.indices.size()),
                                  material});
        }
        return output;
    }

private:
    std::map<std::uint16_t, MeshPart> parts_;
};

[[nodiscard]] auto validWeights(const std::array<std::uint8_t, 4>& weights) -> bool {
    return static_cast<std::uint32_t>(weights[0]) + weights[1] + weights[2] +
               weights[3] ==
           255u;
}

[[nodiscard]] auto weightedBiomeColor(const std::array<std::uint8_t, 4>& weights)
    -> std::array<std::uint8_t, 4> {
    static constexpr std::array<std::array<std::uint8_t, 3>, 4> palette{{
        {{84, 142, 72}}, {{160, 142, 101}}, {{112, 119, 128}}, {{217, 222, 220}}}};
    std::array<std::uint8_t, 4> color{{0, 0, 0, 255}};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        std::uint32_t sum = 0;
        for (std::size_t layer = 0; layer < 4; ++layer) {
            sum += static_cast<std::uint32_t>(palette[layer][channel]) * weights[layer];
        }
        color[channel] = static_cast<std::uint8_t>((sum + 127u) / 255u);
    }
    return color;
}

[[nodiscard]] auto hexColor(std::uint8_t terrain, std::uint8_t scale = 255)
    -> std::array<std::uint8_t, 4> {
    static constexpr std::array<std::array<std::uint8_t, 3>, 3> palette{{
        {{196, 168, 100}}, {{83, 142, 76}}, {{116, 124, 131}}}};
    std::array<std::uint8_t, 3> base{};
    if (terrain < palette.size()) {
        base = palette[terrain];
    } else {
        const std::uint32_t hash = static_cast<std::uint32_t>(terrain) * 0x9e3779b9u;
        base = {{static_cast<std::uint8_t>(72u + (hash & 127u)),
                 static_cast<std::uint8_t>(72u + ((hash >> 8u) & 127u)),
                 static_cast<std::uint8_t>(72u + ((hash >> 16u) & 127u))}};
    }
    return {{static_cast<std::uint8_t>((static_cast<std::uint32_t>(base[0]) * scale) / 255u),
             static_cast<std::uint8_t>((static_cast<std::uint32_t>(base[1]) * scale) / 255u),
             static_cast<std::uint8_t>((static_cast<std::uint32_t>(base[2]) * scale) / 255u),
             255}};
}

[[nodiscard]] auto makeHexVertex(math::Vec3 position, math::Vec3 normal,
                                 math::Vec2 uv, std::uint8_t terrain,
                                 std::uint8_t colorScale) -> TerrainVertex {
    TerrainVertex vertex;
    vertex.position = position;
    vertex.normal = normal;
    vertex.uv = uv;
    vertex.color = hexColor(terrain, colorScale);
    vertex.materialWeights = {{0, 0, 0, 0}};
    vertex.materialWeights[terrain % 4u] = 255;
    vertex.material = terrain;
    vertex.biome = terrain;
    return vertex;
}

constexpr std::int32_t kPaddedEdge = static_cast<std::int32_t>(kVoxelEdge) + 2;

[[nodiscard]] constexpr auto paddedIndex(std::int32_t x, std::int32_t y,
                                         std::int32_t z) -> std::size_t {
    return static_cast<std::size_t>(x + 1) + static_cast<std::size_t>(kPaddedEdge) *
        (static_cast<std::size_t>(z + 1) +
         static_cast<std::size_t>(kPaddedEdge) * static_cast<std::size_t>(y + 1));
}

struct VoxelMaskCell {
    bool visible = false;
    bool positive = true;
    std::uint16_t material = 0;
    std::array<std::uint8_t, 4> ao{{3, 3, 3, 3}};

    friend auto operator==(const VoxelMaskCell&, const VoxelMaskCell&) -> bool = default;
};

[[nodiscard]] auto aoShade(std::uint8_t ao) -> std::uint8_t {
    static constexpr std::array<std::uint8_t, 4> shades{{63, 127, 191, 255}};
    return shades[std::min<std::size_t>(ao, shades.size() - 1u)];
}

[[nodiscard]] auto addCoord(std::int32_t value, std::int32_t delta,
                            std::int32_t& output) -> bool {
    const std::int64_t sum = static_cast<std::int64_t>(value) + delta;
    if (sum < std::numeric_limits<std::int32_t>::min() ||
        sum > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    output = static_cast<std::int32_t>(sum);
    return true;
}

} // namespace

auto toResourceMesh(const MeshData& source) -> resources::Mesh {
    resources::Mesh output;
    if (source.vertices.isEmpty()) return output;

    for (std::uint32_t index : source.indices) {
        if (index >= source.vertices.size()) return {};
    }
    std::uint64_t covered = 0;
    for (const TerrainSurface& surface : source.surfaces) {
        if (surface.firstIndex != covered ||
            surface.indexCount > source.indices.size() - covered) {
            return {};
        }
        covered += surface.indexCount;
    }
    if (!source.surfaces.isEmpty() && covered != source.indices.size()) return {};

    resources::VertexStream stream;
    stream.strideBytes = sizeof(PackedTerrainVertex);
    stream.data.resize(source.vertices.size() * sizeof(PackedTerrainVertex));
    for (std::size_t i = 0; i < source.vertices.size(); ++i) {
        const TerrainVertex& input = source.vertices[i];
        PackedTerrainVertex packed{
            {input.position.x, input.position.y, input.position.z},
            {input.normal.x, input.normal.y, input.normal.z},
            {input.uv.x, input.uv.y},
            {input.color[0], input.color[1], input.color[2], input.color[3]}};
        std::memcpy(stream.data.data() + i * sizeof(PackedTerrainVertex), &packed,
                    sizeof(packed));
    }
    output.streams.push(static_cast<resources::VertexStream&&>(stream));
    output.attributes.push({resources::VertexSemantic::Position,
                            resources::VertexFormat::F32_3, 0,
                            offsetof(PackedTerrainVertex, position)});
    output.attributes.push({resources::VertexSemantic::Normal,
                            resources::VertexFormat::F32_3, 0,
                            offsetof(PackedTerrainVertex, normal)});
    output.attributes.push({resources::VertexSemantic::TexCoord0,
                            resources::VertexFormat::F32_2, 0,
                            offsetof(PackedTerrainVertex, uv)});
    output.attributes.push({resources::VertexSemantic::Color0,
                            resources::VertexFormat::UNORM8_4, 0,
                            offsetof(PackedTerrainVertex, color)});

    const bool useU16 = source.vertices.size() <=
                        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1u;
    output.indexFormat = useU16 ? resources::IndexFormat::U16 : resources::IndexFormat::U32;
    const std::size_t elementSize = resources::indexFormatSize(output.indexFormat);
    output.indices.resize(source.indices.size() * elementSize);
    for (std::size_t i = 0; i < source.indices.size(); ++i) {
        if (useU16) {
            const auto value = static_cast<std::uint16_t>(source.indices[i]);
            std::memcpy(output.indices.data() + i * elementSize, &value, sizeof(value));
        } else {
            const std::uint32_t value = source.indices[i];
            std::memcpy(output.indices.data() + i * elementSize, &value, sizeof(value));
        }
    }
    if (source.surfaces.isEmpty()) {
        if (!source.indices.isEmpty()) output.submeshes.push({0, source.indices.size()});
    } else {
        for (const TerrainSurface& surface : source.surfaces) {
            output.submeshes.push({surface.firstIndex, surface.indexCount});
        }
    }
    output.bounds = resources::computeBounds(output);
    return output;
}

auto meshVoxelChunk(const VoxelCells& cells, const BlockRegistry& registry,
                    const VoxelMeshingOptions& options)
    -> Result<MeshData, TerrainMeshError> {
    if (cells.values.size() != kVoxelCellCount) return TerrainMeshError::InvalidData;

    containers::Array<std::uint16_t> padded;
    padded.resize(static_cast<std::size_t>(kPaddedEdge) * kPaddedEdge * kPaddedEdge);
    for (std::int32_t y = -1; y <= static_cast<std::int32_t>(kVoxelEdge); ++y) {
        for (std::int32_t z = -1; z <= static_cast<std::int32_t>(kVoxelEdge); ++z) {
            for (std::int32_t x = -1; x <= static_cast<std::int32_t>(kVoxelEdge); ++x) {
                const bool local = x >= 0 && y >= 0 && z >= 0 &&
                    x < static_cast<std::int32_t>(kVoxelEdge) &&
                    y < static_cast<std::int32_t>(kVoxelEdge) &&
                    z < static_cast<std::int32_t>(kVoxelEdge);
                const std::uint16_t value = local
                    ? cells.values[voxelIndex(static_cast<std::uint32_t>(x),
                                              static_cast<std::uint32_t>(y),
                                              static_cast<std::uint32_t>(z))]
                    : (options.borderFetch ? options.borderFetch(x, y, z) : 0u);
                if (registry.find(value) == nullptr) return TerrainMeshError::UnknownBlock;
                padded[paddedIndex(x, y, z)] = value;
            }
        }
    }

    const auto sample = [&](const std::array<std::int32_t, 3>& coordinate) {
        return padded[paddedIndex(coordinate[0], coordinate[1], coordinate[2])];
    };
    const auto opaque = [&](const std::array<std::int32_t, 3>& coordinate) {
        return registry.find(sample(coordinate))->opaque;
    };
    const auto vertexAo = [&](const std::array<std::int32_t, 3>& voxel,
                              std::uint32_t axis, std::int32_t sign,
                              std::uint32_t u, std::uint32_t v,
                              std::int32_t uDirection, std::int32_t vDirection) {
        std::array<std::int32_t, 3> outside = voxel;
        outside[axis] += sign;
        std::array<std::int32_t, 3> sideU = outside;
        std::array<std::int32_t, 3> sideV = outside;
        std::array<std::int32_t, 3> corner = outside;
        sideU[u] += uDirection;
        sideV[v] += vDirection;
        corner[u] += uDirection;
        corner[v] += vDirection;
        const bool first = opaque(sideU);
        const bool second = opaque(sideV);
        if (first && second) return static_cast<std::uint8_t>(0);
        return static_cast<std::uint8_t>(3u - static_cast<std::uint8_t>(first) -
                                         static_cast<std::uint8_t>(second) -
                                         static_cast<std::uint8_t>(opaque(corner)));
    };

    MeshAssembler assembler;
    constexpr std::int32_t edge = static_cast<std::int32_t>(kVoxelEdge);
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        const std::uint32_t u = (axis + 1u) % 3u;
        const std::uint32_t v = (axis + 2u) % 3u;
        std::vector<VoxelMaskCell> mask(static_cast<std::size_t>(edge) * edge);
        for (std::int32_t slice = -1; slice < edge; ++slice) {
            for (std::int32_t j = 0; j < edge; ++j) {
                for (std::int32_t i = 0; i < edge; ++i) {
                    std::array<std::int32_t, 3> a{{0, 0, 0}};
                    a[axis] = slice;
                    a[u] = i;
                    a[v] = j;
                    std::array<std::int32_t, 3> b = a;
                    ++b[axis];
                    const std::uint16_t aId = sample(a);
                    const std::uint16_t bId = sample(b);
                    const BlockDesc& aBlock = *registry.find(aId);
                    const BlockDesc& bBlock = *registry.find(bId);
                    VoxelMaskCell cell;
                    const bool sameSolid = aId == bId && aBlock.solid && bBlock.solid;
                    const bool transparentPair = aBlock.solid && bBlock.solid &&
                        !aBlock.opaque && !bBlock.opaque && aId != bId;
                    std::array<std::int32_t, 3> solidVoxel{};
                    std::int32_t sign = 1;
                    if (transparentPair && aId < bId && slice >= 0) {
                        cell.visible = true;
                        cell.positive = true;
                        cell.material = aBlock.textureLayer;
                        solidVoxel = a;
                    } else if (transparentPair && bId < aId && slice + 1 < edge) {
                        cell.visible = true;
                        cell.positive = false;
                        cell.material = bBlock.textureLayer;
                        solidVoxel = b;
                        sign = -1;
                    } else if (!transparentPair && !sameSolid && slice >= 0 &&
                               aBlock.solid && !bBlock.opaque) {
                        cell.visible = true;
                        cell.positive = true;
                        cell.material = aBlock.textureLayer;
                        solidVoxel = a;
                    } else if (!transparentPair && !sameSolid && slice + 1 < edge &&
                               bBlock.solid && !aBlock.opaque) {
                        cell.visible = true;
                        cell.positive = false;
                        cell.material = bBlock.textureLayer;
                        solidVoxel = b;
                        sign = -1;
                    }
                    if (cell.visible) {
                        static constexpr std::array<std::int32_t, 4> uDirections{{-1, 1, 1, -1}};
                        static constexpr std::array<std::int32_t, 4> vDirections{{-1, -1, 1, 1}};
                        for (std::size_t cornerIndex = 0; cornerIndex < 4; ++cornerIndex) {
                            cell.ao[cornerIndex] = vertexAo(
                                solidVoxel, axis, sign, u, v,
                                uDirections[cornerIndex], vDirections[cornerIndex]);
                        }
                    }
                    mask[static_cast<std::size_t>(j) * edge + i] = cell;
                }
            }

            for (std::int32_t j = 0; j < edge; ++j) {
                for (std::int32_t i = 0; i < edge;) {
                    const std::size_t start = static_cast<std::size_t>(j) * edge + i;
                    const VoxelMaskCell key = mask[start];
                    if (!key.visible) {
                        ++i;
                        continue;
                    }
                    std::int32_t width = 1;
                    while (i + width < edge &&
                           mask[static_cast<std::size_t>(j) * edge + i + width] == key) {
                        ++width;
                    }
                    std::int32_t height = 1;
                    bool canGrow = true;
                    while (j + height < edge && canGrow) {
                        for (std::int32_t offset = 0; offset < width; ++offset) {
                            if (!(mask[static_cast<std::size_t>(j + height) * edge +
                                       i + offset] == key)) {
                                canGrow = false;
                                break;
                            }
                        }
                        if (canGrow) ++height;
                    }

                    std::array<std::int32_t, 3> p{{0, 0, 0}};
                    p[axis] = slice + 1;
                    p[u] = i;
                    p[v] = j;
                    std::array<std::int32_t, 3> du{{0, 0, 0}};
                    std::array<std::int32_t, 3> dv{{0, 0, 0}};
                    du[u] = width;
                    dv[v] = height;
                    const std::array<std::array<std::int32_t, 3>, 4> canonical{{
                        p,
                        {{p[0] + du[0], p[1] + du[1], p[2] + du[2]}},
                        {{p[0] + du[0] + dv[0], p[1] + du[1] + dv[1],
                          p[2] + du[2] + dv[2]}},
                        {{p[0] + dv[0], p[1] + dv[1], p[2] + dv[2]}}}};
                    const std::array<std::size_t, 4> order = key.positive
                        ? std::array<std::size_t, 4>{{0, 1, 2, 3}}
                        : std::array<std::size_t, 4>{{0, 3, 2, 1}};
                    const std::array<math::Vec2, 4> canonicalUv{{
                        {0.0f, 0.0f}, {static_cast<float>(width), 0.0f},
                        {static_cast<float>(width), static_cast<float>(height)},
                        {0.0f, static_cast<float>(height)}}};
                    math::Vec3 normal{};
                    normal[axis] = key.positive ? 1.0f : -1.0f;
                    std::array<TerrainVertex, 4> vertices;
                    for (std::size_t vertexIndex = 0; vertexIndex < 4; ++vertexIndex) {
                        const std::size_t source = order[vertexIndex];
                        TerrainVertex& vertex = vertices[vertexIndex];
                        vertex.position = {
                            static_cast<float>(canonical[source][0]),
                            static_cast<float>(canonical[source][1]),
                            static_cast<float>(canonical[source][2])};
                        vertex.normal = normal;
                        vertex.uv = canonicalUv[source];
                        const std::uint8_t ao = key.ao[source];
                        const std::uint8_t shade = aoShade(ao);
                        vertex.color = {{shade, shade, shade, 255}};
                        vertex.material = key.material;
                        vertex.ambientOcclusion = ao;
                    }
                    const bool flip = vertices[0].ambientOcclusion +
                                          vertices[2].ambientOcclusion >
                                      vertices[1].ambientOcclusion +
                                          vertices[3].ambientOcclusion;
                    assembler.quad(key.material, vertices, flip);

                    for (std::int32_t y = 0; y < height; ++y) {
                        for (std::int32_t x = 0; x < width; ++x) {
                            mask[static_cast<std::size_t>(j + y) * edge + i + x] = {};
                        }
                    }
                    i += width;
                }
            }
        }
    }
    return assembler.finish();
}

auto meshHeightChunk(const HeightTile& tile, ChunkCoord coord,
                     const HeightMeshingOptions& options)
    -> Result<MeshData, TerrainMeshError> {
    if (tile.samples.size() != kHeightSampleCount ||
        !std::isfinite(options.skirtDepth) || options.skirtDepth <= 0.0f ||
        (options.transitionFloor.has_value() &&
         !std::isfinite(*options.transitionFloor))) {
        return TerrainMeshError::InvalidData;
    }
    for (const HeightSample& sample : tile.samples) {
        if (!std::isfinite(sample.height) || !validWeights(sample.materialWeights)) {
            return TerrainMeshError::InvalidData;
        }
    }

    MeshData output;
    constexpr std::size_t topVertices = kHeightSampleCount;
    constexpr std::size_t skirtVertices = 4u * 2u * kHeightSamples;
    constexpr std::size_t topIndices = kHeightQuads * kHeightQuads * 6u;
    constexpr std::size_t skirtIndices = 4u * kHeightQuads * 6u;
    output.vertices.reserve(topVertices + skirtVertices);
    output.indices.reserve(topIndices + skirtIndices);

    const std::int64_t baseX = static_cast<std::int64_t>(coord.x) * kHeightQuads;
    const std::int64_t baseZ = static_cast<std::int64_t>(coord.z) * kHeightQuads;
    bool invalidFetch = false;
    const auto fetchHeight = [&](std::int64_t globalX, std::int64_t globalZ) {
        const std::int64_t localX = globalX - baseX;
        const std::int64_t localZ = globalZ - baseZ;
        if (localX >= 0 && localZ >= 0 && localX <= kHeightQuads &&
            localZ <= kHeightQuads) {
            return tile.samples[heightSampleIndex(static_cast<std::uint32_t>(localX),
                                                  static_cast<std::uint32_t>(localZ))].height;
        }
        if (options.borderFetch) {
            const float value = options.borderFetch(globalX, globalZ);
            if (!std::isfinite(value)) invalidFetch = true;
            return std::isfinite(value) ? value : 0.0f;
        }
        const auto x = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(localX, 0, kHeightQuads));
        const auto z = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(localZ, 0, kHeightQuads));
        return tile.samples[heightSampleIndex(x, z)].height;
    };

    for (std::uint32_t z = 0; z < kHeightSamples; ++z) {
        for (std::uint32_t x = 0; x < kHeightSamples; ++x) {
            const HeightSample& sample = tile.samples[heightSampleIndex(x, z)];
            const std::int64_t globalX = baseX + x;
            const std::int64_t globalZ = baseZ + z;
            TerrainVertex vertex;
            vertex.position = {static_cast<float>(x), sample.height,
                               static_cast<float>(z)};
            vertex.normal = math::normalized({
                fetchHeight(globalX - 1, globalZ) - fetchHeight(globalX + 1, globalZ),
                2.0f,
                fetchHeight(globalX, globalZ - 1) - fetchHeight(globalX, globalZ + 1)});
            vertex.uv = {static_cast<float>(x) / kHeightQuads,
                         static_cast<float>(z) / kHeightQuads};
            vertex.color = weightedBiomeColor(sample.materialWeights);
            vertex.materialWeights = sample.materialWeights;
            vertex.biome = sample.biomeId;
            output.vertices.push(vertex);
        }
    }
    if (invalidFetch) return TerrainMeshError::InvalidData;

    for (std::uint32_t z = 0; z < kHeightQuads; ++z) {
        for (std::uint32_t x = 0; x < kHeightQuads; ++x) {
            const std::uint32_t a = heightSampleIndex(x, z);
            const std::uint32_t b = heightSampleIndex(x + 1u, z);
            const std::uint32_t c = heightSampleIndex(x, z + 1u);
            const std::uint32_t d = heightSampleIndex(x + 1u, z + 1u);
            const std::uint32_t triangles[] = {a, c, d, d, b, a};
            for (std::uint32_t index : triangles) output.indices.push(index);
        }
    }

    const auto appendSkirt = [&](const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edge,
                                 math::Vec3 normal) -> bool {
        const std::uint32_t base = static_cast<std::uint32_t>(output.vertices.size());
        for (std::size_t i = 0; i < edge.size(); ++i) {
            const auto [x, z] = edge[i];
            const HeightSample& sample = tile.samples[heightSampleIndex(x, z)];
            const float bottom = options.transitionFloor.has_value()
                ? *options.transitionFloor
                : sample.height - options.skirtDepth;
            if (bottom >= sample.height) return false;
            TerrainVertex top;
            top.position = {static_cast<float>(x), sample.height, static_cast<float>(z)};
            top.normal = normal;
            top.uv = {static_cast<float>(i), 0.0f};
            top.color = weightedBiomeColor(sample.materialWeights);
            top.materialWeights = sample.materialWeights;
            top.biome = sample.biomeId;
            TerrainVertex low = top;
            low.position.y = bottom;
            low.uv.y = sample.height - bottom;
            output.vertices.push(top);
            output.vertices.push(low);
        }
        for (std::uint32_t segment = 0; segment < edge.size() - 1u; ++segment) {
            const std::uint32_t top0 = base + segment * 2u;
            const std::uint32_t low0 = top0 + 1u;
            const std::uint32_t top1 = top0 + 2u;
            const std::uint32_t low1 = top0 + 3u;
            const std::uint32_t triangles[] = {top0, top1, low1, low1, low0, top0};
            for (std::uint32_t index : triangles) output.indices.push(index);
        }
        return true;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> edge;
    edge.reserve(kHeightSamples);
    for (std::uint32_t x = 0; x < kHeightSamples; ++x) edge.emplace_back(x, 0u);
    if (!appendSkirt(edge, {0.0f, 0.0f, -1.0f})) return TerrainMeshError::InvalidData;
    edge.clear();
    for (std::uint32_t z = 0; z < kHeightSamples; ++z) edge.emplace_back(kHeightQuads, z);
    if (!appendSkirt(edge, {1.0f, 0.0f, 0.0f})) return TerrainMeshError::InvalidData;
    edge.clear();
    for (std::uint32_t x = kHeightSamples; x-- > 0;) edge.emplace_back(x, kHeightQuads);
    if (!appendSkirt(edge, {0.0f, 0.0f, 1.0f})) return TerrainMeshError::InvalidData;
    edge.clear();
    for (std::uint32_t z = kHeightSamples; z-- > 0;) edge.emplace_back(0u, z);
    if (!appendSkirt(edge, {-1.0f, 0.0f, 0.0f})) return TerrainMeshError::InvalidData;

    output.surfaces.push({0, static_cast<std::uint32_t>(output.indices.size()), 0});
    return output;
}

auto meshHexChunk(const HexTile& tile, ChunkCoord coord,
                  const HexMeshingOptions& options)
    -> Result<MeshData, TerrainMeshError> {
    const std::uint64_t expectedWidth = static_cast<std::uint64_t>(tile.radius) * 2u + 1u;
    if (expectedWidth > std::numeric_limits<std::uint32_t>::max() ||
        tile.width != expectedWidth ||
        tile.cells.size() != static_cast<std::size_t>(tile.width) * tile.width ||
        tile.radius > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        !std::isfinite(options.radius) || options.radius <= 0.0f ||
        !std::isfinite(options.floorHeight)) {
        return TerrainMeshError::InvalidData;
    }

    MeshAssembler assembler;
    const std::int32_t radius = static_cast<std::int32_t>(tile.radius);
    constexpr float sqrtThreeOverTwo = 0.8660254037844386f;
    for (std::int32_t r = -radius; r <= radius; ++r) {
        const std::int32_t qMin = std::max(-radius, -r - radius);
        const std::int32_t qMax = std::min(radius, -r + radius);
        for (std::int32_t q = qMin; q <= qMax; ++q) {
            const AxialCoord cell{q, r};
            const std::optional<std::uint32_t> cellIndex = tile.index(cell);
            if (!cellIndex.has_value()) return TerrainMeshError::InvalidData;
            const HexCell& value = tile.cells[*cellIndex];
            if (value.terrainType == kInvalidTerrain) return TerrainMeshError::InvalidData;

            std::array<math::Vec3, 6> corners;
            float centerHeight = 0.0f;
            for (std::uint32_t cornerIndex = 0; cornerIndex < 6; ++cornerIndex) {
                const std::optional<float> height = tile.corner(cell, cornerIndex);
                if (!height.has_value() || !std::isfinite(*height) ||
                    options.floorHeight >= *height) {
                    return TerrainMeshError::InvalidData;
                }
                const HexCornerCoord lattice = hexCorner(cell, cornerIndex);
                corners[cornerIndex] = {
                    static_cast<float>(lattice.x2) * sqrtThreeOverTwo * options.radius,
                    *height,
                    static_cast<float>(lattice.y2) * 0.5f * options.radius};
                centerHeight += *height;
            }
            centerHeight /= 6.0f;
            const math::Vec3 center{
                (static_cast<float>(q) * 1.7320508075688772f +
                 static_cast<float>(r) * sqrtThreeOverTwo) * options.radius,
                centerHeight,
                static_cast<float>(r) * 1.5f * options.radius};
            const std::uint16_t material = value.terrainType;

            for (std::uint32_t cornerIndex = 0; cornerIndex < 6; ++cornerIndex) {
                const std::uint32_t next = (cornerIndex + 1u) % 6u;
                const math::Vec3 normal = math::normalized(
                    math::cross(corners[next] - center, corners[cornerIndex] - center));
                assembler.triangle(
                    material,
                    makeHexVertex(center, normal, {0.5f, 0.5f}, value.terrainType, 255),
                    makeHexVertex(corners[next], normal,
                                  {0.5f + (corners[next].x - center.x) * 0.5f / options.radius,
                                   0.5f + (corners[next].z - center.z) * 0.5f / options.radius},
                                  value.terrainType, 255),
                    makeHexVertex(corners[cornerIndex], normal,
                                  {0.5f + (corners[cornerIndex].x - center.x) * 0.5f /
                                              options.radius,
                                   0.5f + (corners[cornerIndex].z - center.z) * 0.5f /
                                              options.radius},
                                  value.terrainType, 255));

                const math::Vec3 bottomCenter{center.x, options.floorHeight, center.z};
                math::Vec3 bottomCurrent = corners[cornerIndex];
                math::Vec3 bottomNext = corners[next];
                bottomCurrent.y = options.floorHeight;
                bottomNext.y = options.floorHeight;
                assembler.triangle(
                    material,
                    makeHexVertex(bottomCenter, {0.0f, -1.0f, 0.0f}, {0.5f, 0.5f},
                                  value.terrainType, 150),
                    makeHexVertex(bottomCurrent, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f},
                                  value.terrainType, 150),
                    makeHexVertex(bottomNext, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f},
                                  value.terrainType, 150));
            }

            const std::int64_t globalQ64 = static_cast<std::int64_t>(coord.x) * tile.width + q;
            const std::int64_t globalR64 = static_cast<std::int64_t>(coord.z) * tile.width + r;
            if (globalQ64 < std::numeric_limits<std::int32_t>::min() ||
                globalQ64 > std::numeric_limits<std::int32_t>::max() ||
                globalR64 < std::numeric_limits<std::int32_t>::min() ||
                globalR64 > std::numeric_limits<std::int32_t>::max()) {
                return TerrainMeshError::CoordinateOverflow;
            }
            const AxialCoord global{static_cast<std::int32_t>(globalQ64),
                                    static_cast<std::int32_t>(globalR64)};
            for (std::uint32_t direction = 0; direction < 6; ++direction) {
                const AxialCoord localNeighbor{q + kHexDirections[direction].q,
                                               r + kHexDirections[direction].r};
                if (tile.contains(localNeighbor)) continue;
                AxialCoord globalNeighbor;
                if (!addCoord(global.q, kHexDirections[direction].q, globalNeighbor.q) ||
                    !addCoord(global.r, kHexDirections[direction].r, globalNeighbor.r)) {
                    return TerrainMeshError::CoordinateOverflow;
                }
                if (options.hasNeighbor && options.hasNeighbor(globalNeighbor)) continue;

                const std::uint32_t firstCorner = (5u + 6u - direction) % 6u;
                const std::uint32_t secondCorner = (6u - direction) % 6u;
                math::Vec3 bottomFirst = corners[firstCorner];
                math::Vec3 bottomSecond = corners[secondCorner];
                bottomFirst.y = options.floorHeight;
                bottomSecond.y = options.floorHeight;
                const math::Vec3 normal = math::normalized(math::cross(
                    corners[secondCorner] - corners[firstCorner],
                    bottomSecond - corners[firstCorner]));
                std::array<TerrainVertex, 4> side{{
                    makeHexVertex(corners[firstCorner], normal, {0.0f, 0.0f},
                                  value.terrainType, 205),
                    makeHexVertex(corners[secondCorner], normal, {1.0f, 0.0f},
                                  value.terrainType, 205),
                    makeHexVertex(bottomSecond, normal,
                                  {1.0f, corners[secondCorner].y - options.floorHeight},
                                  value.terrainType, 205),
                    makeHexVertex(bottomFirst, normal,
                                  {0.0f, corners[firstCorner].y - options.floorHeight},
                                  value.terrainType, 205)}};
                assembler.quad(material, side);
            }
        }
    }
    return assembler.finish();
}

auto meshChunkSnapshot(const ChunkSnapshot& snapshot,
                       const TerrainMeshingContext& context)
    -> Result<TerrainMeshBuild, TerrainMeshError> {
    if (snapshot.state < ChunkState::CellsReady) return TerrainMeshError::InvalidData;
    TerrainMeshBuild output;
    output.coord = snapshot.coord;
    output.policy = snapshot.policy;
    output.sourceVersion = snapshot.version;

    switch (snapshot.policy) {
    case PolicyKind::Voxel: {
        if (context.blocks == nullptr) return TerrainMeshError::MissingBlockRegistry;
        const VoxelCells* base = std::get_if<VoxelCells>(&snapshot.base);
        VoxelCells cells;
        if (base != nullptr) {
            if (base->values.size() != kVoxelCellCount) return TerrainMeshError::InvalidData;
            cells = *base;
        } else {
            const PackedVoxelCells* packed = std::get_if<PackedVoxelCells>(&snapshot.base);
            if (packed == nullptr) return TerrainMeshError::InvalidData;
            std::optional<VoxelCells> unpacked = unpackVoxelCells(*packed);
            if (!unpacked.has_value()) return TerrainMeshError::InvalidData;
            cells = std::move(*unpacked);
        }
        for (const ChunkDelta& delta : snapshot.deltas) {
            if (delta.value.kind != PolicyKind::Voxel || delta.index >= kVoxelCellCount) {
                return TerrainMeshError::InvalidData;
            }
            cells.values[delta.index] = delta.value.voxel;
        }
        auto mesh = meshVoxelChunk(cells, *context.blocks, context.voxel);
        if (!mesh) return mesh.error();
        output.mesh = static_cast<MeshData&&>(mesh.value());
        break;
    }
    case PolicyKind::Height: {
        const HeightTile* base = std::get_if<HeightTile>(&snapshot.base);
        if (base == nullptr || base->samples.size() != kHeightSampleCount) {
            return TerrainMeshError::InvalidData;
        }
        HeightTile tile = *base;
        for (const ChunkDelta& delta : snapshot.deltas) {
            if (delta.value.kind != PolicyKind::Height || delta.index >= kHeightSampleCount) {
                return TerrainMeshError::InvalidData;
            }
            tile.samples[delta.index] = delta.value.height;
        }
        auto mesh = meshHeightChunk(tile, snapshot.coord, context.height);
        if (!mesh) return mesh.error();
        output.mesh = static_cast<MeshData&&>(mesh.value());
        break;
    }
    case PolicyKind::Hex: {
        const HexTile* base = std::get_if<HexTile>(&snapshot.base);
        if (base == nullptr) return TerrainMeshError::InvalidData;
        HexTile tile = *base;
        for (const ChunkDelta& delta : snapshot.deltas) {
            if (delta.value.kind != PolicyKind::Hex || delta.index >= tile.cells.size() ||
                tile.cells[delta.index].terrainType == kInvalidTerrain) {
                return TerrainMeshError::InvalidData;
            }
            tile.cells[delta.index] = delta.value.hex;
        }
        auto mesh = meshHexChunk(tile, snapshot.coord, context.hex);
        if (!mesh) return mesh.error();
        output.mesh = static_cast<MeshData&&>(mesh.value());
        break;
    }
    }
    return output;
}

auto commitTerrainMesh(ChunkStore& store, ChunkHandle handle,
                       const TerrainMeshBuild& build) -> bool {
    const std::optional<ChunkSnapshot> current = store.snapshot(handle);
    if (!current.has_value() || current->coord != build.coord ||
        current->policy != build.policy || current->version != build.sourceVersion) {
        return false;
    }
    return store.markMeshed(handle, build.sourceVersion);
}

} // namespace f4::world
