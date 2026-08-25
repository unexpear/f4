#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/containers/string.h"

namespace f4::resources {

using containers::Array;
using containers::String;

enum class ShaderStage : std::uint8_t {
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessControl,
    TessEvaluation,
};

// Opaque source/binary blob (plan §7: shaders stay opaque behind handles).
struct ShaderSource {
    ShaderStage stage = ShaderStage::Vertex;
    String entryPoint{"main"};
    Array<std::byte> blob;
};

} // namespace f4::resources