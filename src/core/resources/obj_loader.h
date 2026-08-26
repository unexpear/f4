#pragma once

#include <cstddef>
#include <span>

#include "core/containers/array.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/result.h"

namespace f4::resources {

using containers::Array;

struct ObjScene {
    Array<Mesh> meshes;
};

[[nodiscard]] auto loadObj(std::span<const std::byte> bytes)
    -> Result<ObjScene, LoadError>;

} // namespace f4::resources
