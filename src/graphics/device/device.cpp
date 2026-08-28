#include "graphics/device/device.h"

#include "core/assert.h"

namespace f4::gfx {

auto createDevice() -> IDevice* {
#if defined(F4_GRAPHICS_BACKEND_null)
    extern auto createNullDevice() -> IDevice*;
    return createNullDevice();
#else
    // No backend selected in this build; nothing to instantiate.
    F4_VERIFY(false, "F4_GRAPHICS_BACKEND does not resolve to a compiled backend");
    return nullptr;
#endif
}

void deleteDevice(IDevice* device) {
    delete device;
}

} // namespace f4::gfx
