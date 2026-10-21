#include "engine/world/terrain_solid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace f4::world {
namespace {
auto representable(double value) -> bool {
    return std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}
auto order(double a, double b) -> int { return (a > b) - (a < b); }

struct Column {
    PolicyKind policy = PolicyKind::Height;
    double top = 0;
    double floor = 0;
    bool finiteFloor = false;
};

// Policy and floor only. The top is applied later so a boundary sample can
// use the profile height instead of a placeholder.
auto prepareColumn(PolicyKind policy, std::optional<double> floor) -> Result<Column, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (policy == PolicyKind::Voxel) return Error::UnsupportedVoxel;
    if (policy == PolicyKind::Height) {
        if (floor.has_value()) return Error::InvalidData;
        return Column{policy, 0, 0, false};
    }
    if (policy != PolicyKind::Hex || !floor || !representable(*floor)) return Error::InvalidData;
    return Column{policy, 0, *floor, true};
}

auto applyTop(Column& column, double top) -> std::optional<TerrainSolidError> {
    if (!representable(top)) return TerrainSolidError::InvalidData;
    column.top = top;
    if (column.finiteFloor && !(top > column.floor)) return TerrainSolidError::HexFloorInversion;
    return {};
}

auto contains(const Column& column, double height) -> bool {
    if (!(height <= column.top)) return false;
    return !column.finiteFloor || height >= column.floor;
}

auto classifyColumns(std::span<const Column> columns, double height) -> Result<TerrainSolidSample, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (!std::isfinite(height) || columns.empty()) return Error::InvalidData;
    if (columns.size() == 4) {
        std::vector<double> tops;
        tops.reserve(4);
        for (const auto& column : columns) tops.push_back(column.top);
        std::sort(tops.begin(), tops.end());
        tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
        float previous = 0;
        for (std::size_t i = 0; i < tops.size(); ++i) {
            const auto narrowed = static_cast<float>(tops[i]);
            if (i > 0 && previous >= narrowed) return Error::PrecisionLoss;
            previous = narrowed;
        }
        const auto changes = [&](double sample) {
            unsigned count = 0;
            bool inside[4];
            for (int i = 0; i < 4; ++i) inside[i] = contains(columns[static_cast<std::size_t>(i)], sample);
            for (int i = 0; i < 4; ++i) count += inside[i] != inside[(i + 1) % 4];
            return count;
        };
        // More than two solid/empty transitions around the ring. One contiguous
        // solid arc has two; three or four means the corner is nonmanifold.
        for (std::size_t i = 1; i < tops.size(); ++i) {
            if (changes((tops[i - 1] + tops[i]) * 0.5) > 2) return Error::NonmanifoldCorner;
        }
        if (changes(height) > 2) return Error::NonmanifoldCorner;
    }

    // Equal tops keep the first containing index. That matches the canonical
    // sector only because columns are ordered negative then positive, or
    // NE, NW, SW, SE. Reordering the span would silently change ties.
    int owner = -1;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!contains(columns[i], height)) continue;
        if (owner < 0 || columns[i].top > columns[static_cast<std::size_t>(owner)].top) owner = static_cast<int>(i);
    }
    if (owner < 0) return TerrainSolidSample{};
    double lowestOther = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (static_cast<int>(i) == owner) continue;
        if (columns[i].top < columns[static_cast<std::size_t>(owner)].top)
            lowestOther = std::min(lowestOther, columns[i].top);
    }
    const auto& chosen = columns[static_cast<std::size_t>(owner)];
    TerrainSolidSample sample;
    sample.solid = true;
    sample.policy = chosen.policy;
    sample.sector = static_cast<std::uint8_t>(owner);
    sample.wallBand = lowestOther != std::numeric_limits<double>::infinity() && height >= lowestOther &&
                      height <= chosen.top;
    return sample;
}

struct Endpoint { double tangent, negative, positive; };

auto pieceCollapses(Endpoint a, Endpoint b) -> bool {
    const float begin = static_cast<float>(a.tangent), end = static_cast<float>(b.tangent);
    if (begin >= end) return true;
    const float an = static_cast<float>(a.negative), ap = static_cast<float>(a.positive);
    const float bn = static_cast<float>(b.negative), bp = static_cast<float>(b.positive);
    return order(an, ap) != order(a.negative, a.positive) || order(bn, bp) != order(b.negative, b.positive);
}

auto profileError(std::span<const TerrainBoundarySpan> profile, TerrainBoundaryLine line) -> std::optional<TerrainSolidError> {
    using Error = TerrainSolidError;
    if ((line.axis != TerrainBoundaryAxis::X && line.axis != TerrainBoundaryAxis::Z) ||
        !representable(line.plane) || !representable(line.begin) || !representable(line.end) ||
        line.begin >= line.end || profile.size() > 131073)
        return Error::InvalidData;
    if (static_cast<double>(static_cast<float>(line.plane)) != line.plane) return Error::PrecisionLoss;
    if (profile.empty()) return Error::IncompleteProfile;
    double cursor = line.begin;
    for (const auto& span : profile) {
        if (!representable(span.begin) || !representable(span.end) || span.begin != cursor ||
            span.begin >= span.end || span.end > line.end)
            return Error::InvalidData;
        if (span.negativeCount != 1 || span.positiveCount != 1 || !span.negative || !span.positive)
            return Error::IncompleteProfile;
        for (const auto& sample : {*span.negative, *span.positive})
            if (!representable(sample.beginHeight) || !representable(sample.endHeight)) return Error::InvalidData;
        cursor = span.end;
    }
    if (cursor != line.end) return Error::IncompleteProfile;
    for (const auto& span : profile) {
        Endpoint a{span.begin, span.negative->beginHeight, span.positive->beginHeight};
        Endpoint b{span.end, span.negative->endHeight, span.positive->endHeight};
        const double first = a.negative - a.positive, last = b.negative - b.positive;
        if ((first < 0 && last > 0) || (first > 0 && last < 0)) {
            const double fraction = std::abs(first) / (std::abs(first) + std::abs(last));
            const Endpoint crossing{std::lerp(a.tangent, b.tangent, fraction),
                                    std::lerp(a.negative, b.negative, fraction),
                                    std::lerp(a.positive, b.positive, fraction)};
            if (pieceCollapses(a, crossing) || pieceCollapses(crossing, b)) return Error::PrecisionLoss;
        } else if (pieceCollapses(a, b)) return Error::PrecisionLoss;
    }
    return {};
}

auto heightAt(double t0, double h0, double t1, double h1, double t) -> double {
    if (t == t0) return h0;
    if (t == t1) return h1;
    return std::lerp(h0, h1, (t - t0) / (t1 - t0));
}

constexpr unsigned kNegativeSector[4] = {3, 1, 2, 2};
constexpr unsigned kPositiveSector[4] = {0, 0, 1, 3};
} // namespace

auto classifyTerrainColumn(TerrainSolidColumn column, double height) -> Result<TerrainSolidSample, TerrainSolidError> {
    auto built = prepareColumn(column.policy, column.floor);
    if (!built) return built.error();
    if (auto error = applyTop(built.value(), column.top)) return *error;
    const Column columns[] = {built.value()};
    return classifyColumns(std::span<const Column>(columns, 1), height);
}

auto classifyTerrainBoundarySolid(const TerrainBoundarySolidQuery& query)
    -> Result<TerrainSolidSample, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (!std::isfinite(query.height)) return Error::InvalidData;
    auto negative = prepareColumn(query.negative, query.negativeFloor);
    auto positive = prepareColumn(query.positive, query.positiveFloor);
    if (!negative) return negative.error();
    if (!positive) return positive.error();
    if (auto error = profileError(query.profile, query.line)) return *error;
    if (query.tangent < query.line.begin || query.tangent > query.line.end) return Error::InvalidData;
    for (std::size_t i = 0; i + 1 < query.profile.size(); ++i) {
        if (query.tangent != query.profile[i].end) continue;
        const auto& left = query.profile[i];
        const auto& right = query.profile[i + 1];
        if (left.negative->endHeight != right.negative->beginHeight ||
            left.positive->endHeight != right.positive->beginHeight)
            return Error::DiscontinuousHeight;
    }
    const TerrainBoundarySpan* span = nullptr;
    for (const auto& candidate : query.profile) {
        if (query.tangent >= candidate.begin && query.tangent <= candidate.end) {
            span = &candidate;
            break;
        }
    }
    if (span == nullptr) return Error::IncompleteProfile;
    const double negativeTop = heightAt(span->begin, span->negative->beginHeight, span->end,
                                         span->negative->endHeight, query.tangent);
    const double positiveTop = heightAt(span->begin, span->positive->beginHeight, span->end,
                                         span->positive->endHeight, query.tangent);
    if (auto error = applyTop(negative.value(), negativeTop)) return *error;
    if (auto error = applyTop(positive.value(), positiveTop)) return *error;
    const Column columns[] = {negative.value(), positive.value()};
    return classifyColumns(std::span<const Column>(columns, 2), query.height);
}

auto classifyTerrainCornerSolid(std::span<const TerrainSolidColumn, 4> sectors, double height)
    -> Result<TerrainSolidSample, TerrainSolidError> {
    Column columns[4];
    for (std::size_t i = 0; i < 4; ++i) {
        auto built = prepareColumn(sectors[i].policy, sectors[i].floor);
        if (!built) return built.error();
        if (auto error = applyTop(built.value(), sectors[i].top)) return *error;
        columns[i] = built.value();
    }
    return classifyColumns(std::span<const Column>(columns, 4), height);
}

auto resolveTerrainCornerHeights(std::span<const TerrainCornerArm> arms)
    -> Result<std::array<double, 4>, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (arms.empty() || arms.size() > 4) return Error::InvalidData;
    std::array<std::optional<double>, 4> sectors;
    std::array<bool, 4> present{};
    const auto assign = [&](unsigned sector, double value) {
        if (sectors[sector] && *sectors[sector] != value) return false;
        sectors[sector] = value;
        return true;
    };
    for (const auto& arm : arms) {
        if (arm.direction > 3 || present[arm.direction]) return Error::InvalidData;
        if (!representable(arm.negativeHeight) || !representable(arm.positiveHeight)) return Error::InvalidData;
        present[arm.direction] = true;
        if (!assign(kNegativeSector[arm.direction], arm.negativeHeight) ||
            !assign(kPositiveSector[arm.direction], arm.positiveHeight))
            return Error::InconsistentSector;
    }
    for (unsigned pass = 0; pass < 4; ++pass) {
        for (unsigned arm = 0; arm < 4; ++arm) {
            if (present[arm]) continue;
            const auto negative = kNegativeSector[arm], positive = kPositiveSector[arm];
            if (sectors[negative] && !assign(positive, *sectors[negative])) return Error::InconsistentSector;
            if (sectors[positive] && !assign(negative, *sectors[positive])) return Error::InconsistentSector;
        }
    }
    std::array<double, 4> resolved{};
    std::vector<double> distinct;
    for (unsigned i = 0; i < 4; ++i) {
        if (!sectors[i]) return Error::InconsistentSector;
        resolved[i] = *sectors[i];
        distinct.push_back(*sectors[i]);
    }
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    float previous = 0;
    for (std::size_t i = 0; i < distinct.size(); ++i) {
        const auto narrowed = static_cast<float>(distinct[i]);
        if (i > 0 && previous >= narrowed) return Error::PrecisionLoss;
        previous = narrowed;
    }
    return resolved;
}

auto sampleUpwardTriangleHeight(const TerrainTopSample& a, const TerrainTopSample& b, const TerrainTopSample& c,
                                double x, double z) -> Result<double, TerrainSolidError> {
    using Error = TerrainSolidError;
    for (const auto& sample : {a, b, c}) {
        if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z)) return Error::InvalidData;
    }
    if (!std::isfinite(x) || !std::isfinite(z)) return Error::InvalidData;
    const auto signedArea = [](const TerrainTopSample& p, const TerrainTopSample& q, const TerrainTopSample& r) {
        return (q.z - p.z) * (r.x - p.x) - (q.x - p.x) * (r.z - p.z);
    };
    const double area = signedArea(a, b, c);
    if (area < 0) return Error::UnsupportedUnderside;
    if (area == 0) return Error::InvalidData;
    const double wa = signedArea(TerrainTopSample{x, 0, z}, b, c) / area;
    const double wb = signedArea(TerrainTopSample{x, 0, z}, c, a) / area;
    const double wc = signedArea(TerrainTopSample{x, 0, z}, a, b) / area;
    if (wa < 0 || wb < 0 || wc < 0) return Error::InvalidData;
    const double height = wa * a.y + wb * b.y + wc * c.y;
    if (!representable(height)) return Error::PrecisionLoss;
    return height;
}

auto validateVoxelSpans(std::span<const VoxelSolidSpan> spans) -> std::optional<TerrainSolidError> {
    double previousTop = -std::numeric_limits<double>::infinity();
    for (const VoxelSolidSpan& span : spans) {
        if (!representable(span.floor) || !representable(span.top) || !(span.top > span.floor) ||
            !(span.floor >= previousTop)) {
            return TerrainSolidError::InvalidData;
        }
        previousTop = span.top;
    }
    return {};
}

auto classifyVoxelColumn(std::span<const VoxelSolidSpan> spans, double height)
    -> Result<TerrainSolidSample, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (!std::isfinite(height)) return Error::InvalidData;
    if (auto error = validateVoxelSpans(spans)) return *error;
    bool inside = false;
    for (const VoxelSolidSpan& span : spans) {
        if (height >= span.floor && height <= span.top) inside = true;
    }
    TerrainSolidSample sample;
    sample.solid = inside;
    sample.policy = PolicyKind::Voxel;
    return sample;
}

auto classifyVoxelPlaneSolid(const VoxelPlaneSolidQuery& query, double height)
    -> Result<TerrainSolidSample, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (!std::isfinite(height)) return Error::InvalidData;
    if (auto error = validateVoxelSpans(query.spans)) return *error;
    if (query.other == PolicyKind::Voxel) return Error::UnsupportedVoxel;
    if (query.other == PolicyKind::Height) {
        if (query.otherFloor.has_value() || !representable(query.otherTop)) return Error::InvalidData;
    } else if (query.other == PolicyKind::Hex) {
        if (!query.otherFloor || !representable(*query.otherFloor) || !representable(query.otherTop))
            return Error::InvalidData;
        if (!(query.otherTop > *query.otherFloor)) return Error::HexFloorInversion;
    } else {
        return Error::InvalidData;
    }

    const std::uint8_t voxelSector = query.voxelOnNegative ? 0 : 1;
    const std::uint8_t otherSector = query.voxelOnNegative ? 1 : 0;
    struct Candidate {
        double top = 0;
        PolicyKind policy = PolicyKind::Height;
        std::uint8_t sector = 0;
        bool contains = false;
    };
    std::vector<Candidate> incident;
    incident.reserve(query.spans.size() + 1);
    for (const VoxelSolidSpan& span : query.spans) {
        incident.push_back({span.top, PolicyKind::Voxel, voxelSector,
                            height >= span.floor && height <= span.top});
    }
    bool otherContains = height <= query.otherTop;
    if (query.other == PolicyKind::Hex) otherContains = otherContains && height >= *query.otherFloor;
    incident.push_back({query.otherTop, query.other, otherSector, otherContains});

    int owner = -1;
    for (std::size_t i = 0; i < incident.size(); ++i) {
        if (!incident[i].contains) continue;
        if (owner < 0 || incident[i].top > incident[static_cast<std::size_t>(owner)].top ||
            (incident[i].top == incident[static_cast<std::size_t>(owner)].top &&
             incident[i].sector < incident[static_cast<std::size_t>(owner)].sector)) {
            owner = static_cast<int>(i);
        }
    }
    if (owner < 0) return TerrainSolidSample{};
    const Candidate& chosen = incident[static_cast<std::size_t>(owner)];
    std::optional<double> nearestShorter;
    for (std::size_t i = 0; i < incident.size(); ++i) {
        if (static_cast<int>(i) == owner || !(incident[i].top < chosen.top)) continue;
        if (!nearestShorter || incident[i].top > *nearestShorter) nearestShorter = incident[i].top;
    }
    TerrainSolidSample sample;
    sample.solid = true;
    sample.policy = chosen.policy;
    sample.sector = chosen.sector;
    sample.wallBand = nearestShorter.has_value() && height >= *nearestShorter && height <= chosen.top;
    return sample;
}

// UnsupportedVoxel, VoxelCavity, and VoxelOverhang never return an owner.
auto classifyVoxelStack(std::span<const bool> solidFromBottom) -> Result<TerrainSolidSample, TerrainSolidError> {
    using Error = TerrainSolidError;
    if (solidFromBottom.empty()) return Error::InvalidData;
    unsigned runs = 0;
    bool previous = false;
    bool seenSolid = false;
    bool airBelowSolid = false;
    for (const bool solid : solidFromBottom) {
        if (!solid && !seenSolid) airBelowSolid = true;
        if (solid && !previous) ++runs;
        if (solid) seenSolid = true;
        previous = solid;
    }
    if (runs >= 2) return Error::VoxelOverhang;
    if (runs == 1 && airBelowSolid) return Error::VoxelCavity;
    return Error::UnsupportedVoxel;
}

} // namespace f4::world
