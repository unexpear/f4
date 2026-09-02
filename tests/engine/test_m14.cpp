#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>

#include <doctest/doctest.h>

#include "engine/world/world_data.h"

namespace {

using namespace f4;
using namespace f4::world;

auto readyChunk(ChunkStore& store, ChunkCoord coord, PolicyKind policy,
                const PolicyParams& params = {}) -> ChunkHandle {
    const ChunkHandle handle = store.create(coord, 0x1234'5678'9abc'def0ull, policy, params);
    REQUIRE(store.queue(handle));
    REQUIRE(store.generate(handle));
    return handle;
}

auto byteCopy(const memory::MemoryWriter& writer) -> memory::Array<std::byte> {
    return writer.bytes();
}

void hashBytes(std::uint64_t& hash, std::uint64_t value, std::uint32_t byteCount) {
    for (std::uint32_t byte = 0; byte < byteCount; ++byte) {
        hash ^= (value >> (byte * 8u)) & 0xffu;
        hash *= 1099511628211ull;
    }
}

auto voxelDigest(const VoxelCells& cells) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint16_t value : cells.values) hashBytes(hash, value, 2);
    return hash;
}

auto heightDigest(const HeightTile& tile) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (const HeightSample& sample : tile.samples) {
        hashBytes(hash, std::bit_cast<std::uint32_t>(sample.height), 4);
        hashBytes(hash, sample.biomeId, 2);
        for (std::uint8_t weight : sample.materialWeights) hashBytes(hash, weight, 1);
    }
    return hash;
}

auto hexDigest(const HexTile& tile) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (const HexCell& cell : tile.cells) {
        hashBytes(hash, cell.terrainType, 1);
        hashBytes(hash, cell.owner, 4);
    }
    const std::int32_t radius = static_cast<std::int32_t>(tile.radius);
    for (std::int32_t r = -radius; r <= radius; ++r) {
        for (std::int32_t q = -radius; q <= radius; ++q) {
            if (!tile.contains({q, r})) continue;
            for (std::uint32_t corner = 0; corner < 6; ++corner) {
                const auto height = tile.corner({q, r}, corner);
                if (height.has_value()) {
                    hashBytes(hash, std::bit_cast<std::uint32_t>(*height), 4);
                }
            }
        }
    }
    return hash;
}

auto bytesDigest(const memory::Array<std::byte>& bytes) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::byte value : bytes) hashBytes(hash, std::to_integer<std::uint8_t>(value), 1);
    return hash;
}

} // namespace

TEST_CASE("M14: floor division and coordinate hashes preserve negative-space identity") {
    static_assert(VoxelPolicy::kind == PolicyKind::Voxel);
    static_assert(HeightPolicy::kind == PolicyKind::Height);
    static_assert(HexPolicy::kind == PolicyKind::Hex);
    static_assert(std::is_same_v<VoxelPolicy::Cell, std::uint16_t>);
    struct Case { std::int32_t value; std::int32_t divisor; std::int32_t div; std::int32_t mod; };
    constexpr Case cases[] = {
        {-513, 512, -2, 511}, {-512, 512, -1, 0}, {-511, 512, -1, 1},
        {-2, 2, -1, 0}, {-1, 2, -1, 1}, {0, 2, 0, 0}, {1, 2, 0, 1}, {2, 2, 1, 0}};
    for (const Case& item : cases) {
        CHECK(floorDiv(item.value, item.divisor) == item.div);
        CHECK(floorMod(item.value, item.divisor) == item.mod);
        CHECK(item.value == floorDiv(item.value, item.divisor) * item.divisor +
                                floorMod(item.value, item.divisor));
    }

    containers::HashMap<ChunkCoord, std::uint32_t> chunks;
    CHECK(chunks.insert({-1, 2, -3}, 7));
    REQUIRE(chunks.find({-1, 2, -3}) != nullptr);
    CHECK(*chunks.find({-1, 2, -3}) == 7);
    CHECK(chunks.find({-1, 2, 3}) == nullptr);

    RegionMap regions;
    CHECK(regions.regionFor(-513, 511) == RegionCoord{-2, 0});
    CHECK(regions.regionFor(-512, 512) == RegionCoord{-1, 1});
    regions.set({-1, 1}, PolicyKind::Voxel);
    CHECK(regions.policyAt(-1, 512) == PolicyKind::Voxel);
    CHECK(regions.policyAt(0, 512) == PolicyKind::Height);
    CHECK(regions.erase({-1, 1}));
}

TEST_CASE("M14: block registry enforces reserved and game id ranges") {
    BlockRegistry blocks;
    REQUIRE(blocks.find(0) != nullptr);
    CHECK_FALSE(blocks.find(0)->solid);
    CHECK_FALSE(blocks.registerGameBlock(255, {true, true, 2, 1.0f}));
    CHECK(blocks.registerGameBlock(256, {true, false, 7, 2.5f}));
    CHECK_FALSE(blocks.registerGameBlock(256, {false, false, 0, 0.0f}));
    REQUIRE(blocks.find(256) != nullptr);
    CHECK(blocks.find(256)->textureLayer == 7);
    CHECK_FALSE(blocks.setEngineBlock(256, {}));
}

TEST_CASE("M14: voxel policy is deterministic, dense, editable, and DDA-queryable") {
    CHECK(voxelIndex(0, 0, 0) == 0);
    CHECK(voxelIndex(31, 31, 31) == kVoxelCellCount - 1);
    CHECK(voxelIndex(5, 7, 11) == (5u | (11u << 5u) | (7u << 10u)));

    const VoxelCells a = generateVoxelCells(44, {-3, 1, 8}, {});
    const VoxelCells b = generateVoxelCells(44, {-3, 1, 8}, {});
    CHECK(a == b);
    CHECK(a.values.size() * sizeof(std::uint16_t) == 64u * 1024u);
    VoxelPolicyParams invalid;
    invalid.frequency = std::numeric_limits<float>::quiet_NaN();
    CHECK(generateVoxelCells(1, {}, invalid).values.isEmpty());

    VoxelCells cells;
    cells.values.resize(kVoxelCellCount);
    CHECK(isVoxelEmpty(cells));
    cells.values[voxelIndex(4, 2, 2)] = 2;
    CHECK_FALSE(isVoxelEmpty(cells));
    const auto localHit = raycastVoxels(cells, {0.5f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}, 8.0f);
    REQUIRE(localHit.has_value());
    CHECK(localHit->x == 4);
    CHECK(localHit->enteringFaceNormal == math::Vec3{-1.0f, 0.0f, 0.0f});
    CHECK(localHit->t == doctest::Approx(3.5f));

    const auto worldHit = raycastVoxelWorld(
        {30.5f, 2.5f, 2.5f}, {1.0f, 0.0f, 0.0f}, 8.0f,
        [](std::int32_t x, std::int32_t y, std::int32_t z) -> std::uint16_t {
            return x == 34 && y == 2 && z == 2 ? 2 : 0;
        });
    REQUIRE(worldHit.has_value());
    CHECK(worldHit->x == 34);
    CHECK(worldHit->t == doctest::Approx(3.5f));
}

TEST_CASE("M14: generated policy outputs match cross-toolchain golden digests") {
    HexPolicyParams hexParams;
    hexParams.radius = 4;
    const std::uint64_t voxel = voxelDigest(generateVoxelCells(0xabc'def1, {-7, 2, 11}, {}));
    const std::uint64_t height = heightDigest(generateHeightTile(0xabc'def1, {-7, 0, 11}, {}));
    const std::uint64_t hex = hexDigest(generateHexTile(0xabc'def1, {-7, 0, 11}, hexParams));
    ChunkStore store(1);
    const ChunkHandle handle = readyChunk(store, {-7, 2, 11}, PolicyKind::Voxel);
    REQUIRE(store.edit(handle, 900, CellValue::voxelValue(9)));
    REQUIRE(store.edit(handle, 2, CellValue::voxelValue(4)));
    memory::MemoryWriter writer;
    REQUIRE(store.saveDeltas(handle, writer));
    const std::uint64_t deltas = bytesDigest(writer.bytes());
    INFO("voxel digest: " << voxel);
    INFO("height digest: " << height);
    INFO("hex digest: " << hex);
    INFO("delta digest: " << deltas);
    CHECK(voxel == 18334859166678057859ull);
    CHECK(height == 17382306546337681000ull);
    CHECK(hex == 1641998045346952095ull);
    CHECK(deltas == 3603394504325397505ull);
}

TEST_CASE("M14: chunk lifecycle rejects stale work and preserves sparse deltas") {
    ChunkStore store(4);
    const ChunkHandle handle = store.create({-2, 0, 3}, 99, PolicyKind::Voxel);
    CHECK(store.find({-2, 0, 3}) == handle);
    CHECK(store.state(handle) == ChunkState::Invalid);
    CHECK_FALSE(store.generate(handle));
    REQUIRE(store.queue(handle));
    REQUIRE(store.generate(handle));
    CHECK(store.state(handle) == ChunkState::CellsReady);
    CHECK(store.dirty(handle) == DirtyFlags{false, true, true});

    const auto generated = store.cell(handle, voxelIndex(1, 1, 1));
    REQUIRE(generated.has_value());
    REQUIRE(store.edit(handle, voxelIndex(1, 1, 1), CellValue::voxelValue(300)));
    CHECK(store.cell(handle, voxelIndex(1, 1, 1))->voxel == 300);
    const auto edited = store.snapshot(handle);
    REQUIRE(edited.has_value());
    REQUIRE(edited->deltas.size() == 1);
    CHECK(edited->deltas[0].index == voxelIndex(1, 1, 1));
    CHECK_FALSE(store.markMeshed(handle, edited->version - 1));
    REQUIRE(store.markMeshed(handle, edited->version));
    CHECK(store.state(handle) == ChunkState::Meshed);
    CHECK(store.dirty(handle) == DirtyFlags{});
    const std::uint64_t meshedVersion = store.version(handle);
    REQUIRE(store.markDirty(handle, {false, false, true}));
    CHECK(store.state(handle) == ChunkState::CellsReady);
    CHECK(store.dirty(handle) == DirtyFlags{false, false, true});
    CHECK(store.version(handle) == meshedVersion + 1u);
    CHECK_FALSE(store.markMeshed(handle, meshedVersion));

    REQUIRE(store.regenerate(handle));
    CHECK(store.cell(handle, voxelIndex(1, 1, 1))->voxel == 300);
    REQUIRE(store.destroy(handle));
    CHECK(store.state(handle) == ChunkState::Invalid);
    CHECK_FALSE(store.destroy(handle));
    const ChunkHandle replacement = store.create({-2, 0, 3}, 99, PolicyKind::Voxel);
    CHECK(replacement.index == handle.index);
    CHECK(replacement.generation != handle.generation);
}

TEST_CASE("M14: chunk saves contain only sorted edits and reject corruption") {
    ChunkStore source(2);
    const ChunkHandle sourceHandle = readyChunk(source, {3, -1, 4}, PolicyKind::Voxel);
    memory::MemoryWriter empty;
    REQUIRE(source.saveDeltas(sourceHandle, empty));
    CHECK(empty.bytes().isEmpty());

    REQUIRE(source.edit(sourceHandle, 900, CellValue::voxelValue(9)));
    REQUIRE(source.edit(sourceHandle, 2, CellValue::voxelValue(4)));
    REQUIRE(source.edit(sourceHandle, 100, CellValue::voxelValue(7)));
    memory::MemoryWriter first;
    memory::MemoryWriter second;
    REQUIRE(source.saveDeltas(sourceHandle, first));
    REQUIRE(source.saveDeltas(sourceHandle, second));
    CHECK(first.bytes().size() == second.bytes().size());
    CHECK(std::equal(first.bytes().begin(), first.bytes().end(), second.bytes().begin()));

    ChunkStore target(2);
    const ChunkHandle targetHandle = readyChunk(target, {3, -1, 4}, PolicyKind::Voxel);
    REQUIRE(target.loadDeltas(targetHandle, first.bytes()));
    const std::uint64_t loadedVersion = target.version(targetHandle);
    const auto loaded = target.snapshot(targetHandle);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->deltas.size() == 3);
    CHECK(loaded->deltas[0].index == 2);
    CHECK(loaded->deltas[1].index == 100);
    CHECK(loaded->deltas[2].index == 900);
    CHECK(target.cell(targetHandle, 2)->voxel == 4);
    CHECK(target.cell(targetHandle, 100)->voxel == 7);
    CHECK(target.cell(targetHandle, 900)->voxel == 9);

    memory::Array<std::byte> corrupt = byteCopy(first);
    corrupt[0] ^= std::byte{0xff};
    CHECK_FALSE(target.loadDeltas(targetHandle, corrupt));
    CHECK(target.snapshot(targetHandle)->deltas.size() == 3);
    REQUIRE(target.loadDeltas(targetHandle, first.bytes()));
    CHECK(target.version(targetHandle) > loadedVersion);
    ChunkStore wrongCoord(1);
    const ChunkHandle wrong = readyChunk(wrongCoord, {4, -1, 4}, PolicyKind::Voxel);
    CHECK_FALSE(wrongCoord.loadDeltas(wrong, first.bytes()));
}

TEST_CASE("M14: height and hex delta codecs round-trip every cell field") {
    ChunkStore heights(2);
    const ChunkHandle sourceHeight = readyChunk(heights, {-2, 0, 5}, PolicyKind::Height);
    HeightSample heightEdit;
    heightEdit.height = -12.75f;
    heightEdit.biomeId = 42;
    heightEdit.materialWeights = {1, 2, 3, 249};
    REQUIRE(heights.edit(sourceHeight, 77, CellValue::heightValue(heightEdit)));
    memory::MemoryWriter heightBytes;
    REQUIRE(heights.saveDeltas(sourceHeight, heightBytes));
    const ChunkHandle targetHeight = readyChunk(heights, {-3, 0, 5}, PolicyKind::Height);
    CHECK_FALSE(heights.loadDeltas(targetHeight, heightBytes.bytes()));

    ChunkStore heightReload(1);
    const ChunkHandle reloadedHeight = readyChunk(heightReload, {-2, 0, 5}, PolicyKind::Height);
    REQUIRE(heightReload.loadDeltas(reloadedHeight, heightBytes.bytes()));
    REQUIRE(heightReload.cell(reloadedHeight, 77).has_value());
    CHECK(heightReload.cell(reloadedHeight, 77)->height == heightEdit);
    memory::Array<std::byte> noEdits;
    REQUIRE(heightReload.loadDeltas(reloadedHeight, noEdits));
    CHECK(heightReload.snapshot(reloadedHeight)->deltas.isEmpty());

    PolicyParams params;
    params.hex.radius = 2;
    ChunkStore hexSource(1);
    const ChunkHandle sourceHex = readyChunk(hexSource, {4, 0, -3}, PolicyKind::Hex, params);
    const auto sourceSnapshot = hexSource.snapshot(sourceHex);
    REQUIRE(sourceSnapshot.has_value());
    const HexTile* tile = std::get_if<HexTile>(&sourceSnapshot->base);
    REQUIRE(tile != nullptr);
    const auto centerIndex = tile->index({0, 0});
    REQUIRE(centerIndex.has_value());
    const HexCell hexEdit{7, 0x1234'5678u};
    REQUIRE(hexSource.edit(sourceHex, *centerIndex, CellValue::hexValue(hexEdit)));
    memory::MemoryWriter hexBytes;
    REQUIRE(hexSource.saveDeltas(sourceHex, hexBytes));

    ChunkStore hexReload(1);
    const ChunkHandle reloadedHex = readyChunk(hexReload, {4, 0, -3}, PolicyKind::Hex, params);
    REQUIRE(hexReload.loadDeltas(reloadedHex, hexBytes.bytes()));
    REQUIRE(hexReload.cell(reloadedHex, *centerIndex).has_value());
    CHECK(hexReload.cell(reloadedHex, *centerIndex)->hex == hexEdit);
}

TEST_CASE("M14: height tiles share exact edges, normalized sampling, and seam normals") {
    constexpr std::uint64_t seed = 0xfeed'beef;
    const HeightTile center = generateHeightTile(seed, {-1, 0, 2}, {});
    const HeightTile east = generateHeightTile(seed, {0, 0, 2}, {});
    const HeightTile north = generateHeightTile(seed, {-1, 0, 3}, {});
    REQUIRE(center.samples.size() == kHeightSampleCount);
    for (std::uint32_t i = 0; i < kHeightSamples; ++i) {
        CHECK(center.samples[heightSampleIndex(kHeightQuads, i)] ==
              east.samples[heightSampleIndex(0, i)]);
        CHECK(center.samples[heightSampleIndex(i, kHeightQuads)] ==
              north.samples[heightSampleIndex(i, 0)]);
        const HeightSample& sample = center.samples[heightSampleIndex(i, i)];
        CHECK(static_cast<unsigned>(sample.materialWeights[0]) + sample.materialWeights[1] +
                  sample.materialWeights[2] + sample.materialWeights[3] == 255u);
    }

    const auto sampled = sampleHeight(center, 12.25f, 19.75f);
    REQUIRE(sampled.has_value());
    CHECK(sampled->height == doctest::Approx(bilinearHeight(center, 12.25f, 19.75f)));
    CHECK(static_cast<unsigned>(sampled->materialWeights[0]) + sampled->materialWeights[1] +
              sampled->materialWeights[2] + sampled->materialWeights[3] == 255u);

    auto fetch = [&](std::int32_t x, std::int32_t z) -> float {
        const std::int32_t chunkX = floorDiv(x, static_cast<std::int32_t>(kHeightQuads));
        const std::uint32_t localX = static_cast<std::uint32_t>(
            floorMod(x, static_cast<std::int32_t>(kHeightQuads)));
        const std::uint32_t localZ = static_cast<std::uint32_t>(z - 2 *
            static_cast<std::int32_t>(kHeightQuads));
        const HeightTile& tile = chunkX == -1 ? center : east;
        return tile.samples[heightSampleIndex(localX, localZ)].height;
    };
    const math::Vec3 seamNormal = heightNormal(0, 140, fetch);
    const float left = center.samples[heightSampleIndex(63, 12)].height;
    const float right = east.samples[heightSampleIndex(1, 12)].height;
    const float down = east.samples[heightSampleIndex(0, 11)].height;
    const float up = east.samples[heightSampleIndex(0, 13)].height;
    CHECK(seamNormal == math::normalized({left - right, 2.0f, down - up}));
    CHECK(math::length(seamNormal) == doctest::Approx(1.0f));
}

TEST_CASE("M14: hex topology, dense backing, and corner lattice are coherent") {
    for (const AxialCoord direction : kHexDirections) {
        CHECK(hexDistance({}, direction) == 1);
        CHECK(hexDistance(direction, {}) == 1);
    }
    const auto ring = hexRing({-3, 4}, 4);
    CHECK(ring.size() == 24);
    for (const AxialCoord cell : ring) CHECK(hexDistance({-3, 4}, cell) == 4);
    const auto spiral = hexSpiral({2, -1}, 5);
    CHECK(spiral.size() == 1u + 3u * 5u * 6u);
    const auto line = hexLine({-4, 2}, {3, -5});
    REQUIRE(line.size() == hexDistance({-4, 2}, {3, -5}) + 1u);
    CHECK(line[0] == AxialCoord{-4, 2});
    CHECK(line[line.size() - 1] == AxialCoord{3, -5});

    HexPolicyParams params;
    params.radius = 3;
    const HexTile tile = generateHexTile(77, {-2, 0, 3}, params);
    REQUIRE(tile.width == 7);
    CHECK(tile.cells.size() == 49);
    CHECK(tile.contains({-3, 0}));
    CHECK_FALSE(tile.contains({-3, -3}));
    REQUIRE(tile.index({0, 0}).has_value());
    CHECK(tile.cells[*tile.index({0, 0})].terrainType != kInvalidTerrain);
    CHECK(tile.cells[0].terrainType == kInvalidTerrain);
    REQUIRE(tile.corner({0, 0}, 0).has_value());
    REQUIRE(tile.corner({1, 0}, 2).has_value());
    CHECK(*tile.corner({0, 0}, 0) == *tile.corner({1, 0}, 2));
    CHECK(hexCorner({0, 0}, 0) == hexCorner({1, 0}, 2));
    const auto centerHeight = tile.centerHeight({0, 0});
    REQUIRE(centerHeight.has_value());
    float cornerSum = 0.0f;
    for (std::uint32_t corner = 0; corner < 6; ++corner) {
        cornerSum += *tile.corner({0, 0}, corner);
    }
    CHECK(*centerHeight == doctest::Approx(cornerSum / 6.0f));
    CHECK(generateHexTile(77, {-2, 0, 3}, params).cells.size() == tile.cells.size());

    params.radius = std::numeric_limits<std::uint32_t>::max();
    CHECK(generateHexTile(1, {}, params).cells.isEmpty());
}

TEST_CASE("M14: snapshots stay coherent while edits advance the overlay") {
    ChunkStore store(1);
    const ChunkHandle handle = readyChunk(store, {}, PolicyKind::Voxel);
    std::atomic<bool> writerDone{false};
    std::atomic<bool> coherent{true};
    std::thread writer([&] {
        for (std::uint16_t value = 1; value <= 1000; ++value) {
            if (!store.edit(handle, 17, CellValue::voxelValue(value))) coherent = false;
        }
        writerDone = true;
    });
    std::thread reader([&] {
        std::uint64_t previousVersion = 0;
        while (!writerDone.load()) {
            const auto copy = store.snapshot(handle);
            if (!copy.has_value() || copy->version < previousVersion || copy->deltas.size() > 1) {
                coherent = false;
                break;
            }
            if (!copy->deltas.isEmpty() &&
                (copy->deltas[0].index != 17 || copy->deltas[0].value.voxel == 0)) {
                coherent = false;
                break;
            }
            previousVersion = copy->version;
        }
    });
    writer.join();
    reader.join();
    CHECK(coherent.load());
    CHECK(store.version(handle) == 1000);
    CHECK(store.cell(handle, 17)->voxel == 1000);
}
