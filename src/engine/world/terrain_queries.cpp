#include "engine/world/terrain_queries.h"

#include <cmath>
#include <limits>

namespace f4::world {
namespace {
using D3 = std::array<double, 3>;

auto finite(math::Vec3 v) -> bool {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
auto components(math::Vec3 v) -> D3 { return {v.x, v.y, v.z}; }
auto subtract(const D3& a, const D3& b) -> D3 {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
auto dot(const D3& a, const D3& b) -> double {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
auto cross(const D3& a, const D3& b) -> D3 {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
} // namespace

auto raycastTerrainMesh(const MeshData& mesh, math::Vec3 origin, math::Vec3 direction,
                         float maxDistance, math::Vec3 translation, bool cullBackFaces)
    -> std::optional<TerrainMeshRayHit> {
    if (!finite(origin) || !finite(direction) || !finite(translation) ||
        !std::isfinite(maxDistance) || maxDistance < 0.f || mesh.indices.size() % 3 != 0) return std::nullopt;
    D3 ray = components(direction);
    const double length = std::sqrt(dot(ray, ray));
    if (length == 0.0) return std::nullopt;
    for (auto& component : ray) component /= length;
    // Float inputs are promoted before arithmetic, including translation and
    // normalization, so large or subnormal finite directions do not overflow.
    const auto eye = subtract(components(origin), components(translation));
    for (const auto& vertex : mesh.vertices) if (!finite(vertex.position)) return std::nullopt;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return std::nullopt;

    std::size_t axis = 0;
    for (std::size_t i = 1; i < 3; ++i) if (std::abs(ray[i]) > std::abs(ray[axis])) axis = i;
    const auto u = (axis + 1) % 3, v = (axis + 2) % 3;
    const double shearU = ray[u] / ray[axis], shearV = ray[v] / ray[axis];
    std::optional<TerrainMeshRayHit> closest;
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
        const D3 a = components(mesh.vertices[mesh.indices[index]].position);
        const D3 b = components(mesh.vertices[mesh.indices[index + 1]].position);
        const D3 c = components(mesh.vertices[mesh.indices[index + 2]].position);
        const auto normal = cross(subtract(b, a), subtract(c, a));
        const double normalLength = std::sqrt(dot(normal, normal));
        if (normalLength == 0.0) continue;
        const bool front = dot(normal, ray) < 0.0;
        if (cullBackFaces && !front) continue;

        // Project into a ray-aligned plane. Shared directed edges use the same
        // double-precision product differences rather than per-face epsilons.
        std::array<D3, 3> p{subtract(a, eye), subtract(b, eye), subtract(c, eye)};
        for (auto& point : p) {
            point[u] -= shearU * point[axis];
            point[v] -= shearV * point[axis];
        }
        const auto edge = [&](std::size_t first, std::size_t second) {
            return p[first][u] * p[second][v] - p[first][v] * p[second][u];
        };
        const D3 weights{edge(1, 2), edge(2, 0), edge(0, 1)};
        if ((weights[0] < 0.0 || weights[1] < 0.0 || weights[2] < 0.0) &&
            (weights[0] > 0.0 || weights[1] > 0.0 || weights[2] > 0.0)) continue;
        const double determinant = weights[0] + weights[1] + weights[2];
        if (determinant == 0.0) continue;
        const D3 barycentric{weights[0] / determinant, weights[1] / determinant, weights[2] / determinant};
        const double distance = (barycentric[0] * p[0][axis] + barycentric[1] * p[1][axis] +
                                 barycentric[2] * p[2][axis]) / ray[axis];
        if (!std::isfinite(distance) || distance < 0.0 || distance > maxDistance ||
            (closest && distance >= closest->distance)) continue;
        const auto offset = components(translation);
        D3 position;
        for (std::size_t i = 0; i < 3; ++i) {
            position[i] = offset[i] + barycentric[0] * a[i] + barycentric[1] * b[i] + barycentric[2] * c[i];
        }
        constexpr double limit = std::numeric_limits<float>::max();
        if (std::abs(position[0]) > limit || std::abs(position[1]) > limit || std::abs(position[2]) > limit) continue;
        closest = TerrainMeshRayHit{distance,
            {static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])},
            {static_cast<float>(normal[0] / normalLength), static_cast<float>(normal[1] / normalLength),
             static_cast<float>(normal[2] / normalLength)}, barycentric, index / 3, front};
    }
    return closest;
}

} // namespace f4::world
