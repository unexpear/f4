#include "engine/world/boundary_replacement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <vector>

#include "engine/world/terrain_boundary_walls.h"
#include "engine/world/terrain_regions.h"

namespace f4::world {
auto buildTerrainBoundaryReplacement(std::span<const TerrainBoundaryMesh> negative,
                                     std::span<const TerrainBoundaryMesh> positive, TerrainBoundaryLine line,
                                     BoundaryReplacementLimits limits, std::uint16_t negativeMaterial,
                                     std::uint16_t positiveMaterial)
    -> Result<TerrainBoundaryReplacement, BoundaryReplacementError> {
    using Error = BoundaryReplacementError;
    if (limits.maxTriangles > 1000000) return Error::InvalidData;
    if (negative.size() > limits.maxSources || positive.size() > limits.maxSources - negative.size() ||
        limits.maxTriangles == 0) return Error::LimitExceeded;
    const auto profile = compareTerrainBoundaryTops(negative, positive, line);
    if (!profile) return profile.error() == TerrainBoundaryError::SegmentLimitExceeded ? Error::LimitExceeded : Error::InvalidData;
    const auto walls = meshTerrainBoundaryWalls({profile.value().begin(), profile.value().size()}, line,
        negativeMaterial, positiveMaterial, std::min<std::size_t>(limits.maxTriangles, 131072));
    if (!walls) {
        switch (walls.error()) {
        case TerrainBoundaryWallError::IncompleteProfile: return Error::IncompleteProfile;
        case TerrainBoundaryWallError::PrecisionLoss: return Error::PrecisionLoss;
        case TerrainBoundaryWallError::TriangleLimitExceeded: return Error::LimitExceeded;
        default: return Error::InvalidData;
        }
    }
    TerrainBoundaryReplacement output;
    std::size_t bytes = 0;
    const auto append = [&](const std::array<TerrainVertex, 3>& source, math::Vec3 origin,
                            std::uint16_t material, BoundaryReplacementTriangle provenance) -> std::optional<Error> {
        const bool range = output.mesh.surfaces.isEmpty() || output.mesh.surfaces.back().material != material ||
            output.triangles[output.triangles.size() - 1].positive != provenance.positive;
        const std::size_t extra = 3 * sizeof(TerrainVertex) + 3 * sizeof(std::uint32_t) + sizeof(BoundaryReplacementTriangle) +
            (range ? sizeof(TerrainSurface) : 0);
        if (output.triangles.size() == limits.maxTriangles || extra > limits.maxBytes - bytes) return Error::LimitExceeded;
        auto vertices = source;
        for (auto& v : vertices) {
            const std::array<double, 3> world{double(v.position.x) + origin.x, double(v.position.y) + origin.y, double(v.position.z) + origin.z};
            for (const auto coordinate : world)
                if (!std::isfinite(coordinate) || std::abs(coordinate) > std::numeric_limits<float>::max() ||
                    double(static_cast<float>(coordinate)) != coordinate) return Error::PrecisionLoss;
            v.position = {static_cast<float>(world[0]), static_cast<float>(world[1]), static_cast<float>(world[2])};
        }
        const auto base = static_cast<std::uint32_t>(output.mesh.vertices.size());
        for (const auto& v : vertices) output.mesh.vertices.push(v);
        for (const auto i : {base, base + 1, base + 2}) output.mesh.indices.push(i);
        if (range) output.mesh.surfaces.push({base, 0, material});
        output.mesh.surfaces.back().indexCount += 3;
        output.triangles.push(provenance); bytes += extra;
        return {};
    };
    for (const bool side : {false, true}) {
        const auto sources = side ? positive : negative;
        for (std::size_t s = 0; s < sources.size(); ++s) {
            const auto& source = sources[s];
            auto trimmed = trimTerrainBoundaryFaces(*source.mesh, source.translation, line);
            if (!trimmed) {
                if (trimmed.error() == TerrainBoundaryTrimError::TriangleLimitExceeded) return Error::LimitExceeded;
                if (trimmed.error() == TerrainBoundaryTrimError::PrecisionLoss) return Error::PrecisionLoss;
                return Error::TrimRejected;
            }
            struct Mapping { std::size_t index = 0, count = 0; };
            std::vector<Mapping> inverse(source.mesh->indices.size() / 3);
            for (std::size_t i = 0; i < trimmed.value().sourceTriangles.size(); ++i) {
                auto& entry = inverse[trimmed.value().sourceTriangles[i]]; entry.index = i; ++entry.count;
            }
            std::vector<TerrainBoundaryTopCut> cuts;
            for (const auto& edge : walls.value().topEdges) {
                if (edge.positive != side) continue;
                const auto& span = profile.value()[edge.spanIndex];
                const auto& sample = side ? *span.positive : *span.negative;
                if (sample.sourceIndex != s) continue;
                const auto& mapping = inverse[sample.triangleIndex];
                if (mapping.count != 1) return Error::TrimRejected;
                cuts.push_back({mapping.index, edge.begin, edge.beginHeight});
                cuts.push_back({mapping.index, edge.end, edge.endHeight});
            }
            auto stitched = stitchTerrainBoundaryTops(trimmed.value().mesh, source.translation, line, cuts);
            if (!stitched) {
                if (stitched.error() == TerrainBoundaryStitchError::TriangleLimitExceeded) return Error::LimitExceeded;
                if (stitched.error() == TerrainBoundaryStitchError::PrecisionLoss) return Error::PrecisionLoss;
                return Error::StitchRejected;
            }
            const auto& mesh = stitched.value().mesh;
            std::size_t surface = 0;
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                while (!mesh.surfaces.isEmpty() && i >= std::size_t(mesh.surfaces[surface].firstIndex) + mesh.surfaces[surface].indexCount) ++surface;
                const std::array vertices{mesh.vertices[mesh.indices[i]], mesh.vertices[mesh.indices[i + 1]], mesh.vertices[mesh.indices[i + 2]]};
                const auto material = mesh.surfaces.isEmpty() ? vertices[0].material : mesh.surfaces[surface].material;
                const auto original = trimmed.value().sourceTriangles[stitched.value().sourceTriangles[i / 3]];
                if (auto error = append(vertices, source.translation, material, {false, side, s, original})) return *error;
            }
        }
    }
    const auto& mesh = walls.value().mesh;
    for (std::size_t i = 0; i < walls.value().triangles.size(); ++i) {
        const auto& stamp = walls.value().triangles[i];
        const auto& span = profile.value()[stamp.spanIndex];
        const auto& sample = stamp.positiveOwner ? *span.positive : *span.negative;
        const std::array vertices{mesh.vertices[mesh.indices[i * 3]], mesh.vertices[mesh.indices[i * 3 + 1]], mesh.vertices[mesh.indices[i * 3 + 2]]};
        if (auto error = append(vertices, {}, vertices[0].material, {true, stamp.positiveOwner, sample.sourceIndex, sample.triangleIndex})) return *error;
    }
    return output;
}
auto composeTerrainBoundaryReplacements(std::span<const TerrainBoundaryMesh> sources,
                                         std::span<const TerrainBoundaryRequest> boundaries,
                                         BoundaryCompositionLimits limits)
    -> Result<TerrainBoundaryComposition, BoundaryReplacementError> {
    using Error = BoundaryReplacementError;
    if (sources.empty() || boundaries.empty() || limits.maxBoundaries == 0 || limits.maxBoundaries > 256 ||
        limits.mesh.maxTriangles > 1000000) return Error::InvalidData;
    if (sources.size() > limits.mesh.maxSources || boundaries.size() > limits.maxBoundaries ||
        limits.mesh.maxTriangles == 0) return Error::LimitExceeded;
    std::vector<bool> used(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& s = sources[i];
        if (!s.mesh || !std::isfinite(s.translation.x) || !std::isfinite(s.translation.y) ||
            !std::isfinite(s.translation.z)) return Error::InvalidData;
        for (std::size_t j = 0; j < i; ++j)
            if (s.mesh == sources[j].mesh && s.translation.x == sources[j].translation.x &&
                s.translation.y == sources[j].translation.y && s.translation.z == sources[j].translation.z)
                return Error::InvalidData;
    }
    const auto conflicts = [](TerrainBoundaryLine a, TerrainBoundaryLine b) {
        if (a.axis == b.axis)
            return a.plane == b.plane && std::max(a.begin, b.begin) < std::min(a.end, b.end);
        const bool intersects = a.plane >= b.begin && a.plane <= b.end && b.plane >= a.begin && b.plane <= a.end;
        const bool endpoints = (a.plane == b.begin || a.plane == b.end) && (b.plane == a.begin || b.plane == a.end);
        return intersects && !endpoints;
    };
    struct Arm { std::size_t boundary; unsigned endpoint, direction; }; // E,N,W,S = 0,1,2,3.
    std::map<std::pair<double, double>, std::vector<Arm>> endpoints;
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
        const auto& b = boundaries[i];
        const auto line = b.line;
        if ((line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
            !std::isfinite(line.plane) || !std::isfinite(line.begin) || !std::isfinite(line.end) ||
            !std::isfinite(line.end - line.begin) || line.begin >= line.end) return Error::InvalidData;
        std::vector<bool> member(sources.size());
        for (const auto side : {b.negative, b.positive}) {
            for (const auto s : side) {
                if (s >= sources.size() || member[s]) return Error::InvalidData;
                member[s] = true; used[s] = true;
            }
        }
        for (std::size_t j = 0; j < i; ++j)
            if (conflicts(line, boundaries[j].line)) return Error::ConflictingBoundaries;
        const bool xAxis = line.axis == TerrainBoundaryAxis::X;
        for (const unsigned end : {0u, 1u}) {
            const double tangent = end ? line.end : line.begin;
            const auto point = xAxis ? std::pair{line.plane, tangent} : std::pair{tangent, line.plane};
            endpoints[point].push_back({i, end, xAxis ? (end ? 3u : 1u) : (end ? 2u : 0u)});
        }
    }
    if (std::find(used.begin(), used.end(), false) != used.end()) return Error::InvalidData;
    std::vector<bool> needsEndpoints(boundaries.size());
    for (const auto& [point, arms] : endpoints) {
        if (arms.size() < 2) continue;
        for (const auto coordinate : {point.first, point.second})
            if (std::abs(coordinate) > std::numeric_limits<float>::max() || double(float(coordinate)) != coordinate)
                return Error::PrecisionLoss;
        for (const auto& arm : arms) needsEndpoints[arm.boundary] = true;
    }
    // [request][begin/end][negative/positive], sampled from that pass's source tops.
    std::vector<std::array<std::array<double, 2>, 2>> endpointHeights(boundaries.size());
    std::vector<std::size_t> order(boundaries.size());
    std::iota(order.begin(), order.end(), 0);
    const auto key = [&](std::size_t i) {
        const auto line = boundaries[i].line;
        return std::tuple{line.axis, line.plane, line.begin, line.end};
    };
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return key(a) < key(b); });

    struct WorkingSource {
        MeshData mesh;
        math::Vec3 origin;
        std::vector<std::size_t> originals;
    };
    struct Wall {
        std::array<TerrainVertex, 3> vertices;
        std::uint16_t material;
        BoundaryCompositionTriangle provenance;
    };
    // Fixed-size storage keeps mesh views stable during a builder call. Replacement
    // happens only after its entire result has been remapped to original triangles.
    std::vector<WorkingSource> working(sources.size());
    for (std::size_t s = 0; s < sources.size(); ++s) {
        working[s].mesh = *sources[s].mesh; working[s].origin = sources[s].translation;
        working[s].originals.resize(sources[s].mesh->indices.size() / 3);
        std::iota(working[s].originals.begin(), working[s].originals.end(), 0);
    }
    std::vector<Wall> walls;
    const auto appendMesh = [](MeshData& mesh, const std::array<TerrainVertex, 3>& vertices,
                               std::uint16_t material, bool newRange = false) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        if (newRange || mesh.surfaces.isEmpty() || mesh.surfaces.back().material != material)
            mesh.surfaces.push({base, 0, material});
        for (const auto& v : vertices) mesh.vertices.push(v);
        for (const auto i : {base, base + 1, base + 2}) mesh.indices.push(i);
        mesh.surfaces.back().indexCount += 3;
    };
    for (const auto b : order) {
        const auto& request = boundaries[b];
        std::array<std::vector<TerrainBoundaryMesh>, 2> views;
        for (const bool side : {false, true})
            for (const auto s : side ? request.positive : request.negative)
                views[side].push_back({&working[s].mesh, working[s].origin});
        const auto built = buildTerrainBoundaryReplacement(views[0], views[1], request.line, limits.mesh,
            request.negativeMaterial, request.positiveMaterial);
        if (!built) return built.error();
        if (needsEndpoints[b]) {
            const auto profile = compareTerrainBoundaryTops(views[0], views[1], request.line);
            if (!profile || profile.value().isEmpty()) return Error::InvalidData;
            for (const unsigned end : {0u, 1u}) {
                const auto& span = profile.value()[end ? profile.value().size() - 1 : 0];
                if (!span.negative || !span.positive) return Error::IncompleteProfile;
                endpointHeights[b][end] = end ? std::array{span.negative->endHeight, span.positive->endHeight} :
                    std::array{span.negative->beginHeight, span.positive->beginHeight};
            }
        }
        std::vector<WorkingSource> next(sources.size());
        const auto& result = built.value();
        std::size_t surface = 0;
        for (std::size_t i = 0; i < result.triangles.size(); ++i) {
            const auto& stamp = result.triangles[i];
            const auto s = (stamp.positive ? request.positive : request.negative)[stamp.sourceIndex];
            const auto original = working[s].originals[stamp.sourceTriangle];
            while (i * 3 >= std::size_t(result.mesh.surfaces[surface].firstIndex) + result.mesh.surfaces[surface].indexCount) ++surface;
            const auto material = result.mesh.surfaces[surface].material;
            const std::array vertices{result.mesh.vertices[result.mesh.indices[i * 3]],
                result.mesh.vertices[result.mesh.indices[i * 3 + 1]], result.mesh.vertices[result.mesh.indices[i * 3 + 2]]};
            if (stamp.wall) {
                if (walls.size() == limits.mesh.maxTriangles) return Error::LimitExceeded;
                walls.push_back({vertices, material, {true, stamp.positive, s, original, b}});
            } else {
                appendMesh(next[s].mesh, vertices, material);
                next[s].originals.push_back(original);
            }
        }
        for (const auto side : {request.negative, request.positive})
            for (const auto s : side) working[s] = std::move(next[s]);
    }
    struct Junction { float x, z; std::vector<float> heights; };
    std::vector<Junction> junctions;
    for (const auto& [point, arms] : endpoints) {
        if (arms.size() < 2) continue;
        // Quadrants NE,NW,SW,SE. Missing arms imply a continuous column height
        // between their two sectors, not permission to leave an unbuilt wall.
        constexpr std::array<unsigned, 4> negativeSector{3, 1, 2, 2}, positiveSector{0, 0, 1, 3};
        std::array<std::optional<double>, 4> sectors;
        std::array<bool, 4> present{};
        const auto assign = [&](unsigned sector, double height) {
            if (sectors[sector] && *sectors[sector] != height) return false;
            sectors[sector] = height; return true;
        };
        for (const auto& arm : arms) {
            if (present[arm.direction]) return Error::ConflictingBoundaries;
            present[arm.direction] = true;
            const auto h = endpointHeights[arm.boundary][arm.endpoint];
            if (!assign(negativeSector[arm.direction], h[0]) || !assign(positiveSector[arm.direction], h[1]))
                return Error::JunctionRejected;
        }
        for (unsigned pass = 0; pass < 4; ++pass) {
            for (unsigned arm = 0; arm < 4; ++arm) {
                if (present[arm]) continue;
                const auto n = negativeSector[arm], p = positiveSector[arm];
                if (sectors[n] && !assign(p, *sectors[n])) return Error::JunctionRejected;
                if (sectors[p] && !assign(n, *sectors[p])) return Error::JunctionRejected;
            }
        }
        std::vector<double> heights;
        for (const auto& h : sectors) {
            if (!h) return Error::JunctionRejected;
            heights.push_back(*h);
        }
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
        Junction junction{float(point.first), float(point.second), {}};
        for (const auto h : heights) {
            if (!std::isfinite(h) || std::abs(h) > std::numeric_limits<float>::max() ||
                (!junction.heights.empty() && junction.heights.back() >= float(h))) return Error::PrecisionLoss;
            junction.heights.push_back(float(h));
        }
        for (std::size_t i = 1; i < heights.size(); ++i) {
            const auto mid = (heights[i - 1] + heights[i]) * 0.5;
            unsigned changes = 0;
            for (unsigned q = 0; q < 4; ++q) changes += (*sectors[q] > mid) != (*sectors[(q + 1) % 4] > mid);
            if (changes > 2) return Error::JunctionRejected;
        }
        junctions.push_back(std::move(junction));
    }
    if (!junctions.empty()) {
        std::vector<Wall> split;
        const auto push = [&](const Wall& wall) {
            if (split.size() == limits.mesh.maxTriangles) return false;
            split.push_back(wall); return true;
        };
        for (const auto& wall : walls) {
            bool divided = false;
            for (unsigned e = 0; e < 3 && !divided; ++e) {
                const auto& a = wall.vertices[e]; const auto& b = wall.vertices[(e + 1) % 3];
                if (a.position.x != b.position.x || a.position.z != b.position.z || a.position.y == b.position.y) continue;
                const auto it = std::find_if(junctions.begin(), junctions.end(), [&](const auto& j) {
                    return j.x == a.position.x && j.z == a.position.z;
                });
                if (it == junctions.end()) continue;
                std::vector<float> cuts;
                for (const auto h : it->heights)
                    if (h > std::min(a.position.y, b.position.y) && h < std::max(a.position.y, b.position.y)) cuts.push_back(h);
                if (cuts.empty()) continue;
                if (a.position.y > b.position.y) std::reverse(cuts.begin(), cuts.end());
                auto previous = a;
                cuts.push_back(b.position.y);
                for (const auto h : cuts) {
                    // Generated walls have constant categorical attributes/normals
                    // and UV=(world tangent, world height); this is not a generic mesh split.
                    auto next = a; next.position.y = h; next.uv.y = h;
                    if (h == b.position.y) next = b;
                    if (!push({{previous, next, wall.vertices[(e + 2) % 3]}, wall.material, wall.provenance}))
                        return Error::LimitExceeded;
                    previous = next;
                }
                divided = true;
            }
            if (!divided && !push(wall)) return Error::LimitExceeded;
        }
        walls = std::move(split);
    }
    TerrainBoundaryComposition output;
    std::size_t bytes = 0;
    const auto append = [&](const std::array<TerrainVertex, 3>& vertices, std::uint16_t material,
                            BoundaryCompositionTriangle provenance) -> bool {
        const bool range = output.mesh.surfaces.isEmpty() || output.mesh.surfaces.back().material != material ||
            output.triangles.back().sourceIndex != provenance.sourceIndex ||
            output.triangles.back().wall != provenance.wall ||
            output.triangles.back().boundaryIndex != provenance.boundaryIndex;
        const auto extra = 3 * sizeof(TerrainVertex) + 3 * sizeof(std::uint32_t) + sizeof(BoundaryCompositionTriangle) +
            (range ? sizeof(TerrainSurface) : 0);
        if (output.triangles.size() == limits.mesh.maxTriangles || extra > limits.mesh.maxBytes - bytes) return false;
        appendMesh(output.mesh, vertices, material, range);
        output.triangles.push(provenance); bytes += extra;
        return true;
    };
    for (std::size_t s = 0; s < working.size(); ++s) {
        const auto& source = working[s];
        std::size_t surface = 0;
        for (std::size_t i = 0; i < source.originals.size(); ++i) {
            while (i * 3 >= std::size_t(source.mesh.surfaces[surface].firstIndex) + source.mesh.surfaces[surface].indexCount) ++surface;
            const std::array vertices{source.mesh.vertices[source.mesh.indices[i * 3]],
                source.mesh.vertices[source.mesh.indices[i * 3 + 1]], source.mesh.vertices[source.mesh.indices[i * 3 + 2]]};
            if (!append(vertices, source.mesh.surfaces[surface].material, {false, false, s, source.originals[i]}))
                return Error::LimitExceeded;
        }
    }
    for (const auto& wall : walls)
        if (!append(wall.vertices, wall.material, wall.provenance)) return Error::LimitExceeded;
    return output;
}
} // namespace f4::world
