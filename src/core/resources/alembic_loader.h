#pragma once

#include <cstddef>
#include <span>

#include "core/containers/array.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/result.h"

namespace f4::resources {

using containers::Array;

struct AlembicScene {
    Array<Mesh> meshes;
};

[[nodiscard]] auto loadAlembic(std::span<const std::byte> bytes)
    -> Result<AlembicScene, LoadError>;

} // namespace f4::resources
