#include "graphics/handle.h"

// Translation-unit anchor for the gfx handle family. All handle machinery is
// header-only (ids::Handle<Tag> + ids::GenerationalArena); this TU exists so
// the graphics library has a stable unit and a home for any future out-of-line
// symbols shared across backends.
namespace f4::gfx {} // namespace f4::gfx
