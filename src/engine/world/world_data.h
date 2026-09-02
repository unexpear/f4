#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <variant>

#include "core/containers/array.h"
#include "core/containers/hashmap.h"
#include "core/ids/generational_arena.h"
#include "core/math/vec3.h"
#include "core/memory/serialization.h"

namespace f4::world {

[[nodiscard]] auto floorDiv(std::int32_t value, std::int32_t positiveDivisor)
    -> std::int32_t;
[[nodiscard]] auto floorMod(std::int32_t value, std::int32_t positiveDivisor)
    -> std::int32_t;

struct ChunkCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    friend constexpr auto operator==(const ChunkCoord&, const ChunkCoord&) -> bool = default;
};

struct RegionCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;
    friend constexpr auto operator==(const RegionCoord&, const RegionCoord&) -> bool = default;
};

struct AxialCoord {
    std::int32_t q = 0;
    std::int32_t r = 0;
    friend constexpr auto operator==(const AxialCoord&, const AxialCoord&) -> bool = default;
};

struct HexCornerCoord {
    std::int64_t x2 = 0;
    std::int64_t y2 = 0;
    friend constexpr auto operator==(const HexCornerCoord&, const HexCornerCoord&) -> bool = default;
};

} // namespace f4::world

template <>
struct std::hash<f4::world::ChunkCoord> {
    auto operator()(const f4::world::ChunkCoord& value) const noexcept -> std::size_t;
};

template <>
struct std::hash<f4::world::RegionCoord> {
    auto operator()(const f4::world::RegionCoord& value) const noexcept -> std::size_t;
};

template <>
struct std::hash<f4::world::AxialCoord> {
    auto operator()(const f4::world::AxialCoord& value) const noexcept -> std::size_t;
};

template <>
struct std::hash<f4::world::HexCornerCoord> {
    auto operator()(const f4::world::HexCornerCoord& value) const noexcept -> std::size_t;
};

namespace f4::world {

enum class PolicyKind : std::uint8_t { Voxel, Height, Hex };
enum class ChunkState : std::uint8_t { Invalid, Queued, LayoutDone, CellsReady, Meshed };

struct DirtyFlags {
    bool cellsDirty = false;
    bool meshDirty = false;
    bool bordersDirty = false;
    friend constexpr auto operator==(const DirtyFlags&, const DirtyFlags&) -> bool = default;
};

struct ChunkTag;
using ChunkHandle = ids::Handle<ChunkTag>;

struct VoxelPolicyParams {
    float baseHeight = 16.0f;
    float heightAmplitude = 12.0f;
    float frequency = 0.015625f;
    friend constexpr auto operator==(const VoxelPolicyParams&,
                                     const VoxelPolicyParams&) -> bool = default;
};

struct HeightPolicyParams {
    float baseHeight = 20.0f;
    float heightAmplitude = 16.0f;
    float frequency = 0.0078125f;
    friend constexpr auto operator==(const HeightPolicyParams&,
                                     const HeightPolicyParams&) -> bool = default;
};

struct HexPolicyParams {
    std::uint32_t radius = 16;
    float heightAmplitude = 8.0f;
    float frequency = 0.03125f;
    friend constexpr auto operator==(const HexPolicyParams&,
                                     const HexPolicyParams&) -> bool = default;
};

struct PolicyParams {
    VoxelPolicyParams voxel;
    HeightPolicyParams height;
    HexPolicyParams hex;
    friend constexpr auto operator==(const PolicyParams&, const PolicyParams&) -> bool = default;
};

struct BlockDesc {
    bool solid = false;
    bool opaque = false;
    std::uint16_t textureLayer = 0;
    float hardness = 0.0f;
};

class BlockRegistry {
public:
    BlockRegistry();
    auto setEngineBlock(std::uint16_t id, const BlockDesc& desc) -> bool;
    auto registerGameBlock(std::uint16_t id, const BlockDesc& desc) -> bool;
    [[nodiscard]] auto find(std::uint16_t id) const -> const BlockDesc*;

private:
    containers::HashMap<std::uint16_t, BlockDesc> blocks_;
};

inline constexpr std::uint32_t kVoxelEdge = 32;
inline constexpr std::uint32_t kVoxelCellCount = kVoxelEdge * kVoxelEdge * kVoxelEdge;
[[nodiscard]] constexpr auto voxelIndex(std::uint32_t x, std::uint32_t y,
                                        std::uint32_t z) -> std::uint32_t {
    return x | (z << 5u) | (y << 10u);
}

struct VoxelCells {
    containers::Array<std::uint16_t> values;
    friend auto operator==(const VoxelCells& a, const VoxelCells& b) -> bool;
};

[[nodiscard]] auto isVoxelEmpty(const VoxelCells& cells) -> bool;
struct VoxelRayHit {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    math::Vec3 enteringFaceNormal{};
    float t = 0.0f;
};

using VoxelFetch = std::function<std::uint16_t(std::int32_t, std::int32_t,
                                               std::int32_t)>;
[[nodiscard]] auto generateVoxelCells(std::uint64_t worldSeed, ChunkCoord coord,
                                      const VoxelPolicyParams& params) -> VoxelCells;
[[nodiscard]] auto raycastVoxels(const VoxelCells& cells, math::Vec3 origin,
                                 math::Vec3 direction, float maxDistance)
    -> std::optional<VoxelRayHit>;
[[nodiscard]] auto raycastVoxelWorld(math::Vec3 origin, math::Vec3 direction,
                                     float maxDistance, const VoxelFetch& fetch)
    -> std::optional<VoxelRayHit>;

struct VoxelPolicy {
    using Cell = std::uint16_t;
    using Cells = VoxelCells;
    using Params = VoxelPolicyParams;
    static constexpr PolicyKind kind = PolicyKind::Voxel;
    static constexpr std::uint32_t chunkExtent = kVoxelEdge;
    [[nodiscard]] static auto generate(std::uint64_t seed, ChunkCoord coord,
                                       const Params& params) -> Cells {
        return generateVoxelCells(seed, coord, params);
    }
};

inline constexpr std::uint32_t kHeightQuads = 64;
inline constexpr std::uint32_t kHeightSamples = kHeightQuads + 1;
inline constexpr std::uint32_t kHeightSampleCount = kHeightSamples * kHeightSamples;

struct HeightSample {
    float height = 0.0f;
    std::uint16_t biomeId = 0;
    std::array<std::uint8_t, 4> materialWeights{{255, 0, 0, 0}};
    friend constexpr auto operator==(const HeightSample&, const HeightSample&) -> bool = default;
};

struct HeightTile {
    containers::Array<HeightSample> samples;
    friend auto operator==(const HeightTile& a, const HeightTile& b) -> bool;
};

[[nodiscard]] constexpr auto heightSampleIndex(std::uint32_t x, std::uint32_t z)
    -> std::uint32_t {
    return z * kHeightSamples + x;
}
[[nodiscard]] auto generateHeightTile(std::uint64_t worldSeed, ChunkCoord coord,
                                      const HeightPolicyParams& params) -> HeightTile;
// Height and material weights are bilinear. The categorical biome id comes
// from the nearest sample so gameplay and rendering share one explicit rule.
[[nodiscard]] auto sampleHeight(const HeightTile& tile, float localX, float localZ)
    -> std::optional<HeightSample>;
[[nodiscard]] auto bilinearHeight(const HeightTile& tile, float localX, float localZ)
    -> float;
[[nodiscard]] auto heightNormal(std::int32_t globalX, std::int32_t globalZ,
                                const std::function<float(std::int32_t, std::int32_t)>& fetch)
    -> math::Vec3;

struct HeightPolicy {
    using Cell = HeightSample;
    using Cells = HeightTile;
    using Params = HeightPolicyParams;
    static constexpr PolicyKind kind = PolicyKind::Height;
    static constexpr std::uint32_t chunkExtent = kHeightQuads;
    [[nodiscard]] static auto generate(std::uint64_t seed, ChunkCoord coord,
                                       const Params& params) -> Cells {
        return generateHeightTile(seed, coord, params);
    }
};

inline constexpr std::uint8_t kInvalidTerrain = 0xffu;
inline constexpr std::uint32_t kInvalidOwner = 0xffffffffu;
struct HexCell {
    std::uint8_t terrainType = kInvalidTerrain;
    std::uint32_t owner = kInvalidOwner;
    friend constexpr auto operator==(const HexCell&, const HexCell&) -> bool = default;
};
inline constexpr HexCell kInvalidCell{};

struct HexTile {
    std::uint32_t radius = 0;
    std::uint32_t width = 0;
    containers::Array<HexCell> cells;
    containers::HashMap<HexCornerCoord, float> cornerHeights;

    [[nodiscard]] auto contains(AxialCoord coordinate) const -> bool;
    [[nodiscard]] auto index(AxialCoord coordinate) const -> std::optional<std::uint32_t>;
    [[nodiscard]] auto corner(AxialCoord coordinate, std::uint32_t cornerIndex) const
        -> std::optional<float>;
    [[nodiscard]] auto centerHeight(AxialCoord coordinate) const -> std::optional<float>;
};

inline constexpr std::array<AxialCoord, 6> kHexDirections{{
    {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}}};
[[nodiscard]] auto hexDistance(AxialCoord a, AxialCoord b) -> std::uint32_t;
[[nodiscard]] auto hexCorner(AxialCoord cell, std::uint32_t cornerIndex)
    -> HexCornerCoord;
[[nodiscard]] auto hexRing(AxialCoord center, std::uint32_t radius)
    -> containers::Array<AxialCoord>;
[[nodiscard]] auto hexSpiral(AxialCoord center, std::uint32_t radius)
    -> containers::Array<AxialCoord>;
[[nodiscard]] auto hexLine(AxialCoord from, AxialCoord to)
    -> containers::Array<AxialCoord>;
[[nodiscard]] auto generateHexTile(std::uint64_t worldSeed, ChunkCoord coord,
                                   const HexPolicyParams& params) -> HexTile;

struct HexPolicy {
    using Cell = HexCell;
    using Cells = HexTile;
    using Params = HexPolicyParams;
    static constexpr PolicyKind kind = PolicyKind::Hex;
    [[nodiscard]] static auto generate(std::uint64_t seed, ChunkCoord coord,
                                       const Params& params) -> Cells {
        return generateHexTile(seed, coord, params);
    }
};

struct CellValue {
    PolicyKind kind = PolicyKind::Voxel;
    std::uint16_t voxel = 0;
    HeightSample height{};
    HexCell hex{};

    [[nodiscard]] static auto voxelValue(std::uint16_t value) -> CellValue;
    [[nodiscard]] static auto heightValue(const HeightSample& value) -> CellValue;
    [[nodiscard]] static auto hexValue(const HexCell& value) -> CellValue;
    friend constexpr auto operator==(const CellValue&, const CellValue&) -> bool = default;
};

struct ChunkDelta {
    std::uint32_t index = 0;
    CellValue value;
    friend constexpr auto operator==(const ChunkDelta&, const ChunkDelta&) -> bool = default;
};

using ChunkBase = std::variant<std::monostate, VoxelCells, HeightTile, HexTile>;

struct ChunkSnapshot {
    ChunkCoord coord;
    std::uint64_t worldSeed = 0;
    PolicyKind policy = PolicyKind::Voxel;
    PolicyParams params;
    ChunkState state = ChunkState::Invalid;
    DirtyFlags dirty;
    std::uint64_t version = 0;
    ChunkBase base;
    containers::Array<ChunkDelta> deltas;
};

class RegionMap {
public:
    explicit RegionMap(std::int32_t tileExtentMeters = 512,
                       PolicyKind defaultPolicy = PolicyKind::Height);
    [[nodiscard]] auto regionFor(std::int32_t worldX, std::int32_t worldZ) const
        -> RegionCoord;
    [[nodiscard]] auto policyAt(std::int32_t worldX, std::int32_t worldZ) const
        -> PolicyKind;
    void set(RegionCoord coord, PolicyKind policy);
    auto erase(RegionCoord coord) -> bool;
    [[nodiscard]] auto tileExtentMeters() const -> std::int32_t { return tileExtentMeters_; }

private:
    std::int32_t tileExtentMeters_ = 512;
    PolicyKind defaultPolicy_ = PolicyKind::Height;
    containers::HashMap<RegionCoord, PolicyKind> regions_;
};

class ChunkStore {
public:
    explicit ChunkStore(std::size_t capacity = 1024);
    [[nodiscard]] auto create(ChunkCoord coord, std::uint64_t worldSeed,
                              PolicyKind policy, const PolicyParams& params = {})
        -> ChunkHandle;
    auto destroy(ChunkHandle handle) -> bool;
    [[nodiscard]] auto find(ChunkCoord coord) const -> ChunkHandle;
    [[nodiscard]] auto state(ChunkHandle handle) const -> ChunkState;
    [[nodiscard]] auto dirty(ChunkHandle handle) const -> DirtyFlags;
    [[nodiscard]] auto version(ChunkHandle handle) const -> std::uint64_t;
    [[nodiscard]] auto liveCount() const -> std::size_t;

    auto queue(ChunkHandle handle) -> bool;
    auto generate(ChunkHandle handle) -> bool;
    auto regenerate(ChunkHandle handle) -> bool;
    auto edit(ChunkHandle handle, std::uint32_t index, const CellValue& value) -> bool;
    [[nodiscard]] auto cell(ChunkHandle handle, std::uint32_t index) const
        -> std::optional<CellValue>;
    [[nodiscard]] auto snapshot(ChunkHandle handle) const -> std::optional<ChunkSnapshot>;
    auto markDirty(ChunkHandle handle, DirtyFlags flags) -> bool;
    auto markMeshed(ChunkHandle handle, std::uint64_t snapshotVersion) -> bool;

    // Untouched chunks produce an empty stream. Edited chunks use the M2
    // preamble/chunk framing and serialize deltas in sorted index order.
    auto saveDeltas(ChunkHandle handle, memory::MemoryWriter& output) const -> bool;
    auto loadDeltas(ChunkHandle handle, const memory::Array<std::byte>& input) -> bool;

private:
    struct ChunkRecord {
        ChunkCoord coord;
        std::uint64_t worldSeed = 0;
        PolicyKind policy = PolicyKind::Voxel;
        PolicyParams params;
        ChunkState state = ChunkState::Invalid;
        DirtyFlags dirty;
        std::uint64_t version = 0;
        ChunkBase base;
        containers::HashMap<std::uint32_t, CellValue> deltas;
        mutable std::mutex mutex;
    };
    [[nodiscard]] auto validCell(const ChunkRecord& chunk, std::uint32_t index,
                                 const CellValue& value) const -> bool;

    ids::GenerationalArena<ChunkRecord, ChunkTag> chunks_;
    containers::HashMap<ChunkCoord, ChunkHandle> byCoord_;
};

} // namespace f4::world
