#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/ecs/entity.h"
#include "core/resources/resource_ids.h"
#include "engine/rendering/mesh_renderer.h"

namespace f4::engine {

using containers::Array;

struct RenderItem {
    ecs::Entity entity{};
    MeshRenderer meshRenderer{};
    resources::MaterialHandle material{};
    float depth = 0.f; // view-space depth (smaller = nearer)
    bool transparent = false;
    std::uint32_t pipelineSortKey = 0; // material pipeline index for batching
};

class RenderQueue {
public:
    void clear() { items_.clear(); }
    void push(const RenderItem& item) { items_.push(item); }
    void push(RenderItem&& item) { items_.push(static_cast<RenderItem&&>(item)); }

    // Sorts: opaque front-to-back, transparent back-to-front.
    // Opaque group sorted by pipeline key then depth; transparent by depth descending.
    void sort();

    [[nodiscard]] auto items() const -> const Array<RenderItem>& { return items_; }
    [[nodiscard]] auto items() -> Array<RenderItem>& { return items_; }
    [[nodiscard]] auto count() const -> std::size_t { return items_.size(); }
    [[nodiscard]] auto isEmpty() const -> bool { return items_.isEmpty(); }

private:
    Array<RenderItem> items_;
};

} // namespace f4::engine
