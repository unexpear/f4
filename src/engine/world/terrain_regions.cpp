#include "engine/world/terrain_regions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace f4::world {
namespace {
using D3 = std::array<double, 3>;
struct Point { D3 position, weights; };

auto finite(math::Vec3 v) -> bool {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

auto clipPlane(const std::vector<Point>& input, unsigned axis, double boundary, bool lower)
    -> std::vector<Point> {
    std::vector<Point> output;
    if (input.empty()) return output;
    const auto inside = [&](const Point& p) {
        return lower ? p.position[axis] >= boundary : p.position[axis] <= boundary;
    };
    auto previous = input.back();
    for (const auto& current : input) {
        if (inside(previous) != inside(current)) {
            // Canonical edge direction gives adjacent faces the same arithmetic
            // at a shared cut, even when their windings traverse it oppositely.
            const auto& a = previous.position < current.position ? previous : current;
            const auto& b = previous.position < current.position ? current : previous;
            const double t = (boundary - a.position[axis]) / (b.position[axis] - a.position[axis]);
            Point cut;
            for (unsigned i = 0; i < 3; ++i) {
                cut.position[i] = a.position[i] + t * (b.position[i] - a.position[i]);
                cut.weights[i] = a.weights[i] + t * (b.weights[i] - a.weights[i]);
            }
            cut.position[axis] = boundary;
            output.push_back(cut);
        }
        if (inside(current)) output.push_back(current);
        previous = current;
    }
    return output;
}

auto vertexAt(const Point& p, const std::array<TerrainVertex, 3>& source) -> TerrainVertex {
    const auto interpolate = [&](auto member) {
        return p.weights[0] * member(source[0]) + p.weights[1] * member(source[1]) +
               p.weights[2] * member(source[2]);
    };
    // Categorical attributes use the dominant original corner, with index ties.
    const auto dominant = static_cast<std::size_t>(std::max_element(p.weights.begin(), p.weights.end()) - p.weights.begin());
    auto result = source[dominant];
    result.position = {static_cast<float>(p.position[0]), static_cast<float>(p.position[1]), static_cast<float>(p.position[2])};
    result.normal = {static_cast<float>(interpolate([](auto v) { return v.normal.x; })),
                     static_cast<float>(interpolate([](auto v) { return v.normal.y; })),
                     static_cast<float>(interpolate([](auto v) { return v.normal.z; }))};
    result.uv = {static_cast<float>(interpolate([](auto v) { return v.uv.x; })),
                 static_cast<float>(interpolate([](auto v) { return v.uv.y; }))};
    result.ambientOcclusion = static_cast<std::uint8_t>(std::clamp(std::lround(interpolate([](auto v) { return v.ambientOcclusion; })), 0L, 3L));
    std::array<double, 4> remainders;
    unsigned sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result.color[i] = static_cast<std::uint8_t>(std::clamp(std::lround(interpolate([i](auto v) { return v.color[i]; })), 0L, 255L));
        const double weight = std::clamp(interpolate([i](auto v) { return v.materialWeights[i]; }), 0.0, 255.0);
        result.materialWeights[i] = static_cast<std::uint8_t>(std::floor(weight));
        remainders[i] = weight - result.materialWeights[i];
        sum += result.materialWeights[i];
    }
    // Largest-remainder quantization preserves the material sum of 255.
    while (sum < 255) {
        const auto best = static_cast<std::size_t>(std::max_element(remainders.begin(), remainders.end()) - remainders.begin());
        ++result.materialWeights[best];
        remainders[best] = -1;
        ++sum;
    }
    return result;
}

auto nondegenerate(const TerrainVertex& a, const TerrainVertex& b, const TerrainVertex& c) -> bool {
    const D3 u{double(b.position.x) - a.position.x, double(b.position.y) - a.position.y, double(b.position.z) - a.position.z};
    const D3 v{double(c.position.x) - a.position.x, double(c.position.y) - a.position.y, double(c.position.z) - a.position.z};
    return u[1] * v[2] != u[2] * v[1] || u[2] * v[0] != u[0] * v[2] || u[0] * v[1] != u[1] * v[0];
}

// -X/+X/-Z/+Z planes that border a different policy. Tiles themselves are
// closed for geometry; internal same-policy region lines never get cut walls.
auto exposedSides(const TerrainFootprint& r, const RegionMap& regions, PolicyKind policy) -> std::array<bool, 4> {
    constexpr auto lo = std::numeric_limits<std::int32_t>::min();
    constexpr auto hi = std::numeric_limits<std::int32_t>::max();
    const auto foreign = [&](double x, double z) {
        return x < lo || x > hi || z < lo || z > hi ||
            regions.policyAt(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)) != policy;
    };
    const double x = std::max<double>(lo, r.minX), z = std::max<double>(lo, r.minZ);
    return {foreign(r.minX - 1, z), foreign(r.maxX, z), foreign(x, r.minZ - 1), foreign(x, r.maxZ)};
}
} // namespace

auto terrainRegionMask(TerrainFootprint f, const RegionMap& regions, PolicyKind policy)
    -> Result<TerrainRegionMask, TerrainMeshError> {
    if (!std::isfinite(f.minX) || !std::isfinite(f.maxX) || !std::isfinite(f.minZ) || !std::isfinite(f.maxZ) ||
        f.minX >= f.maxX || f.minZ >= f.maxZ) return TerrainMeshError::InvalidData;
    constexpr double lo = std::numeric_limits<std::int32_t>::min();
    constexpr double hi = double(std::numeric_limits<std::int32_t>::max()) + 1;
    if (f.minX < lo || f.minZ < lo || f.maxX > hi || f.maxZ > hi) return TerrainMeshError::CoordinateOverflow;
    const auto extent = regions.tileExtentMeters();
    const auto regionAt = [extent](double coordinate) {
        auto index = static_cast<std::int64_t>(std::floor(coordinate / extent));
        if (double(index * extent) > coordinate) --index;
        return index;
    };
    const auto x0 = regionAt(f.minX), z0 = regionAt(f.minZ);
    auto x1 = regionAt(f.maxX), z1 = regionAt(f.maxZ);
    if (double(x1 * extent) == f.maxX) --x1;
    if (double(z1 * extent) == f.maxZ) --z1;
    const auto nx = x1 - x0 + 1, nz = z1 - z0 + 1;
    if (nx > 4096 || nz > 4096 || nx * nz > 4096) return TerrainMeshError::RegionLimitExceeded;
    TerrainRegionMask result;
    result.fullyOwned = true;
    for (auto z = z0; z <= z1; ++z) {
        for (auto x = x0; x <= x1; ++x) {
            const auto worldX = static_cast<std::int32_t>(std::max<double>(lo, double(x * extent)));
            const auto worldZ = static_cast<std::int32_t>(std::max<double>(lo, double(z * extent)));
            if (regions.policyAt(worldX, worldZ) == policy) {
                result.rectangles.push({double(x * extent), double((x + 1) * extent),
                                        double(z * extent), double((z + 1) * extent)});
            } else result.fullyOwned = false;
        }
    }
    return result;
}

auto clipTerrainRegions(const MeshData& mesh, math::Vec3 translation, const RegionMap& regions, PolicyKind policy,
                        std::optional<float> hexCutFloor)
    -> Result<MeshData, TerrainMeshError> {
    if (!finite(translation) || mesh.indices.size() % 3 != 0) return TerrainMeshError::InvalidData;
    if (hexCutFloor && (policy != PolicyKind::Hex || !std::isfinite(*hexCutFloor))) return TerrainMeshError::InvalidData;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return TerrainMeshError::InvalidData;
    for (const auto& vertex : mesh.vertices) {
        unsigned sum = 0;
        for (auto weight : vertex.materialWeights) sum += weight;
        if (!finite(vertex.position) || !finite(vertex.normal) || !std::isfinite(vertex.uv.x) ||
            !std::isfinite(vertex.uv.y) || sum != 255 || vertex.ambientOcclusion > 3 ||
            (hexCutFloor && vertex.position.y < *hexCutFloor)) return TerrainMeshError::InvalidData;
    }
    if (hexCutFloor && !mesh.indices.isEmpty()) {
        bool top = false, bottom = false;
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i + 1]].position,
                       c = mesh.vertices[mesh.indices[i + 2]].position;
            const double area = (double(b.z) - a.z) * (double(c.x) - a.x) - (double(b.x) - a.x) * (double(c.z) - a.z);
            if (area > 0) {
                if (a.y <= *hexCutFloor || b.y <= *hexCutFloor || c.y <= *hexCutFloor) return TerrainMeshError::InvalidData;
                top = true;
            } else if (area < 0) {
                if (a.y != *hexCutFloor || b.y != *hexCutFloor || c.y != *hexCutFloor) return TerrainMeshError::InvalidData;
                bottom = true;
            }
        }
        if (!top || !bottom) return TerrainMeshError::InvalidData;
    }
    std::size_t covered = 0;
    for (const auto& surface : mesh.surfaces) {
        if (surface.firstIndex != covered || surface.indexCount % 3 != 0 ||
            surface.indexCount > mesh.indices.size() - covered) return TerrainMeshError::InvalidData;
        covered += surface.indexCount;
    }
    if (!mesh.surfaces.isEmpty() && covered != mesh.indices.size()) return TerrainMeshError::InvalidData;
    if (mesh.indices.isEmpty()) return MeshData{};
    TerrainFootprint f{INFINITY, -INFINITY, INFINITY, -INFINITY};
    for (auto index : mesh.indices) {
        const auto p = mesh.vertices[index].position;
        f.minX = std::min(f.minX, double(p.x) + translation.x); f.maxX = std::max(f.maxX, double(p.x) + translation.x);
        f.minZ = std::min(f.minZ, double(p.z) + translation.z); f.maxZ = std::max(f.maxZ, double(p.z) + translation.z);
    }
    // Wholly vertical meshes on a region plane belong to the closure of either
    // adjacent owner; a tiny envelope lets the ordinary clip path handle them.
    if (f.minX == f.maxX) { f.minX = std::nextafter(f.minX, -INFINITY); f.maxX = std::nextafter(f.maxX, INFINITY); }
    if (f.minZ == f.maxZ) { f.minZ = std::nextafter(f.minZ, -INFINITY); f.maxZ = std::nextafter(f.maxZ, INFINITY); }
    auto mask = terrainRegionMask(f, regions, policy);
    if (!mask) return mask.error();
    if (mask.value().fullyOwned && (!hexCutFloor || std::none_of(mask.value().rectangles.begin(), mask.value().rectangles.end(),
        [&](const auto& r) { const auto sides = exposedSides(r, regions, policy); return std::any_of(sides.begin(), sides.end(), [](bool side) { return side; }); }))) return mesh;
    MeshData output;
    const auto append = [&](std::uint16_t material, const TerrainVertex& a, const TerrainVertex& b, const TerrainVertex& c) {
        if (!nondegenerate(a, b, c)) return true;
        if (output.indices.size() > std::numeric_limits<std::uint32_t>::max() - 3u) return false;
        const auto base = static_cast<std::uint32_t>(output.vertices.size());
        if (output.surfaces.isEmpty() || output.surfaces.back().material != material) output.surfaces.push({base, 0, material});
        output.vertices.push(a); output.vertices.push(b); output.vertices.push(c);
        output.indices.push(base); output.indices.push(base + 1); output.indices.push(base + 2);
        output.surfaces.back().indexCount += 3;
        return true;
    };
    std::size_t surfaceIndex = 0;
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
        while (!mesh.surfaces.isEmpty() && index >= std::size_t(mesh.surfaces[surfaceIndex].firstIndex) + mesh.surfaces[surfaceIndex].indexCount) ++surfaceIndex;
        const std::array<TerrainVertex, 3> source{mesh.vertices[mesh.indices[index]], mesh.vertices[mesh.indices[index + 1]], mesh.vertices[mesh.indices[index + 2]]};
        const auto material = mesh.surfaces.isEmpty() ? source[0].material : mesh.surfaces[surfaceIndex].material;
        const auto a = source[0].position, b = source[1].position, c = source[2].position;
        const double upwardArea = (double(b.z) - a.z) * (double(c.x) - a.x) - (double(b.x) - a.x) * (double(c.z) - a.z);
        std::vector<Point> triangle;
        for (unsigned i = 0; i < 3; ++i) {
            const auto p = source[i].position;
            Point point{{p.x, p.y, p.z}, {0, 0, 0}};
            point.weights[i] = 1;
            triangle.push_back(point);
        }
        for (const auto& rectangle : mask.value().rectangles) {
            // A vertical face on an internal region line belongs to the first
            // owned rectangle only; nonzero-area faces meet at shared edges.
            bool duplicate = false;
            for (const unsigned axis : {0u, 2u}) {
                const double lower = axis == 0 ? rectangle.minX : rectangle.minZ;
                const double offset = axis == 0 ? translation.x : translation.z;
                if (!std::all_of(triangle.begin(), triangle.end(), [&](auto p) { return p.position[axis] + offset == lower; })) continue;
                for (const auto& prior : mask.value().rectangles) {
                    if (&prior == &rectangle) break;
                    if ((axis == 0 && prior.maxX == lower && prior.minZ == rectangle.minZ) ||
                        (axis == 2 && prior.maxZ == lower && prior.minX == rectangle.minX)) duplicate = true;
                }
            }
            if (duplicate) continue;
            auto polygon = clipPlane(triangle, 0, rectangle.minX - translation.x, true);
            polygon = clipPlane(polygon, 0, rectangle.maxX - translation.x, false);
            polygon = clipPlane(polygon, 2, rectangle.minZ - translation.z, true);
            polygon = clipPlane(polygon, 2, rectangle.maxZ - translation.z, false);
            const auto sides = hexCutFloor ? exposedSides(rectangle, regions, policy) : std::array<bool, 4>{};
            const std::array<double, 4> planes{rectangle.minX - translation.x, rectangle.maxX - translation.x,
                                              rectangle.minZ - translation.z, rectangle.maxZ - translation.z};
            bool replacedWall = false;
            if (hexCutFloor && upwardArea == 0 && !polygon.empty()) {
                for (unsigned side = 0; side < 4; ++side) {
                    const unsigned axis = side < 2 ? 0 : 2;
                    if (sides[side] && std::all_of(polygon.begin(), polygon.end(), [&](auto p) { return p.position[axis] == planes[side]; })) replacedWall = true;
                }
            }
            if (replacedWall) continue;
            bool hasArea = false;
            for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
                const auto va = vertexAt(polygon[0], source), vb = vertexAt(polygon[i], source), vc = vertexAt(polygon[i + 1], source);
                hasArea = hasArea || nondegenerate(va, vb, vc);
                if (!append(material, va, vb, vc)) return TerrainMeshError::CoordinateOverflow;
            }
            if (hexCutFloor && upwardArea > 0 && hasArea) {
                for (unsigned side = 0; side < 4; ++side) {
                    if (!sides[side]) continue;
                    const unsigned axis = side < 2 ? 0 : 2;
                    for (std::size_t i = 0; i < polygon.size(); ++i) {
                        const auto& p = polygon[i]; const auto& q = polygon[(i + 1) % polygon.size()];
                        if (p.position[axis] != planes[side] || q.position[axis] != planes[side]) continue;
                        auto topA = vertexAt(p, source), topB = vertexAt(q, source);
                        const math::Vec3 normal = side == 0 ? math::Vec3{-1, 0, 0} : side == 1 ? math::Vec3{1, 0, 0} :
                            side == 2 ? math::Vec3{0, 0, -1} : math::Vec3{0, 0, 1};
                        topA.normal = topB.normal = normal;
                        const unsigned tangent = axis == 0 ? 2 : 0;
                        topA.uv = {static_cast<float>(p.position[tangent]), topA.position.y - *hexCutFloor};
                        topB.uv = {static_cast<float>(q.position[tangent]), topB.position.y - *hexCutFloor};
                        auto bottomA = topA, bottomB = topB;
                        bottomA.position.y = bottomB.position.y = *hexCutFloor;
                        bottomA.uv.y = bottomB.uv.y = 0;
                        // Reverse the upward polygon edge to orient the wall
                        // away from the retained prism volume.
                        if (!append(material, topB, topA, bottomA) || !append(material, topB, bottomA, bottomB)) return TerrainMeshError::CoordinateOverflow;
                    }
                }
            }
        }
    }
    return output;
}
auto trimTerrainBoundaryFaces(const MeshData& mesh, math::Vec3 translation, TerrainBoundaryLine line,
                              std::size_t triangleLimit) -> Result<TerrainBoundaryTrim, TerrainBoundaryTrimError> {
    using Error = TerrainBoundaryTrimError;
    if (!finite(translation) || (line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !std::isfinite(line.plane) || !std::isfinite(line.begin) || !std::isfinite(line.end) || line.begin >= line.end ||
        triangleLimit == 0 || triangleLimit > 131072 || mesh.indices.size() % 3 != 0 ||
        mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max()) return Error::InvalidData;
    const unsigned axis = line.axis == TerrainBoundaryAxis::X ? 0 : 2, tangent = axis == 0 ? 2 : 0;
    const D3 offset{translation.x, translation.y, translation.z};
    const double plane = line.plane - offset[axis], begin = line.begin - offset[tangent], end = line.end - offset[tangent];
    for (const double cut : {plane, begin, end}) {
        if (!std::isfinite(cut) || std::abs(cut) > std::numeric_limits<float>::max() ||
            double(static_cast<float>(cut)) != cut) return Error::PrecisionLoss;
    }
    if (begin >= end || plane + offset[axis] != line.plane || begin + offset[tangent] != line.begin ||
        end + offset[tangent] != line.end) return Error::PrecisionLoss;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return Error::InvalidData;
    for (const auto& v : mesh.vertices) {
        unsigned sum = 0;
        for (auto weight : v.materialWeights) sum += weight;
        if (!finite(v.position) || !finite(v.normal) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y) ||
            sum != 255 || v.ambientOcclusion > 3) return Error::InvalidData;
    }
    std::size_t covered = 0;
    for (const auto& surface : mesh.surfaces) {
        if (surface.firstIndex != covered || surface.indexCount % 3 != 0 ||
            surface.indexCount > mesh.indices.size() - covered) return Error::InvalidData;
        covered += surface.indexCount;
    }
    if (!mesh.surfaces.isEmpty() && covered != mesh.indices.size()) return Error::InvalidData;
    TerrainBoundaryTrim output;
    output.mesh.vertices = mesh.vertices;
    const auto emit = [&](std::array<std::uint32_t, 3> indices, std::uint16_t material, std::size_t original) {
        if (output.sourceTriangles.size() == triangleLimit) return false;
        if (output.mesh.surfaces.isEmpty() || output.mesh.surfaces.back().material != material)
            output.mesh.surfaces.push({static_cast<std::uint32_t>(output.mesh.indices.size()), 0, material});
        for (const auto index : indices) output.mesh.indices.push(index);
        output.mesh.surfaces.back().indexCount += 3;
        output.sourceTriangles.push(original);
        return true;
    };
    std::size_t surfaceIndex = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        while (!mesh.surfaces.isEmpty() && i >= std::size_t(mesh.surfaces[surfaceIndex].firstIndex) +
               mesh.surfaces[surfaceIndex].indexCount) ++surfaceIndex;
        const std::array<std::uint32_t, 3> indices{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        const std::array source{mesh.vertices[indices[0]], mesh.vertices[indices[1]], mesh.vertices[indices[2]]};
        const auto material = mesh.surfaces.isEmpty() ? source[0].material : mesh.surfaces[surfaceIndex].material;
        std::vector<Point> triangle;
        for (unsigned j = 0; j < 3; ++j) {
            const auto p = source[j].position;
            Point point{{p.x, p.y, p.z}, {0, 0, 0}};
            point.weights[j] = 1; triangle.push_back(point);
        }
        const auto coplanar = std::all_of(triangle.begin(), triangle.end(), [&](const auto& p) { return p.position[axis] == plane; });
        const auto bounds = std::minmax({triangle[0].position[tangent], triangle[1].position[tangent], triangle[2].position[tangent]});
        if (!coplanar || bounds.second <= begin || bounds.first >= end || !nondegenerate(source[0], source[1], source[2])) {
            if (!emit(indices, material, i / 3)) return Error::TriangleLimitExceeded;
            continue;
        }
        ++output.affectedTriangles;
        // The complement of a strip is two disjoint closed half-planes.
        for (const bool after : {false, true}) {
            const auto polygon = clipPlane(triangle, tangent, after ? end : begin, after);
            for (std::size_t j = 1; j + 1 < polygon.size(); ++j) {
                const std::array points{polygon[0], polygon[j], polygon[j + 1]};
                const auto area = [&](const auto& a, const auto& b, const auto& c) {
                    return (b[1] - a[1]) * (c[tangent] - a[tangent]) - (b[tangent] - a[tangent]) * (c[1] - a[1]);
                };
                const double exact = area(points[0].position, points[1].position, points[2].position);
                if (exact == 0) continue; // Only mathematical zero-area contacts are omitted.
                const std::array vertices{vertexAt(points[0], source), vertexAt(points[1], source), vertexAt(points[2], source)};
                std::array<D3, 3> rounded;
                for (unsigned k = 0; k < 3; ++k) {
                    const auto p = vertices[k].position; rounded[k] = {p.x, p.y, p.z};
                }
                const double actual = area(rounded[0], rounded[1], rounded[2]);
                if (actual == 0 || (actual > 0) != (exact > 0)) return Error::PrecisionLoss;
                if (output.mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - 3u)
                    return Error::TriangleLimitExceeded;
                const auto base = static_cast<std::uint32_t>(output.mesh.vertices.size());
                for (const auto& v : vertices) output.mesh.vertices.push(v);
                if (!emit({base, base + 1, base + 2}, material, i / 3)) return Error::TriangleLimitExceeded;
            }
        }
    }
    if (output.affectedTriangles == 0) output.mesh = mesh; // Preserve exact no-op layout, including empty surface tables.
    return output;
}
auto stitchTerrainBoundaryTops(const MeshData& mesh, math::Vec3 translation, TerrainBoundaryLine line,
                               std::span<const TerrainBoundaryTopCut> cuts, std::size_t triangleLimit)
    -> Result<TerrainBoundaryStitch, TerrainBoundaryStitchError> {
    using Error = TerrainBoundaryStitchError;
    const auto inRange = [](double x) { return std::isfinite(x) && std::abs(x) <= std::numeric_limits<float>::max(); };
    if (!finite(translation) || (line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !inRange(line.plane) || !inRange(line.begin) || !inRange(line.end) || line.begin >= line.end ||
        mesh.indices.size() % 3 != 0 || mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        cuts.size() > 524292 || triangleLimit == 0 || triangleLimit > 131072) return Error::InvalidData;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return Error::InvalidData;
    for (const auto& v : mesh.vertices) {
        unsigned sum = 0;
        for (auto weight : v.materialWeights) sum += weight;
        if (!finite(v.position) || !finite(v.normal) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y) ||
            sum != 255 || v.ambientOcclusion > 3) return Error::InvalidData;
    }
    std::size_t covered = 0;
    for (const auto& surface : mesh.surfaces) {
        if (surface.firstIndex != covered || surface.indexCount % 3 != 0 ||
            surface.indexCount > mesh.indices.size() - covered) return Error::InvalidData;
        covered += surface.indexCount;
    }
    if (!mesh.surfaces.isEmpty() && covered != mesh.indices.size()) return Error::InvalidData;
    const unsigned axis = line.axis == TerrainBoundaryAxis::X ? 0 : 2, tangent = axis == 0 ? 2 : 0;
    const D3 offset{translation.x, translation.y, translation.z};
    const double plane = line.plane - offset[axis];
    if (!inRange(plane) || double(static_cast<float>(line.plane)) != line.plane ||
        double(static_cast<float>(plane)) != plane || plane + offset[axis] != line.plane) return Error::PrecisionLoss;
    std::vector<TerrainBoundaryTopCut> sorted(cuts.begin(), cuts.end());
    for (const auto& cut : sorted) {
        if (cut.triangleIndex >= mesh.indices.size() / 3 || !inRange(cut.tangent) || !inRange(cut.height) ||
            cut.tangent < line.begin || cut.tangent > line.end) return Error::InvalidData;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.triangleIndex != b.triangleIndex) return a.triangleIndex < b.triangleIndex;
        if (a.tangent != b.tangent) return a.tangent < b.tangent;
        return a.height < b.height;
    });
    TerrainBoundaryStitch output;
    output.mesh.vertices = mesh.vertices;
    const auto emit = [&](std::array<std::uint32_t, 3> indices, std::uint16_t material, std::size_t original) {
        if (output.sourceTriangles.size() == triangleLimit) return false;
        if (output.mesh.surfaces.isEmpty() || output.mesh.surfaces.back().material != material)
            output.mesh.surfaces.push({static_cast<std::uint32_t>(output.mesh.indices.size()), 0, material});
        for (const auto index : indices) output.mesh.indices.push(index);
        output.mesh.surfaces.back().indexCount += 3;
        output.sourceTriangles.push(original);
        return true;
    };
    std::size_t cursor = 0, surfaceIndex = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        while (!mesh.surfaces.isEmpty() && i >= std::size_t(mesh.surfaces[surfaceIndex].firstIndex) +
               mesh.surfaces[surfaceIndex].indexCount) ++surfaceIndex;
        const std::array indices{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        const std::array source{mesh.vertices[indices[0]], mesh.vertices[indices[1]], mesh.vertices[indices[2]]};
        const auto material = mesh.surfaces.isEmpty() ? source[0].material : mesh.surfaces[surfaceIndex].material;
        if (cursor == sorted.size() || sorted[cursor].triangleIndex != i / 3) {
            if (!emit(indices, material, i / 3)) return Error::TriangleLimitExceeded;
            continue;
        }
        std::array<D3, 3> p;
        for (unsigned j = 0; j < 3; ++j) p[j] = {source[j].position.x, source[j].position.y, source[j].position.z};
        const auto upward = [](const D3& a, const D3& b, const D3& c) {
            return (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
        };
        unsigned edge = 3;
        for (unsigned j = 0; j < 3; ++j)
            if (p[j][axis] == plane && p[(j + 1) % 3][axis] == plane && p[(j + 2) % 3][axis] != plane) edge = j;
        if (edge == 3 || upward(p[0], p[1], p[2]) <= 0) return Error::UnsupportedEdge;
        const unsigned next = (edge + 1) % 3, opposite = (edge + 2) % 3;
        const unsigned lo = p[edge][tangent] < p[next][tangent] ? edge : next, hi = lo == edge ? next : edge;
        const double low = p[lo][tangent] + offset[tangent], high = p[hi][tangent] + offset[tangent];
        if (low >= high) return Error::PrecisionLoss;
        std::vector<std::uint32_t> chain{indices[lo]};
        double previous = low;
        while (cursor < sorted.size() && sorted[cursor].triangleIndex == i / 3) {
            const auto& cut = sorted[cursor++];
            if (cut.tangent < low || cut.tangent > high) return Error::InvalidData;
            const double f = (cut.tangent - low) / (high - low);
            const double height = p[lo][1] + f * (p[hi][1] - p[lo][1]) + offset[1];
            if (!inRange(height) || static_cast<float>(height) != static_cast<float>(cut.height)) return Error::PrecisionLoss;
            D3 canonical;
            canonical[axis] = line.plane;
            canonical[tangent] = static_cast<float>(cut.tangent);
            canonical[1] = static_cast<float>(cut.height);
            D3 local;
            for (unsigned j = 0; j < 3; ++j) {
                const double value = canonical[j] - offset[j];
                if (!inRange(value)) return Error::PrecisionLoss;
                local[j] = static_cast<float>(value);
                if (local[j] + offset[j] != canonical[j]) return Error::PrecisionLoss;
            }
            if (cut.tangent == low || cut.tangent == high) {
                if (local != p[cut.tangent == low ? lo : hi]) return Error::PrecisionLoss;
                continue;
            }
            if (cut.tangent == previous) continue; // Repeated canonical point, height already verified.
            const auto prior = output.mesh.vertices[chain.back()].position;
            const double priorT = tangent == 0 ? prior.x : prior.z;
            if (local[tangent] <= priorT || local[tangent] >= p[hi][tangent]) return Error::PrecisionLoss;
            if (output.mesh.vertices.size() == std::numeric_limits<std::uint32_t>::max()) return Error::TriangleLimitExceeded;
            Point point{local, {0, 0, 0}};
            point.weights[lo] = 1 - f; point.weights[hi] = f;
            chain.push_back(static_cast<std::uint32_t>(output.mesh.vertices.size()));
            output.mesh.vertices.push(vertexAt(point, source));
            previous = cut.tangent;
        }
        if (chain.size() == 1) {
            if (!emit(indices, material, i / 3)) return Error::TriangleLimitExceeded;
            continue;
        }
        chain.push_back(indices[hi]);
        ++output.splitTriangles;
        for (std::size_t j = 0; j + 1 < chain.size(); ++j) {
            const std::array face{indices[opposite], chain[lo == edge ? j : j + 1], chain[lo == edge ? j + 1 : j]};
            std::array<D3, 3> vertices;
            for (unsigned k = 0; k < 3; ++k) {
                const auto v = output.mesh.vertices[face[k]].position; vertices[k] = {v.x, v.y, v.z};
            }
            if (upward(vertices[0], vertices[1], vertices[2]) <= 0) return Error::PrecisionLoss;
            if (!emit(face, material, i / 3)) return Error::TriangleLimitExceeded;
        }
    }
    if (output.splitTriangles == 0) output.mesh = mesh;
    return output;
}
} // namespace f4::world
