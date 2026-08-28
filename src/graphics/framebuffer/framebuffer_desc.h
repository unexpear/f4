#pragma once

#include <cstdint>

#include "core/containers/array.h"

#include "graphics/handle.h"
#include "graphics/types.h"

namespace f4::gfx {

using containers::Array;

enum class AttachmentType : std::uint8_t {
    Color,
    Depth,
};

struct AttachmentDesc {
    AttachmentType type = AttachmentType::Color;
    TextureHandle texture{};      // null means "from the swapchain image"
    bool clearOnLoad = false;
    ClearValue clear;
};

struct FramebufferDesc {
    Extent extent;
    Array<AttachmentDesc> attachments;
};

struct GpuFramebuffer {
    Extent extent;
    Array<AttachmentDesc> attachments;
};

} // namespace f4::gfx
