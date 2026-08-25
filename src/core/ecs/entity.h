#pragma once

#include "core/ids/handle.h"

namespace f4::ecs {

// Entity identifiers are the proven M2 generational handles: {index,
// generation}, null = index 0xFFFFFFFF. The tag keeps entity ids from ever
// being confused with other handle families (meshes, textures, ...).
struct EntityTag {};
using Entity = ids::Handle<EntityTag>;

} // namespace f4::ecs