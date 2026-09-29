#include "engine/world/terrain_boundaries.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

namespace f4::world {
namespace {
using D3 = std::array<double, 3>;
struct Segment {
    double begin, end, beginHeight, endHeight;
    std::size_t source, triangle;
    bool positive;
};
struct Event { double coordinate; std::size_t segment; bool entering; };
auto finite(math::Vec3 p) -> bool {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
auto heightAt(const Segment& s, double t) -> double {
    if (t == s.begin) return s.beginHeight;
    if (t == s.end) return s.endHeight;
    return s.beginHeight + (t - s.begin) / (s.end - s.begin) * (s.endHeight - s.beginHeight);
}
} // namespace

auto compareTerrainBoundaryTops(std::span<const TerrainBoundaryMesh> negative,
                                std::span<const TerrainBoundaryMesh> positive,
                                TerrainBoundaryLine line, std::size_t segmentLimit)
    -> Result<containers::Array<TerrainBoundarySpan>, TerrainBoundaryError> {
    if ((line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !std::isfinite(line.plane) || !std::isfinite(line.begin) || !std::isfinite(line.end) ||
        !std::isfinite(line.end - line.begin) || line.begin >= line.end ||
        segmentLimit == 0 || segmentLimit > 65536) return TerrainBoundaryError::InvalidData;
    const unsigned axis = line.axis == TerrainBoundaryAxis::X ? 0 : 2;
    const unsigned tangent = axis == 0 ? 2 : 0;
    std::vector<Segment> segments;
    for (const bool side : {false, true}) {
        const auto sources = side ? positive : negative;
        for (std::size_t source = 0; source < sources.size(); ++source) {
            const auto& input = sources[source];
            if (!input.mesh || !finite(input.translation) || input.mesh->indices.size() % 3 != 0)
                return TerrainBoundaryError::InvalidData;
            const auto& mesh = *input.mesh;
            for (const auto& v : mesh.vertices) if (!finite(v.position)) return TerrainBoundaryError::InvalidData;
            for (const auto i : mesh.indices) if (i >= mesh.vertices.size()) return TerrainBoundaryError::InvalidData;
            // Intersect in local coordinates, then translate. Shared edges use
            // the same arithmetic regardless of triangle winding/source order.
            const D3 offset{input.translation.x, input.translation.y, input.translation.z};
            const double plane = line.plane - offset[axis];
            for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
                std::array<D3, 3> p;
                for (unsigned i = 0; i < 3; ++i) {
                    const auto v = mesh.vertices[mesh.indices[index + i]].position;
                    p[i] = {v.x, v.y, v.z};
                }
                const double upward = (p[1][2] - p[0][2]) * (p[2][0] - p[0][0]) -
                    (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]);
                if (upward <= 0) continue;
                if (std::none_of(p.begin(), p.end(), [&](auto v) { return side ? v[axis] > plane : v[axis] < plane; })) continue;
                std::vector<std::array<double, 2>> points;
                for (unsigned i = 0; i < 3; ++i) {
                    const auto& a = p[i] < p[(i + 1) % 3] ? p[i] : p[(i + 1) % 3];
                    const auto& b = p[i] < p[(i + 1) % 3] ? p[(i + 1) % 3] : p[i];
                    if (p[i][axis] == plane) points.push_back({p[i][tangent] + offset[tangent], p[i][1] + offset[1]});
                    if ((a[axis] < plane && b[axis] > plane) || (a[axis] > plane && b[axis] < plane)) {
                        const double t = (plane - a[axis]) / (b[axis] - a[axis]);
                        points.push_back({a[tangent] + t * (b[tangent] - a[tangent]) + offset[tangent],
                                          a[1] + t * (b[1] - a[1]) + offset[1]});
                    }
                }
                if (points.size() < 2) continue;
                std::sort(points.begin(), points.end());
                Segment s{points.front()[0], points.back()[0], points.front()[1], points.back()[1], source, index / 3, side};
                const double begin = std::max(s.begin, line.begin), end = std::min(s.end, line.end);
                if (begin >= end) continue;
                const double beginHeight = heightAt(s, begin), endHeight = heightAt(s, end);
                s.begin = begin; s.end = end; s.beginHeight = beginHeight; s.endHeight = endHeight;
                if (segments.size() == segmentLimit) return TerrainBoundaryError::SegmentLimitExceeded;
                segments.push_back(s);
            }
        }
    }
    std::vector<Event> events;
    events.reserve(segments.size() * 2);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        events.push_back({segments[i].begin, i, true});
        events.push_back({segments[i].end, i, false});
    }
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        if (a.coordinate != b.coordinate) return a.coordinate < b.coordinate;
        if (a.entering != b.entering) return a.entering < b.entering;
        return a.segment < b.segment;
    });
    containers::Array<TerrainBoundarySpan> result;
    std::array<std::set<std::size_t>, 2> active;
    std::size_t event = 0;
    double begin = line.begin;
    while (begin < line.end) {
        while (event < events.size() && events[event].coordinate == begin) {
            const auto& e = events[event++];
            auto& set = active[segments[e.segment].positive ? 1 : 0];
            if (e.entering) set.insert(e.segment); else set.erase(e.segment);
        }
        const double end = event < events.size() ? events[event].coordinate : line.end;
        TerrainBoundarySpan span{begin, end, active[0].size(), active[1].size(), {}, {}};
        for (unsigned side = 0; side < 2; ++side) {
            if (active[side].size() != 1) continue;
            const auto& s = segments[*active[side].begin()];
            auto& sample = side == 0 ? span.negative : span.positive;
            sample = TerrainBoundarySample{heightAt(s, begin), heightAt(s, end), s.source, s.triangle};
        }
        result.push(span);
        begin = end;
    }
    return result;
}
} // namespace f4::world
