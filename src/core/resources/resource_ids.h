#pragma once

#include "core/ids/handle.h"
#include "core/resources/buffer.h"
#include "core/resources/mesh.h"
#include "core/resources/shader.h"
#include "core/resources/texture.h"

namespace f4::resources {

// Handle families. One tag per resource kind; tags are mutually incompatible
// at compile time (see ids/handle.h). MaterialTag arrives with M8 (plan M5).
struct MeshTag;
struct TextureTag;
struct ShaderTag;
struct BufferTag;
struct PipelineTag;

using MeshHandle = ids::Handle<MeshTag>;
using TextureHandle = ids::Handle<TextureTag>;
using ShaderHandle = ids::Handle<ShaderTag>;
using BufferHandle = ids::Handle<BufferTag>;
using PipelineHandle = ids::Handle<PipelineTag>; // descriptor arrives with M6

// Record type bound to each tag - lets ResourceCache::create<Tag>() deduce
// its payload instead of asking callers to repeat it.
template <typename Tag>
struct ResourceTrait;

template <>
struct ResourceTrait<MeshTag> {
    using type = Mesh;
};
template <>
struct ResourceTrait<TextureTag> {
    using type = Texture;
};
template <>
struct ResourceTrait<ShaderTag> {
    using type = ShaderSource;
};
template <>
struct ResourceTrait<BufferTag> {
    using type = BufferDesc;
};

} // namespace f4::resources