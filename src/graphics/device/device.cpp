#include "graphics/device/device.h"

#include "core/assert.h"
#if defined(F4_GRAPHICS_BACKEND_vulkan)
#include "backends/vulkan/vulkan_device.h"
#endif
#if defined(F4_GRAPHICS_BACKEND_dx12)
#include "backends/dx12/dx12_device.h"
#endif

namespace f4::gfx {

auto createDevice() -> IDevice* {
#if defined(F4_GRAPHICS_BACKEND_null)
    extern auto createNullDevice() -> IDevice*;
    return createNullDevice();
#elif defined(F4_GRAPHICS_BACKEND_opengl)
    extern auto createOpenGLDevice() -> IDevice*;
    return createOpenGLDevice();
#elif defined(F4_GRAPHICS_BACKEND_vulkan)
    return createVulkanDevice();
#elif defined(F4_GRAPHICS_BACKEND_dx12)
    return createDx12Device();
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
