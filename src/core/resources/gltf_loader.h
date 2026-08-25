#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/containers/array.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/result.h"

namespace f4::resources {

using containers::Array;

// Result of loading a glTF file: one or more meshes extracted.
struct GltfScene {
    Array<Mesh> meshes;
};

// Parse a glTF 2.0 JSON string or GLB binary blob from memory.
// Extracts all meshes with position, normal, and texcoord0 attributes.
// Bounds are computed automatically via computeBounds().
[[nodiscard]] auto loadGltf(std::span<const std::byte> bytes)
    -> Result<GltfScene, LoadError>;

} // namespace f4::resources
