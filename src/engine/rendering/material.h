#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/containers/hashmap.h"
#include "core/ids/generational_arena.h"
#include "core/math/vec4.h"
#include "core/resources/resource_ids.h"
#include "graphics/device/device.h"
#include "graphics/gfx_error.h"

namespace f4::engine {

using containers::Array;
using containers::HashMap;

// StandardMaterial v1: albedo texture + tint + MVP UBO. No PBR in M8.
// `albedo` may be invalid (no texture) — renderer falls back to a 1x1 white
// texture so the shader path stays uniform (no variant).
struct StandardMaterialDesc {
    math::Vec4 tint{1.f, 1.f, 1.f, 1.f};
    resources::TextureHandle albedo{}; // resources::TextureHandle (may be invalid)
    bool transparent = false;

    bool operator==(const StandardMaterialDesc& o) const {
        return tint == o.tint && albedo == o.albedo && transparent == o.transparent;
    }
    bool operator!=(const StandardMaterialDesc& o) const { return !(*this == o); }
};

// Concrete material record owned by the cache.
struct StandardMaterial {
    StandardMaterialDesc desc{};
    gfx::PipelineHandle pipeline{};
    gfx::ShaderHandle vs{};
    gfx::ShaderHandle fs{};
};

// Simple cache: MaterialHandle -> StandardMaterial, deduped by desc.
// Owns the GPU Pipeline/Shader lifetime; Renderer drives eviction (clear/destroy).
class MaterialCache {
public:
    explicit MaterialCache() : arena_(64) {}

    [[nodiscard]] auto handleIsAlive(resources::MaterialHandle h) const -> bool {
        return arena_.contains(h);
    }

    [[nodiscard]] auto get(resources::MaterialHandle h) -> StandardMaterial* {
        return arena_.get(h);
    }
    [[nodiscard]] auto get(resources::MaterialHandle h) const -> const StandardMaterial* {
        return arena_.get(h);
    }

    // Find an existing material with identical desc; returns invalid handle if none.
    [[nodiscard]] auto find(const StandardMaterialDesc& desc) const -> resources::MaterialHandle;

    // Get existing or create a new StandardMaterial (pipeline lazily created by
    // caller via ensurePipeline). If newly created, returns a fresh handle with
    // empty pipeline (caller must create shaders/pipeline and assign).
    auto getOrCreate(const StandardMaterialDesc& desc) -> resources::MaterialHandle;

    // Ensure the material's shaders/pipeline exist. No-op if already created.
    // Creates a tint*albedo StandardMaterial pipeline (VS/FS) on IDevice.
    auto ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle h) -> gfx::GfxError;

    auto destroy(resources::MaterialHandle h, gfx::IDevice& dev) -> gfx::GfxError;

    void clear(gfx::IDevice& dev);

    [[nodiscard]] auto liveCount() const -> std::size_t { return arena_.liveCount(); }

private:
    [[nodiscard]] static auto hashDesc(const StandardMaterialDesc& d) -> std::uint64_t;
    ids::GenerationalArena<StandardMaterial, resources::MaterialTag> arena_;
    // Fast dedup: hash(desc) -> handle. Collision handled by equality check; on
    // hash clash we linear probe via find(). Simple and M8-adequate.
    HashMap<std::uint64_t, resources::MaterialHandle> lookup_;
};

} // namespace f4::engine
