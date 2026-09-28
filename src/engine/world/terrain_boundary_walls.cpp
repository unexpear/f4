#include "engine/world/terrain_boundary_walls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace f4::world {
namespace {
auto representable(double value) -> bool {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}
auto order(double a, double b) -> int { return (a > b) - (a < b); }
struct Endpoint { double tangent, negative, positive; };
} // namespace

auto meshTerrainBoundaryWalls(std::span<const TerrainBoundarySpan> profile, TerrainBoundaryLine line,
                               std::uint16_t negativeMaterial, std::uint16_t positiveMaterial,
                               std::size_t triangleLimit)
    -> Result<TerrainBoundaryWallBuild, TerrainBoundaryWallError> {
    using Error = TerrainBoundaryWallError;
    if ((line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !representable(line.plane) || !representable(line.begin) || !representable(line.end) ||
        line.begin >= line.end || triangleLimit == 0 || triangleLimit > 131072 || profile.size() > 131073)
        return Error::InvalidData;
    if (double(static_cast<float>(line.plane)) != line.plane) return Error::PrecisionLoss;
    if (profile.empty()) return Error::IncompleteProfile;
    // Validate the entire partition before generating any geometry.
    double cursor = line.begin;
    for (const auto& span : profile) {
        if (!representable(span.begin) || !representable(span.end) || span.begin != cursor ||
            span.begin >= span.end || span.end > line.end) return Error::InvalidData;
        if (span.negativeCount != 1 || span.positiveCount != 1 || !span.negative || !span.positive)
            return Error::IncompleteProfile;
        for (const auto& sample : {*span.negative, *span.positive})
            if (!representable(sample.beginHeight) || !representable(sample.endHeight)) return Error::InvalidData;
        cursor = span.end;
    }
    if (cursor != line.end) return Error::IncompleteProfile;
    TerrainBoundaryWallBuild output;
    const bool xAxis = line.axis == TerrainBoundaryAxis::X;
    const auto append = [&](Endpoint a, Endpoint b, std::size_t spanIndex) -> std::optional<Error> {
        const float begin = static_cast<float>(a.tangent), end = static_cast<float>(b.tangent);
        if (begin >= end) return Error::PrecisionLoss;
        const float an = static_cast<float>(a.negative), ap = static_cast<float>(a.positive);
        const float bn = static_cast<float>(b.negative), bp = static_cast<float>(b.positive);
        if (order(an, ap) != order(a.negative, a.positive) || order(bn, bp) != order(b.negative, b.positive))
            return Error::PrecisionLoss;
        output.topEdges.push({spanIndex, false, a.tangent, b.tangent, a.negative, b.negative});
        output.topEdges.push({spanIndex, true, a.tangent, b.tangent, a.positive, b.positive});
        if (an == ap && bn == bp) return {};
        const bool positive = ap > an || bp > bn;
        const auto material = positive ? positiveMaterial : negativeMaterial;
        const float sign = positive ? -1.f : 1.f;
        const math::Vec3 normal = xAxis ? math::Vec3{sign, 0, 0} : math::Vec3{0, 0, sign};
        const auto vertex = [&](float t, float height) {
            TerrainVertex v;
            const auto plane = static_cast<float>(line.plane);
            v.position = xAxis ? math::Vec3{plane, height, t} : math::Vec3{t, height, plane};
            v.normal = normal; v.uv = {t, height}; v.material = material;
            return v;
        };
        const std::array vertices{vertex(begin, std::min(an, ap)), vertex(begin, std::max(an, ap)),
                                  vertex(end, std::max(bn, bp)), vertex(end, std::min(bn, bp))};
        // This quad order points +X / -Z. Reverse for the opposite exposed side.
        const bool reverse = xAxis ? positive : !positive;
        const auto triangle = [&](unsigned i, unsigned j, unsigned k) -> bool {
            if (output.triangles.size() == triangleLimit) return false;
            if (reverse) std::swap(j, k);
            const auto base = static_cast<std::uint32_t>(output.mesh.vertices.size());
            output.mesh.vertices.push(vertices[i]); output.mesh.vertices.push(vertices[j]); output.mesh.vertices.push(vertices[k]);
            output.mesh.indices.push(base); output.mesh.indices.push(base + 1); output.mesh.indices.push(base + 2);
            if (output.mesh.surfaces.isEmpty() || output.mesh.surfaces.back().material != material)
                output.mesh.surfaces.push({base, 0, material});
            output.mesh.surfaces.back().indexCount += 3;
            output.triangles.push({spanIndex, positive});
            return true;
        };
        if (an != ap && !triangle(0, 1, 2)) return Error::TriangleLimitExceeded;
        if (bn != bp && !triangle(0, 2, 3)) return Error::TriangleLimitExceeded;
        return {};
    };
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const auto& s = profile[i];
        Endpoint a{s.begin, s.negative->beginHeight, s.positive->beginHeight};
        Endpoint b{s.end, s.negative->endHeight, s.positive->endHeight};
        const double first = a.negative - a.positive, last = b.negative - b.positive;
        if ((first < 0 && last > 0) || (first > 0 && last < 0)) {
            const double fraction = std::abs(first) / (std::abs(first) + std::abs(last));
            const double t = std::lerp(a.tangent, b.tangent, fraction);
            const double height = std::lerp(a.negative, b.negative, fraction);
            // Both triangles reuse exactly one crossing height and position.
            const Endpoint crossing{t, height, height};
            if (auto error = append(a, crossing, i)) return *error;
            if (auto error = append(crossing, b, i)) return *error;
        } else if (auto error = append(a, b, i)) return *error;
    }
    return output;
}

} // namespace f4::world
