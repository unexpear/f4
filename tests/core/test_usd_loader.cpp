#include <doctest/doctest.h>

#include <cstdio>
#include <vector>

#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/resources/usd_loader.h"

using namespace f4::resources;

static const char* kCubeUsda =
    F4_TINYUSDZ_TEST_DATA_DIR "/cube-indexed-primvar.usda";
static const char* kCubeUsdc = F4_TINYUSDZ_TEST_DATA_DIR "/cube.usdc";

static std::vector<std::byte> loadFile(const char* path) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path, "rb");
#else
    f = fopen(path, "rb");
#endif
    REQUIRE(f != nullptr);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<std::byte> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);
    return data;
}

TEST_CASE("USD: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadUsd(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("USD: garbage input rejected") {
    std::vector<std::byte> garbage(4);
    garbage[0] = std::byte{'G'};
    garbage[1] = std::byte{'A'};
    garbage[2] = std::byte{'R'};
    garbage[3] = std::byte{'B'};
    auto result = loadUsd(garbage);
    CHECK(result.hasError());
}

TEST_CASE("USD: cube.usda loads a mesh") {
    auto data = loadFile(kCubeUsda);
    REQUIRE(data.size() > 0);

    auto result = loadUsd(data);
    REQUIRE(result.hasValue());

    auto& scene = result.value();
    REQUIRE(scene.meshes.size() >= 1);

    bool foundMesh = false;
    for (const auto& mesh : scene.meshes) {
        if (mesh.vertexCount() == 0) {
            continue;
        }
        foundMesh = true;
        // 6 quad faces, 8 unique points -> >= 12 triangles.
        CHECK(mesh.vertexCount() >= 8);
        CHECK(mesh.indexCount() >= 12);
        CHECK(mesh.bounds.max.x > mesh.bounds.min.x);
        CHECK(mesh.bounds.max.y > mesh.bounds.min.y);
        CHECK(mesh.bounds.max.z > mesh.bounds.min.z);

        CHECK(mesh.bounds.min.x >= -1.01f);
        CHECK(mesh.bounds.max.x <= 1.01f);
        CHECK(mesh.bounds.min.y >= -1.01f);
        CHECK(mesh.bounds.max.y <= 1.01f);
        CHECK(mesh.bounds.min.z >= -1.01f);
        CHECK(mesh.bounds.max.z <= 1.01f);
    }
    CHECK(foundMesh);
}

TEST_CASE("USD: cube.usdc loads a mesh") {
    auto data = loadFile(kCubeUsdc);
    REQUIRE(data.size() > 0);

    auto result = loadUsd(data);
    REQUIRE(result.hasValue());

    bool foundMesh = false;
    for (const auto& mesh : result.value().meshes) {
        if (mesh.vertexCount() == 0) {
            continue;
        }
        foundMesh = true;
        CHECK(mesh.vertexCount() >= 8);
        CHECK(mesh.indexCount() >= 12);

        bool hasPos = false;
        for (const auto& a : mesh.attributes) {
            if (a.semantic == VertexSemantic::Position) {
                hasPos = true;
                CHECK(a.format == VertexFormat::F32_3);
            }
        }
        CHECK(hasPos);
    }
    CHECK(foundMesh);
}
