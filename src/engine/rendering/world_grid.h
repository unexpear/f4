#pragma once

#include <cstdint>

#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"

namespace f4::engine {

// WorldGrid: a scalable Cartesian grid representing the world coordinate space.
// Renders graticule lines on all three principal planes (XY, XZ, YZ) plus the
// three axes colored X=red, Y=green, Z=blue, all anchored at the world origin.
//
// It owns only GPU state; everything is drawn in world space (identity model),
// so the grid is the fixed background/reference the engine uses to reason about
// 3D positioning. Caller owns the device lifetime (grid must be destroyed first).
enum WorldGridPart {
    WorldGridPlanes = 1 << 0,
    WorldGridAxes   = 1 << 1,
};

struct WorldGridDesc {
    float extent = 5.0f;      // half-size of the grid volume (world units)
    std::uint32_t divisions = 10;   // cells per axis
    float lineThickness = 0.06f;    // half-width of a grid line (world units)
    std::uint32_t parts = WorldGridPlanes | WorldGridAxes;
    float cellBrightness = 0.6f;
};

class WorldGrid {
public:
    WorldGrid() = default;
    ~WorldGrid() = default;
    WorldGrid(const WorldGrid&) = delete;
    WorldGrid& operator=(const WorldGrid&) = delete;

    // Create GPU resources. Must be called once with a live device.
    [[nodiscard]] auto create(gfx::IDevice& device, const WorldGridDesc& desc) -> gfx::GfxError;

    // Release GPU resources. Safe to call when not created.
    void destroy(gfx::IDevice& device);

    [[nodiscard]] bool valid() const { return vb_.isValid() && ib_.isValid(); }
    [[nodiscard]] auto indexCount() const -> std::uint32_t { return indexCount_; }

    // Rebuild geometry for a new extent/divisions (throttle: only when changed).
    [[nodiscard]] auto setScale(gfx::IDevice& device, const WorldGridDesc& desc) -> gfx::GfxError;

    // Draw the grid. `viewProj` is the camera's combined matrix; the grid uses an
    // identity model so it stays pinned to world space.
    void render(gfx::CommandBuffer& cb, const math::Mat4& viewProj);

private:
    [[nodiscard]] auto rebuild(gfx::IDevice& device) -> gfx::GfxError;

    gfx::BufferHandle vb_{};
    gfx::BufferHandle ib_{};
    gfx::BufferHandle ubo_{};
    gfx::PipelineHandle pipeline_{};
    gfx::ShaderHandle vs_{};
    gfx::ShaderHandle fs_{};
    gfx::IDevice* device_ = nullptr;
    std::uint32_t indexCount_ = 0;
    WorldGridDesc desc_{};
    bool inited_ = false;
};

} // namespace f4::engine
