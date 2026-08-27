#pragma once

#include <cstddef>
#include <span>

#include "core/containers/array.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/result.h"

namespace f4::resources {

using containers::Array;

struct UsdScene {
    Array<Mesh> meshes;
};

[[nodiscard]] auto loadUsd(std::span<const std::byte> bytes)
    -> Result<UsdScene, LoadError>;

} // namespace f4::resources
