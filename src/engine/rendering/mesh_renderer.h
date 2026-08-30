#pragma once

#include <cstdint>

#include "core/resources/resource_ids.h"

namespace f4::engine {

// ECS component: lightweight handle pair, no GPU ownership. The renderer
// translation layer (engine/rendering/renderer) owns the GPU representation
// (BufferHandle/PipelineHandle) and validates staleness each frame.
struct MeshRenderer {
    resources::MeshHandle mesh{};
    resources::MaterialHandle material{};
    std::uint32_t subMeshIndex = 0;
    bool castShadows = false; // unused in M8
    bool transparent = false; // drives RenderQueue bucket
};

} // namespace f4::engine
