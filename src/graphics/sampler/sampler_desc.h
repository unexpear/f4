#pragma once

#include <cstdint>

#include "graphics/types.h"

namespace f4::gfx {

// Bilinear clamp-to-edge LDR sampler is the engine default.
struct SamplerDesc {
    TextureFilter filter = TextureFilter::MinMagLinear;
    MipFilter mipFilter = MipFilter::None;
    AddressMode addressU = AddressMode::ClampToEdge;
    AddressMode addressV = AddressMode::ClampToEdge;
    AddressMode addressW = AddressMode::ClampToEdge;
    float maxAnisotropy = 1.0f;
};

struct GpuSampler {
    TextureFilter filter = TextureFilter::MinMagLinear;
    MipFilter mipFilter = MipFilter::None;
    AddressMode addressU = AddressMode::ClampToEdge;
    AddressMode addressV = AddressMode::ClampToEdge;
    AddressMode addressW = AddressMode::ClampToEdge;
    float maxAnisotropy = 1.0f;
};

} // namespace f4::gfx
