#include <cmath>
#include <limits>

#include <doctest/doctest.h>

#include "engine/world/terrain_queries.h"

namespace {
using namespace f4;
using namespace f4::world;

auto quad(float scale = 1.f) -> MeshData {
    MeshData mesh;
    for (const auto p : {math::Vec3{0, 2, 0}, math::Vec3{0, 2, 1},
                         math::Vec3{1, 2, 1}, math::Vec3{1, 2, 0}}) mesh.vertices.push({p * scale});
    for (const std::uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u}) mesh.indices.push(i);
    return mesh;
}
} // namespace

TEST_CASE("Terrain rays: all principal axes, winding, closed distances and barycentrics") {
    for (int rotation = 0; rotation < 3; ++rotation) {
        const auto rotate = [rotation](math::Vec3 p) {
            const float values[]{p.x, p.y, p.z};
            return math::Vec3{values[rotation], values[(rotation + 1) % 3], values[(rotation + 2) % 3]};
        };
        auto mesh = quad();
        for (auto& vertex : mesh.vertices) vertex.position = rotate(vertex.position);
        const auto eye = rotate({0.25f, 5.f, 0.75f});
        const auto down = rotate({0.f, -7.f, 0.f});
        const auto hit = raycastTerrainMesh(mesh, eye, down, 3.f, {}, true);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == 3.0);
        CHECK(hit->position == rotate({0.25f, 2.f, 0.75f}));
        CHECK(hit->normal == rotate({0.f, 1.f, 0.f}));
        CHECK(hit->frontFace);
        CHECK(hit->triangleIndex == 0);
        CHECK(hit->barycentric[0] == 0.25);
        CHECK(hit->barycentric[1] == 0.5);
        CHECK(hit->barycentric[2] == 0.25);
        CHECK_FALSE(raycastTerrainMesh(mesh, eye, down, std::nextafter(3.f, 0.f)));
        const auto below = rotate({0.25f, 1.f, 0.75f});
        const auto up = rotate({0.f, 1.f, 0.f});
        const auto back = raycastTerrainMesh(mesh, below, up, 1.f);
        REQUIRE(back.has_value());
        CHECK_FALSE(back->frontFace);
        CHECK(back->normal == hit->normal);
        CHECK_FALSE(raycastTerrainMesh(mesh, below, up, 1.f, {}, true));
        const auto onSurface = raycastTerrainMesh(mesh, hit->position, up, 0.f);
        REQUIRE(onSurface.has_value());
        CHECK(onSurface->distance == 0.0);
        CHECK_FALSE(raycastTerrainMesh(mesh, hit->position, rotate({1.f, 0.f, 0.f}), 5.f));
    }
}

TEST_CASE("Terrain rays: shared edges, translated vertices and extreme finite scales") {
    const auto mesh = quad();
    for (const auto translation : {math::Vec3{}, math::Vec3{-64.f, 0.f, 128.f},
                                    math::Vec3{65536.f, 0.f, -65536.f}}) {
        for (unsigned step = 0; step <= 64; ++step) {
            const float t = static_cast<float>(step) / 64.f;
            const auto hit = raycastTerrainMesh(mesh, translation + math::Vec3{t, 5.f, t}, {0, -1, 0}, 3.f, translation);
            REQUIRE(hit.has_value());
            CHECK(hit->triangleIndex == 0); // Stable lowest-index tie on the diagonal.
            CHECK(hit->distance == 3.0);
        }
    }
    for (unsigned step = 1; step < 64; ++step) {
        const float t = static_cast<float>(step) / 64.f;
        const auto left = raycastTerrainMesh(mesh, {std::nextafter(t, 0.f), 5.f, t}, {0, -1, 0}, 3.f);
        const auto right = raycastTerrainMesh(mesh, {std::nextafter(t, 1.f), 5.f, t}, {0, -1, 0}, 3.f);
        REQUIRE(left.has_value());
        REQUIRE(right.has_value());
        CHECK(left->triangleIndex == 0);
        CHECK(right->triangleIndex == 1);
        for (const auto direction : {math::Vec3{0.5f, -1.f, 0.25f}, math::Vec3{-0.5f, -1.f, -0.25f},
                                     math::Vec3{-0.7f, -1.f, 0.2f}}) {
            const math::Vec3 target{t, 2.f, t};
            const auto oblique = raycastTerrainMesh(mesh, target - direction * 4.f, direction, 5.f);
            REQUIRE(oblique.has_value());
            CHECK(oblique->position.x == doctest::Approx(t));
            CHECK(oblique->position.z == doctest::Approx(t));
            CHECK(oblique->normal == math::Vec3{0, 1, 0});
        }
    }
    for (const float scale : {1e-30f, 0.0001f, 1.f, 1e20f}) {
        const auto scaled = quad(scale);
        const auto hit = raycastTerrainMesh(scaled, {0.25f * scale, 5.f * scale, 0.75f * scale}, {0, -1, 0}, 4.f * scale);
        REQUIRE(hit.has_value());
        CHECK(hit->distance / scale == doctest::Approx(3.0));
        CHECK(hit->normal == math::Vec3{0, 1, 0});
    }
    for (const float magnitude : {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()}) {
        const auto hit = raycastTerrainMesh(mesh, {0.25f, 5.f, 0.75f}, {0, -magnitude, 0}, 3.f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == 3.0);
    }
}

TEST_CASE("Terrain rays: nearest surface, invalid data and degenerate faces") {
    auto mesh = quad();
    auto high = quad();
    for (auto vertex : high.vertices) { vertex.position.y = 4.f; mesh.vertices.push(vertex); }
    for (const auto index : high.indices) mesh.indices.push(index + 4);
    auto hit = raycastTerrainMesh(mesh, {0.5f, 5.f, 0.5f}, {0, -1, 0}, 8.f);
    REQUIRE(hit.has_value());
    CHECK(hit->distance == 1.0);
    CHECK(hit->triangleIndex == 2);
    CHECK_FALSE(raycastTerrainMesh(mesh, {2, 5, 2}, {0, -1, 0}, 8.f));
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {}, 8.f));
    for (const float bad : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, bad));
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(raycastTerrainMesh(mesh, {nan, 5, 0.5f}, {0, -1, 0}, 8));
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, nan, 0}, 8));
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, 8, {nan, 0, 0}));
    mesh.indices.push(0); // Incomplete triangle rejects the whole query.
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, 8));
    mesh.indices.pop();
    mesh.indices.back() = 999;
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, 8));
    mesh = quad();
    mesh.vertices.push({{nan, 0, 0}});
    CHECK_FALSE(raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, 8));
    mesh = quad();
    mesh.indices[1] = mesh.indices[0];
    hit = raycastTerrainMesh(mesh, {0.5f, 5, 0.5f}, {0, -1, 0}, 8);
    REQUIRE(hit.has_value());
    CHECK(hit->triangleIndex == 1);
}

TEST_CASE("Terrain rays: nonplanar height quads follow rendered triangles not bilinear sampling") {
    HeightTile tile;
    tile.samples.resize(kHeightSampleCount);
    tile.samples[heightSampleIndex(11, 11)].height = 4.f;
    const auto built = meshHeightChunk(tile, {});
    REQUIRE(built.hasValue());
    const auto hit = raycastTerrainMesh(built.value(), {10.25f, 10.f, 10.75f}, {0, -1, 0}, 20.f);
    REQUIRE(hit.has_value());
    CHECK(hit->position.y == 1.f);
    const auto sampled = sampleHeight(tile, 10.25f, 10.75f);
    REQUIRE(sampled.has_value());
    CHECK(sampled->height == 0.75f); // Existing sampling API is intentionally unchanged.
    CHECK(hit->position.y != sampled->height);
}
