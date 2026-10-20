#include "backends/opengl/opengl_device.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "core/assert.h"
#include "core/logging/logger.h"
#include "graphics/device/object_store.h"

namespace f4::gfx {

namespace {

// Per-family records: neutral Gpu* record plus the GL object id.
struct GlBuffer {
    GpuBuffer rec;
    GLuint id = 0;
    GLenum target = GL_ARRAY_BUFFER;
};

struct GlTexture {
    GpuTexture rec;
    GLuint id = 0;
};

struct GlSampler {
    GpuSampler rec;
    GLuint id = 0;
};

struct GlShader {
    GpuShader rec;
    GLuint id = 0; // GL vertex/fragment shader object
};

struct GlPipeline {
    GpuPipeline rec;
    GLuint program = 0;
    GLuint vao = 0;
    GLenum mode = GL_TRIANGLES;
};

struct GlSwapchain {
    GpuSwapchain rec;
};

struct GlFramebuffer {
    GpuFramebuffer rec;
    GLuint fbo = 0; // 0 = default (window) framebuffer
};

struct GlColorState {
    bool blendEnabled = false;
    GLenum srcColor = GL_ONE;
    GLenum dstColor = GL_ZERO;
    GLenum srcAlpha = GL_ONE;
    GLenum dstAlpha = GL_ZERO;
    GLenum colorOp = GL_FUNC_ADD;
    GLenum alphaOp = GL_FUNC_ADD;
    std::uint32_t writeMask = ColorWriteMask::All;
};

[[nodiscard]] auto glTextureInternal(TextureFormat format) -> GLenum {
    switch (format) {
    case TextureFormat::RGBA8_UNORM: return GL_RGBA8;
    case TextureFormat::BGRA8_UNORM: return GL_RGBA8; // swizzled by shader read
    case TextureFormat::R8_UNORM: return GL_R8;
    case TextureFormat::RG8_UNORM: return GL_RG8;
    case TextureFormat::R16_FLOAT: return GL_R16F;
    case TextureFormat::RG16_FLOAT: return GL_RG16F;
    case TextureFormat::RGBA16_FLOAT: return GL_RGBA16F;
    case TextureFormat::RGBA32_FLOAT: return GL_RGBA32F;
    case TextureFormat::Depth32F: return GL_DEPTH_COMPONENT32F;
    case TextureFormat::Depth24Stencil8: return GL_DEPTH24_STENCIL8;
    }
    return GL_RGBA8;
}

[[nodiscard]] auto glTopology(PrimitiveTopology topology) -> GLenum {
    switch (topology) {
    case PrimitiveTopology::PointList: return GL_POINTS;
    case PrimitiveTopology::LineList: return GL_LINES;
    case PrimitiveTopology::TriangleList: return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    }
    return GL_TRIANGLES;
}

[[nodiscard]] auto glIndexType(IndexFormat format) -> GLenum {
    return format == IndexFormat::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}

// External pixel format (the base/format and type pair used for uploads and
// readbacks). Every TextureFormat must map to a legal (format, type) for
// glTexSubImage2D/glReadPixels.
[[nodiscard]] auto glPixelFormat(TextureFormat format) -> GLenum {
    switch (format) {
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::RGBA16_FLOAT:
    case TextureFormat::RGBA32_FLOAT: return GL_RGBA;
    case TextureFormat::R8_UNORM:
    case TextureFormat::R16_FLOAT: return GL_RED;
    case TextureFormat::RG8_UNORM:
    case TextureFormat::RG16_FLOAT: return GL_RG;
    case TextureFormat::Depth32F: return GL_DEPTH_COMPONENT;
    case TextureFormat::Depth24Stencil8: return GL_DEPTH_STENCIL;
    }
    return GL_RGBA;
}

[[nodiscard]] auto glPixelType(TextureFormat format) -> GLenum {
    switch (format) {
    case TextureFormat::R16_FLOAT:
    case TextureFormat::RG16_FLOAT:
    case TextureFormat::RGBA16_FLOAT: return GL_HALF_FLOAT;
    case TextureFormat::RGBA32_FLOAT:
    case TextureFormat::Depth32F: return GL_FLOAT;
    case TextureFormat::Depth24Stencil8: return GL_UNSIGNED_INT_24_8;
    default: return GL_UNSIGNED_BYTE;
    }
}

[[nodiscard]] auto glAddressMode(AddressMode mode) -> GLenum {
    switch (mode) {
    case AddressMode::Repeat: return GL_REPEAT;
    case AddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
    }
    return GL_CLAMP_TO_EDGE;
}

[[nodiscard]] auto glMinFilter(TextureFilter filter, MipFilter mipFilter) -> GLenum {
    const bool linear = filter == TextureFilter::MinMagLinear ||
                        filter == TextureFilter::MinLinearMagNearest;
    if (mipFilter == MipFilter::Nearest) {
        return linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    }
    if (mipFilter == MipFilter::Linear) {
        return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
    return linear ? GL_LINEAR : GL_NEAREST;
}

[[nodiscard]] auto glMagFilter(TextureFilter filter) -> GLenum {
    return filter == TextureFilter::MinMagLinear ||
                   filter == TextureFilter::MinNearestMagLinear
               ? GL_LINEAR
               : GL_NEAREST;
}

[[nodiscard]] auto glBlendFactor(BlendFactor factor) -> GLenum {
    switch (factor) {
    case BlendFactor::Zero: return GL_ZERO;
    case BlendFactor::One: return GL_ONE;
    case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::SrcColor: return GL_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
    }
    return GL_ONE;
}

[[nodiscard]] auto glBlendOp(BlendOp op) -> GLenum {
    switch (op) {
    case BlendOp::Add: return GL_FUNC_ADD;
    case BlendOp::Sub: return GL_FUNC_SUBTRACT;
    case BlendOp::RevSub: return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Min: return GL_MIN;
    case BlendOp::Max: return GL_MAX;
    }
    return GL_FUNC_ADD;
}

// Bytes one mip level of the given format occupies at `width` x `height`.
[[nodiscard]] auto glMipBytes(const GpuTexture& t, std::uint32_t width, std::uint32_t height)
    -> std::uint64_t {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    switch (t.format) {
    case TextureFormat::R8_UNORM: return 1ull * pixels;
    case TextureFormat::RG8_UNORM:
    case TextureFormat::R16_FLOAT: return 2ull * pixels;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::RG16_FLOAT:
    case TextureFormat::Depth32F: return 4ull * pixels;
    case TextureFormat::RGBA16_FLOAT: return 8ull * pixels;
    case TextureFormat::RGBA32_FLOAT: return 16ull * pixels;
    case TextureFormat::Depth24Stencil8: return 4ull * pixels;
    }
    return 0;
}

[[nodiscard]] auto glCompare(CompareOp op) -> GLenum {
    switch (op) {
    case CompareOp::Never: return GL_NEVER;
    case CompareOp::Less: return GL_LESS;
    case CompareOp::Equal: return GL_EQUAL;
    case CompareOp::LessEqual: return GL_LEQUAL;
    case CompareOp::Greater: return GL_GREATER;
    case CompareOp::NotEqual: return GL_NOTEQUAL;
    case CompareOp::GreaterEqual: return GL_GEQUAL;
    case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_LESS;
}

[[nodiscard]] auto glCull(CullMode mode) -> GLenum {
    switch (mode) {
    case CullMode::None: return GL_NONE;
    case CullMode::Front: return GL_FRONT;
    case CullMode::Back: return GL_BACK;
    }
    return GL_BACK;
}

[[nodiscard]] auto glVertexFormatComponents(VertexFormat format) -> GLint {
    switch (format) {
    case VertexFormat::Float32x2: return 2;
    case VertexFormat::Float32x3: return 3;
    case VertexFormat::Float32x4: return 4;
    case VertexFormat::UNorm8x4: return 4;
    }
    return 0;
}

[[nodiscard]] auto glVertexFormatType(VertexFormat format) -> GLenum {
    switch (format) {
    case VertexFormat::Float32x2:
    case VertexFormat::Float32x3:
    case VertexFormat::Float32x4: return GL_FLOAT;
    case VertexFormat::UNorm8x4: return GL_UNSIGNED_BYTE;
    }
    return GL_FLOAT;
}

[[nodiscard]] auto glVertexFormatNormalized(VertexFormat format) -> GLboolean {
    return format == VertexFormat::UNorm8x4 ? GL_TRUE : GL_FALSE;
}

// Primitives produced by `count` indices/vertices for the given topology.
[[nodiscard]] auto countPrimitives(PrimitiveTopology topology, std::uint64_t count)
    -> std::uint64_t {
    switch (topology) {
    case PrimitiveTopology::PointList: return count;
    case PrimitiveTopology::LineList: return count / 2;
    case PrimitiveTopology::TriangleList: return count / 3;
    case PrimitiveTopology::TriangleStrip: return count > 2 ? count - 2 : 0;
    }
    return 0;
}

// Compile one shader stage; returns the GL shader object id (0 on failure).
// `log` is always filled with the driver's info log (errors on failure,
// warnings on success) so callers can surface diagnostics through stats/logging.
[[nodiscard]] auto compileShader(GLenum type, const std::string& source, std::string& log) -> GLuint {
    const GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length > 0) {
        log.resize(static_cast<std::size_t>(length));
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        // Trim trailing NUL(s) GL may append.
        while (!log.empty() && log.back() == '\0') {
            log.pop_back();
        }
    }

    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Buffers are uploaded through GL_ARRAY_BUFFER to satisfy their initial data
// regardless of usage; subsequent writeBuffer binds the true usage target.
[[nodiscard]] auto bufferTarget(BufferUsage usage) -> GLenum {
    if (hasUsage(usage, BufferUsage::Index)) {
        return GL_ELEMENT_ARRAY_BUFFER;
    }
    if (hasUsage(usage, BufferUsage::Uniform)) {
        return GL_UNIFORM_BUFFER;
    }
    if (hasUsage(usage, BufferUsage::Storage)) {
        return GL_SHADER_STORAGE_BUFFER;
    }
    return GL_ARRAY_BUFFER;
}

} // namespace

struct OpenGLDevice::Impl {
    ObjectStore<GlBuffer, GpuBufferTag> buffers;
    ObjectStore<GlTexture, GpuTextureTag> textures;
    ObjectStore<GlSampler, GpuSamplerTag> samplers;
    ObjectStore<GlShader, GpuShaderTag> shaders;
    ObjectStore<GlPipeline, GpuPipelineTag> pipelines;
    ObjectStore<GlSwapchain, GpuSwapchainTag> swapchains;
    ObjectStore<GlFramebuffer, GpuFramebufferTag> framebuffers;

    GfxError lastSubmitError_ = GfxError::None;

    // Per-frame render diagnostics, accumulated across submits.
    DeviceStats stats_;

    // Per-submit state machine (mirrors NullDevice validation, real GL).
    bool renderPassActive_ = false;
    GlPipeline* boundPipeline_ = nullptr;
    PrimitiveTopology boundTopology_ = PrimitiveTopology::TriangleList;
    GLuint boundIndexBuffer_ = 0;
    IndexFormat boundIndexFormat_ = IndexFormat::U16;
    GlFramebuffer* boundFramebuffer_ = nullptr;

    // GL state cache — engine-owned GL state is only modified through submit().
    // This avoids redundant state changes when the same pipeline is re-bound.
    GlPipeline* lastPipeline_ = nullptr;
    bool glDepthTest_ = false;
    bool glDepthClamp_ = false;
    bool glCullFace_ = false;
    bool glScissorTest_ = false;
    GLenum glDepthFunc_ = GL_LESS;
    GLboolean glDepthMask_ = GL_TRUE;
    std::vector<GlColorState> glColorStates_;
    std::size_t activeColorStateCount_ = 1;
    GLint maxDrawBuffers_ = 0;
    GLint maxColorAttachments_ = 0;
    GLint maxUniformBufferBindings_ = 0;
    GLint maxStorageBufferBindings_ = 0;
    GLint maxTextureUnits_ = 0;
    GLuint glProgram_ = 0;
    GLuint glVao_ = 0;
};

OpenGLDevice::OpenGLDevice() : impl_(new Impl()) {
    F4_ASSERT(impl_ != nullptr, "OpenGLDevice impl allocation failed");
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &impl_->maxDrawBuffers_);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &impl_->maxColorAttachments_);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &impl_->maxUniformBufferBindings_);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &impl_->maxStorageBufferBindings_);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &impl_->maxTextureUnits_);
    F4_ASSERT(impl_->maxDrawBuffers_ > 0, "OpenGL reports no draw buffers");
    impl_->glColorStates_.resize(static_cast<std::size_t>(impl_->maxDrawBuffers_));
}

OpenGLDevice::~OpenGLDevice() {
    // Destroy any live GL objects so a caller deleting the device mid-stream
    // won't leak GPU objects.
    impl_->buffers.forEach([](GlBuffer* b) { glDeleteBuffers(1, &b->id); });
    impl_->textures.forEach([](GlTexture* t) { glDeleteTextures(1, &t->id); });
    impl_->samplers.forEach([](GlSampler* s) { glDeleteSamplers(1, &s->id); });
    impl_->shaders.forEach([](GlShader* s) { glDeleteShader(s->id); });
    impl_->pipelines.forEach([](GlPipeline* p) {
        if (p->program != 0) {
            glDeleteProgram(p->program);
        }
        if (p->vao != 0) {
            glDeleteVertexArrays(1, &p->vao);
        }
    });
    impl_->framebuffers.forEach([](GlFramebuffer* fb) {
        if (fb->fbo != 0) {
            glDeleteFramebuffers(1, &fb->fbo);
        }
    });
    delete impl_;
}

auto OpenGLDevice::backend() const -> Backend { return Backend::OpenGL; }

auto OpenGLDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    if (desc.sizeBytes == 0 || initialData.size() > desc.sizeBytes) {
        return GfxError::BadDescriptor;
    }
    GlBuffer b;
    b.rec.sizeBytes = desc.sizeBytes;
    b.rec.usage = desc.usage;
    b.target = bufferTarget(desc.usage);
    glGenBuffers(1, &b.id);
    glBindBuffer(b.target, b.id);
    const bool initializesWholeBuffer = initialData.size() == desc.sizeBytes;
    glBufferData(b.target, static_cast<GLsizeiptr>(desc.sizeBytes),
                 initializesWholeBuffer ? initialData.data() : nullptr, GL_STATIC_DRAW);
    if (!initialData.empty() && !initializesWholeBuffer) {
        glBufferSubData(b.target, 0, static_cast<GLsizeiptr>(initialData.size()),
                        initialData.data());
    }
    return impl_->buffers.create(b);
}

auto OpenGLDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height)) {
        return GfxError::BadDescriptor;
    }
    GlTexture t;
    t.rec.format = desc.format;
    t.rec.width = desc.width;
    t.rec.height = desc.height;
    t.rec.mipLevels = desc.mipLevels;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    std::uint32_t width = desc.width;
    std::uint32_t height = desc.height;
    for (std::uint32_t level = 0; level < desc.mipLevels; ++level) {
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), glTextureInternal(desc.format),
                     static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                     glPixelFormat(desc.format), glPixelType(desc.format), nullptr);
        width = width > 1 ? width >> 1u : 1u;
        height = height > 1 ? height >> 1u : 1u;
    }
    // The OpenGL default is 1000. Restrict completeness checks to the mip
    // levels declared by the RHI texture, including valid partial chains.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                    static_cast<GLint>(desc.mipLevels - 1));
    glBindTexture(GL_TEXTURE_2D, 0);
    return impl_->textures.create(t);
}

auto OpenGLDevice::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    GlSampler s;
    s.rec.filter = desc.filter;
    s.rec.mipFilter = desc.mipFilter;
    s.rec.addressU = desc.addressU;
    s.rec.addressV = desc.addressV;
    s.rec.addressW = desc.addressW;
    s.rec.maxAnisotropy = desc.maxAnisotropy;
    glGenSamplers(1, &s.id);
    glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER,
                        static_cast<GLint>(glMinFilter(desc.filter, desc.mipFilter)));
    glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER,
                        static_cast<GLint>(glMagFilter(desc.filter)));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(glAddressMode(desc.addressU)));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(glAddressMode(desc.addressV)));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_R,
                        static_cast<GLint>(glAddressMode(desc.addressW)));
    if (desc.maxAnisotropy > 1.0f) {
        glSamplerParameterf(s.id, GL_TEXTURE_MAX_ANISOTROPY, desc.maxAnisotropy);
    }
    return impl_->samplers.create(s);
}

auto OpenGLDevice::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    GLenum type = 0;
    const char* stageName = nullptr;
    switch (desc.stage) {
    case ShaderStage::Vertex:
        type = GL_VERTEX_SHADER;
        stageName = "vertex";
        break;
    case ShaderStage::Fragment:
        type = GL_FRAGMENT_SHADER;
        stageName = "fragment";
        break;
    case ShaderStage::Compute:
        type = GL_COMPUTE_SHADER;
        stageName = "compute";
        break;
    default: return GfxError::BadDescriptor;
    }
    std::string source(reinterpret_cast<const char*>(desc.blob.data()), desc.blob.size());
    std::string log;
    const GLuint shader = compileShader(type, source, log);
    if (shader == 0) {
        ++impl_->stats_.shaderFailed;
        F4_LOG_ERROR() << "GL " << stageName << " shader failed to compile:\n" << log;
        return GfxError::BadDescriptor;
    }
    ++impl_->stats_.shaderCompiled;
    if (!log.empty()) {
        ++impl_->stats_.shaderWarnings;
        F4_LOG_WARNING() << "GL " << stageName << " shader compiled with warnings:\n" << log;
    }
    GlShader s;
    s.rec.stage = desc.stage;
    s.id = shader;
    return impl_->shaders.create(s);
}

auto OpenGLDevice::createPipeline(const PipelineStateDesc& desc)
    -> Result<PipelineHandle, GfxError> {
    const bool compute = desc.computeShader.isValid();
    const bool hasVertex = desc.vertexShader.isValid();
    const bool hasFragment = desc.fragmentShader.isValid();
    if ((compute && (hasVertex || hasFragment)) ||
        (!compute && (!hasVertex || !hasFragment))) {
        return GfxError::BadDescriptor;
    }
    if (compute && (!desc.vertexInput.bindings.isEmpty() ||
                    !desc.vertexInput.attributes.isEmpty() ||
                    !desc.colorAttachments.isEmpty() ||
                    desc.depthStencil.usesAttachment())) {
        return GfxError::BadDescriptor;
    }
    const std::size_t colorStateCount =
        compute ? 0 : (desc.colorAttachments.isEmpty() ? 1 : desc.colorAttachments.size());
    if ((!compute && colorStateCount > static_cast<std::size_t>(impl_->maxDrawBuffers_)) ||
        desc.layout.pushConstantSizeBytes != 0 || desc.layout.groups.size() > 1) {
        return GfxError::BadDescriptor;
    }
    for (const ColorAttachmentState& color : desc.colorAttachments) {
        if ((color.writeMask & ~ColorWriteMask::All) != 0) {
            return GfxError::BadDescriptor;
        }
    }

    const std::uint32_t allowedStageMask = compute
        ? shaderStageBit(ShaderStage::Compute)
        : shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment);
    if (!desc.layout.groups.isEmpty()) {
        const BindGroup& group = desc.layout.groups[0];
        if (group.bindPoint != 0) {
            return GfxError::BadDescriptor;
        }
        for (std::size_t i = 0; i < group.slots.size(); ++i) {
            const BindingSlot& slot = group.slots[i];
            GLint bindingLimit = 0;
            switch (slot.type) {
            case BindingType::UniformBuffer:
                bindingLimit = impl_->maxUniformBufferBindings_;
                break;
            case BindingType::StorageBuffer:
                bindingLimit = impl_->maxStorageBufferBindings_;
                break;
            case BindingType::Texture:
            case BindingType::Sampler:
                bindingLimit = impl_->maxTextureUnits_;
                break;
            }
            if (slot.count != 1 || slot.slot >= static_cast<std::uint32_t>(bindingLimit) ||
                (slot.stageMask != 0 && (slot.stageMask & ~allowedStageMask) != 0)) {
                return GfxError::BadDescriptor;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (group.slots[j].type == slot.type && group.slots[j].slot == slot.slot) {
                    return GfxError::BadDescriptor;
                }
            }
        }
    }

    GlShader* vs = compute ? nullptr : impl_->shaders.get(desc.vertexShader);
    GlShader* fs = compute ? nullptr : impl_->shaders.get(desc.fragmentShader);
    GlShader* cs = compute ? impl_->shaders.get(desc.computeShader) : nullptr;
    if ((compute && cs == nullptr) || (!compute && (vs == nullptr || fs == nullptr))) {
        return GfxError::InvalidHandle;
    }
    if ((compute && cs->rec.stage != ShaderStage::Compute) ||
        (!compute && (vs->rec.stage != ShaderStage::Vertex ||
                      fs->rec.stage != ShaderStage::Fragment))) {
        return GfxError::BadDescriptor;
    }

    const GLuint program = glCreateProgram();
    if (compute) {
        glAttachShader(program, cs->id);
    } else {
        glAttachShader(program, vs->id);
        glAttachShader(program, fs->id);
    }
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return GfxError::BadDescriptor;
    }

    GLuint vao = 0;
    if (!compute) {
        // Build the VAO from the vertex input layout using the 4.3
        // vertex-attrib binding API.
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        for (const auto& binding : desc.vertexInput.bindings) {
            glBindVertexBuffer(binding.binding, 0, 0, static_cast<GLsizei>(binding.strideBytes));
            glVertexBindingDivisor(binding.binding, static_cast<GLuint>(binding.divisor));
        }
        for (const auto& attribute : desc.vertexInput.attributes) {
            const GLboolean normalized = glVertexFormatNormalized(attribute.format);
            glEnableVertexAttribArray(attribute.location);
            glVertexAttribFormat(attribute.location,
                                 glVertexFormatComponents(attribute.format),
                                 glVertexFormatType(attribute.format), normalized,
                                 static_cast<GLuint>(attribute.offsetBytes));
            glVertexAttribBinding(attribute.location, attribute.binding);
        }
        glBindVertexArray(0);
    }
    // Pipeline construction changes the real VAO binding outside submit().
    // Invalidate the fast-path guard so the next bind restores its full state.
    impl_->glVao_ = 0;
    impl_->lastPipeline_ = nullptr;

    GlPipeline p;
    p.rec.desc = desc;
    p.program = program;
    p.vao = vao;
    p.mode = glTopology(desc.topology);
    return impl_->pipelines.create(p);
}

auto OpenGLDevice::createSwapchain(const SwapchainDesc& desc) -> Result<SwapchainHandle, GfxError> {
    GlSwapchain sw;
    sw.rec.surfaceFormat = desc.surfaceFormat;
    sw.rec.extent = desc.extent;
    sw.rec.presentMode = desc.presentMode;
    return impl_->swapchains.create(sw);
}

auto OpenGLDevice::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        return GfxError::BadDescriptor;
    }

    std::size_t colorAttachmentCount = 0;
    std::size_t depthAttachmentCount = 0;
    for (const AttachmentDesc& attachment : desc.attachments) {
        if (attachment.type == AttachmentType::Color) {
            ++colorAttachmentCount;
        } else {
            ++depthAttachmentCount;
        }
    }
    const GLint colorLimit = std::min(impl_->maxDrawBuffers_, impl_->maxColorAttachments_);
    if (colorAttachmentCount > static_cast<std::size_t>(colorLimit) ||
        depthAttachmentCount > 1) {
        return GfxError::BadDescriptor;
    }

    GlFramebuffer fb;
    fb.rec.extent = desc.extent;
    fb.rec.attachments = desc.attachments;

    // Determine if this is the default framebuffer (all texture handles null).
    bool isDefault = true;
    for (const auto& att : desc.attachments) {
        if (att.texture.isValid()) {
            isDefault = false;
            break;
        }
    }

    if (!isDefault) {
        glGenFramebuffers(1, &fb.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

        std::vector<GLenum> drawBuffers;
        drawBuffers.reserve(colorAttachmentCount);
        std::size_t colorIndex = 0;

        for (const auto& att : desc.attachments) {
            if (!att.texture.isValid()) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fb.fbo);
                return GfxError::BadDescriptor;
            }
            GlTexture* tex = impl_->textures.get(att.texture);
            if (tex == nullptr) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fb.fbo);
                return GfxError::InvalidHandle;
            }
            if (tex->rec.width != desc.extent.width || tex->rec.height != desc.extent.height) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fb.fbo);
                return GfxError::BadDescriptor;
            }

            const bool depthFormat = tex->rec.format == TextureFormat::Depth32F ||
                                     tex->rec.format == TextureFormat::Depth24Stencil8;
            if (att.type == AttachmentType::Color) {
                if (depthFormat) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &fb.fbo);
                    return GfxError::BadDescriptor;
                }
                const GLenum target = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(colorIndex);
                glFramebufferTexture2D(GL_FRAMEBUFFER, target, GL_TEXTURE_2D, tex->id, 0);
                drawBuffers.push_back(target);
                ++colorIndex;
            } else {
                if (!depthFormat) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &fb.fbo);
                    return GfxError::BadDescriptor;
                }
                const GLenum target = tex->rec.format == TextureFormat::Depth24Stencil8
                                          ? GL_DEPTH_STENCIL_ATTACHMENT
                                          : GL_DEPTH_ATTACHMENT;
                glFramebufferTexture2D(GL_FRAMEBUFFER, target, GL_TEXTURE_2D, tex->id, 0);
            }
        }

        if (!drawBuffers.empty()) {
            glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
        } else {
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
        }

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &fb.fbo);
            return GfxError::BadDescriptor;
        }
    }

    return impl_->framebuffers.create(fb);
}

auto OpenGLDevice::destroyBuffer(BufferHandle h) -> GfxError {
    GlBuffer* b = impl_->buffers.get(h);
    if (b == nullptr) {
        return GfxError::StaleHandle;
    }
    glDeleteBuffers(1, &b->id);
    impl_->buffers.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::destroyTexture(TextureHandle h) -> GfxError {
    GlTexture* t = impl_->textures.get(h);
    if (t == nullptr) {
        return GfxError::StaleHandle;
    }
    glDeleteTextures(1, &t->id);
    impl_->textures.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::destroySampler(SamplerHandle h) -> GfxError {
    GlSampler* s = impl_->samplers.get(h);
    if (s == nullptr) {
        return GfxError::StaleHandle;
    }
    glDeleteSamplers(1, &s->id);
    impl_->samplers.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::destroyShader(ShaderHandle h) -> GfxError {
    GlShader* s = impl_->shaders.get(h);
    if (s == nullptr) {
        return GfxError::StaleHandle;
    }
    glDeleteShader(s->id);
    impl_->shaders.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::destroyPipeline(PipelineHandle h) -> GfxError {
    GlPipeline* p = impl_->pipelines.get(h);
    if (p == nullptr) {
        return GfxError::StaleHandle;
    }
    if (impl_->lastPipeline_ == p) {
        impl_->lastPipeline_ = nullptr;
    }
    if (impl_->boundPipeline_ == p) {
        impl_->boundPipeline_ = nullptr;
    }
    if (p->program != 0) {
        glDeleteProgram(p->program);
    }
    if (p->vao != 0) {
        if (impl_->glVao_ == p->vao) {
            impl_->glVao_ = 0;
        }
        glDeleteVertexArrays(1, &p->vao);
    }
    impl_->pipelines.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::destroySwapchain(SwapchainHandle h) -> GfxError {
    if (!impl_->swapchains.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto OpenGLDevice::destroyFramebuffer(FramebufferHandle h) -> GfxError {
    GlFramebuffer* fb = impl_->framebuffers.get(h);
    if (fb == nullptr) {
        return GfxError::StaleHandle;
    }
    if (fb->fbo != 0) {
        glDeleteFramebuffers(1, &fb->fbo);
    }
    impl_->framebuffers.destroy(h);
    return GfxError::None;
}

auto OpenGLDevice::writeBuffer(BufferHandle h, std::uint64_t offset,
                               std::span<const std::byte> data) -> GfxError {
    GlBuffer* b = impl_->buffers.get(h);
    if (b == nullptr) {
        return GfxError::StaleHandle;
    }
    if (offset + data.size() > b->rec.sizeBytes) {
        return GfxError::BadDescriptor;
    }
    glBindBuffer(b->target, b->id);
    // For Uniform buffers (updated every frame), orphan the buffer to avoid
    // GPU pipeline stalls. Other buffer types are typically uploaded once and
    // read by the GPU, so glBufferSubData is fine.
    if (hasUsage(b->rec.usage, BufferUsage::Uniform) ||
        hasUsage(b->rec.usage, BufferUsage::HostWrite)) {
        glBufferData(b->target, static_cast<GLsizeiptr>(b->rec.sizeBytes),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(b->target, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(data.size()), data.data());
    return GfxError::None;
}

auto OpenGLDevice::readBuffer(BufferHandle h, std::uint64_t offset,
                              std::span<std::byte> destination) -> GfxError {
    GlBuffer* buffer = impl_->buffers.get(h);
    if (buffer == nullptr) return GfxError::StaleHandle;
    if (offset > buffer->rec.sizeBytes ||
        destination.size() > buffer->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    if (destination.empty()) return GfxError::None;
    glFinish();
    while (glGetError() != GL_NO_ERROR) {}
    glBindBuffer(buffer->target, buffer->id);
    glGetBufferSubData(buffer->target, static_cast<GLintptr>(offset),
                       static_cast<GLsizeiptr>(destination.size()), destination.data());
    return glGetError() == GL_NO_ERROR ? GfxError::None : GfxError::DeviceLost;
}

auto OpenGLDevice::writeTexture(TextureHandle h, std::uint32_t mipLevel,
                                std::span<const std::byte> data) -> GfxError {
    GlTexture* t = impl_->textures.get(h);
    if (t == nullptr) {
        return GfxError::StaleHandle;
    }
    if (mipLevel >= t->rec.mipLevels) {
        return GfxError::BadDescriptor;
    }
    std::uint32_t w = t->rec.width >> mipLevel;
    std::uint32_t hh = t->rec.height >> mipLevel;
    if (w == 0) {
        w = 1;
    }
    if (hh == 0) {
        hh = 1;
    }
    const std::uint64_t expected = glMipBytes(t->rec, w, hh);
    if (data.size() != expected) {
        return GfxError::BadDescriptor;
    }
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(mipLevel), 0, 0, static_cast<GLsizei>(w),
                    static_cast<GLsizei>(hh), glPixelFormat(t->rec.format),
                    glPixelType(t->rec.format), data.data());
    return GfxError::None;
}

auto OpenGLDevice::createBufferUpload(const BufferDesc& desc, std::span<const std::byte> data)
    -> Result<BufferHandle, GfxError> {
    return createBuffer(desc, data);
}

auto OpenGLDevice::submit(CommandBuffer& cb) -> GfxError {
    // Reset command-validation state. The GL state cache persists across submits
    // because the corresponding driver state persists too.
    impl_->lastSubmitError_ = GfxError::None;
    impl_->renderPassActive_ = false;
    impl_->boundPipeline_ = nullptr;
    impl_->boundIndexBuffer_ = 0;
    impl_->boundFramebuffer_ = nullptr;

    for (const Command& command : cb.commands()) {
        switch (command.kind) {
        case CommandKind::ResourceBarrier:
            if (command.barrierType == BarrierResourceType::Texture) {
                if (!impl_->textures.contains(command.texture)) {
                    impl_->lastSubmitError_ = GfxError::InvalidHandle;
                    return GfxError::InvalidHandle;
                }
            } else if (!impl_->buffers.contains(command.buffer)) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            if ((command.before.access & ResourceAccessMask::ShaderWrite) != 0) {
                GLbitfield barriers = GL_SHADER_STORAGE_BARRIER_BIT;
                if ((command.after.access & (ResourceAccessMask::HostRead |
                                             ResourceAccessMask::HostWrite |
                                             ResourceAccessMask::TransferRead |
                                             ResourceAccessMask::TransferWrite)) != 0) {
                    barriers |= GL_BUFFER_UPDATE_BARRIER_BIT;
                }
                glMemoryBarrier(barriers);
            }
            break;
        case CommandKind::BeginRenderPass: {
            GlFramebuffer* fb = impl_->framebuffers.get(command.framebuffer);
            if (fb == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
            // Scissor is dynamic per pass. Reset it before clears so state from
            // an earlier pass cannot clip this pass's attachments.
            if (impl_->glScissorTest_) {
                glDisable(GL_SCISSOR_TEST);
                impl_->glScissorTest_ = false;
            }
            glViewport(0, 0, static_cast<GLsizei>(fb->rec.extent.width),
                       static_cast<GLsizei>(fb->rec.extent.height));

            bool clearDepth = false;
            std::size_t colorIndex = 0;
            for (const auto& att : fb->rec.attachments) {
                if (att.type == AttachmentType::Color) {
                    if (att.clearOnLoad) {
                        const std::uint32_t restoreMask =
                            impl_->glColorStates_[colorIndex].writeMask;
                        if (restoreMask != ColorWriteMask::All) {
                            glColorMaski(static_cast<GLuint>(colorIndex), GL_TRUE, GL_TRUE,
                                         GL_TRUE, GL_TRUE);
                        }
                        glClearBufferfv(GL_COLOR, static_cast<GLint>(colorIndex), att.clear.color);
                        if (restoreMask != ColorWriteMask::All) {
                            glColorMaski(static_cast<GLuint>(colorIndex),
                                         (restoreMask & ColorWriteMask::R) != 0,
                                         (restoreMask & ColorWriteMask::G) != 0,
                                         (restoreMask & ColorWriteMask::B) != 0,
                                         (restoreMask & ColorWriteMask::A) != 0);
                        }
                    }
                    ++colorIndex;
                } else if (att.type == AttachmentType::Depth && att.clearOnLoad) {
                    clearDepth = true;
                }
            }
            // Depth clears honor the write mask. Overlay pipelines disable
            // depth writes, so temporarily enable them around the clear.
            const bool restoreDepthMask = clearDepth && impl_->glDepthMask_ == GL_FALSE;
            if (restoreDepthMask) {
                glDepthMask(GL_TRUE);
            }
            if (clearDepth) {
                float depthValue = 1.0f;
                for (const auto& att : fb->rec.attachments) {
                    if (att.type == AttachmentType::Depth && att.clearOnLoad) {
                        depthValue = att.clear.depth;
                        break;
                    }
                }
                glClearBufferfv(GL_DEPTH, 0, &depthValue);
            }
            if (restoreDepthMask) {
                glDepthMask(GL_FALSE);
            }

            impl_->renderPassActive_ = true;
            impl_->boundFramebuffer_ = fb;
            break;
        }
        case CommandKind::EndRenderPass: {
            impl_->renderPassActive_ = false;
            impl_->boundFramebuffer_ = nullptr;
            break;
        }
        case CommandKind::BindPipeline: {
            GlPipeline* p = impl_->pipelines.get(command.pipeline);
            if (p == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            const bool computePipeline = p->rec.desc.computeShader.isValid();
            if (impl_->renderPassActive_ && computePipeline) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            // State cache: skip redundant state changes for same-pipeline binds.
            if (p != impl_->lastPipeline_) {
                if (p->program != impl_->glProgram_) {
                    glUseProgram(p->program);
                    impl_->glProgram_ = p->program;
                }
                if (!computePipeline && p->vao != impl_->glVao_) {
                    glBindVertexArray(p->vao);
                    impl_->glVao_ = p->vao;
                }
                if (!computePipeline) {
                    // Cull face.
                    const bool wantCull = (p->rec.desc.raster.cullMode != CullMode::None);
                    if (wantCull != impl_->glCullFace_) {
                        if (wantCull) {
                            glEnable(GL_CULL_FACE);
                        } else {
                            glDisable(GL_CULL_FACE);
                        }
                        impl_->glCullFace_ = wantCull;
                    }
                    if (wantCull) {
                        glCullFace(glCull(p->rec.desc.raster.cullMode));
                    }
                    const bool wantDepthClamp = p->rec.desc.raster.depthClampEnable;
                    if (wantDepthClamp != impl_->glDepthClamp_) {
                        if (wantDepthClamp) {
                            glEnable(GL_DEPTH_CLAMP);
                        } else {
                            glDisable(GL_DEPTH_CLAMP);
                        }
                        impl_->glDepthClamp_ = wantDepthClamp;
                    }
                    // Depth test.
                    const bool wantDepth = p->rec.desc.depthStencil.depthTestEnable;
                    if (wantDepth != impl_->glDepthTest_) {
                        if (wantDepth) {
                            glEnable(GL_DEPTH_TEST);
                        } else {
                            glDisable(GL_DEPTH_TEST);
                        }
                        impl_->glDepthTest_ = wantDepth;
                    }
                    const GLenum wantFunc = glCompare(p->rec.desc.depthStencil.depthCompare);
                    if (wantFunc != impl_->glDepthFunc_) {
                        glDepthFunc(wantFunc);
                        impl_->glDepthFunc_ = wantFunc;
                    }
                    const GLboolean wantMask =
                        p->rec.desc.depthStencil.depthWriteEnable ? GL_TRUE : GL_FALSE;
                    if (wantMask != impl_->glDepthMask_) {
                        glDepthMask(wantMask);
                        impl_->glDepthMask_ = wantMask;
                    }
                    ColorAttachmentState defaultColor;
                    const std::size_t colorStateCount = p->rec.desc.colorAttachments.isEmpty()
                                                            ? 1
                                                            : p->rec.desc.colorAttachments.size();
                    const std::size_t statesToApply =
                        std::max(colorStateCount, impl_->activeColorStateCount_);
                    for (std::size_t i = 0; i < statesToApply; ++i) {
                        const ColorAttachmentState& color =
                            i < colorStateCount ? (p->rec.desc.colorAttachments.isEmpty()
                                                       ? defaultColor
                                                       : p->rec.desc.colorAttachments[i])
                                                : defaultColor;
                        GlColorState& cached = impl_->glColorStates_[i];
                        const GLuint index = static_cast<GLuint>(i);
                        if (color.blendEnable != cached.blendEnabled) {
                            if (color.blendEnable) {
                                glEnablei(GL_BLEND, index);
                            } else {
                                glDisablei(GL_BLEND, index);
                            }
                            cached.blendEnabled = color.blendEnable;
                        }
                        if (color.blendEnable) {
                            const GLenum srcColor = glBlendFactor(color.srcColor);
                            const GLenum dstColor = glBlendFactor(color.dstColor);
                            const GLenum srcAlpha = glBlendFactor(color.srcAlpha);
                            const GLenum dstAlpha = glBlendFactor(color.dstAlpha);
                            if (srcColor != cached.srcColor || dstColor != cached.dstColor ||
                                srcAlpha != cached.srcAlpha || dstAlpha != cached.dstAlpha) {
                                glBlendFuncSeparatei(index, srcColor, dstColor, srcAlpha, dstAlpha);
                                cached.srcColor = srcColor;
                                cached.dstColor = dstColor;
                                cached.srcAlpha = srcAlpha;
                                cached.dstAlpha = dstAlpha;
                            }
                            const GLenum colorOp = glBlendOp(color.colorOp);
                            const GLenum alphaOp = glBlendOp(color.alphaOp);
                            if (colorOp != cached.colorOp || alphaOp != cached.alphaOp) {
                                glBlendEquationSeparatei(index, colorOp, alphaOp);
                                cached.colorOp = colorOp;
                                cached.alphaOp = alphaOp;
                            }
                        }
                        if (color.writeMask != cached.writeMask) {
                            glColorMaski(index, (color.writeMask & ColorWriteMask::R) != 0,
                                         (color.writeMask & ColorWriteMask::G) != 0,
                                         (color.writeMask & ColorWriteMask::B) != 0,
                                         (color.writeMask & ColorWriteMask::A) != 0);
                            cached.writeMask = color.writeMask;
                        }
                    }
                    impl_->activeColorStateCount_ = colorStateCount;
                }
                impl_->lastPipeline_ = p;
            }
            impl_->boundPipeline_ = p;
            impl_->boundTopology_ = p->rec.desc.topology;
            break;
        }
        case CommandKind::BindVertexBuffer: {
            GlBuffer* b = impl_->buffers.get(command.buffer);
            if (b == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            // Binding 0's VAO attrib was configured with glVertexAttribFormat
            // and a vertex buffer binding; (re)bind the GL buffer to it.
            glBindVertexBuffer(command.vertexSlot, b->id, 0,
                               static_cast<GLsizei>(command.strideBytes));
            break;
        }
        case CommandKind::BindIndexBuffer: {
            GlBuffer* b = impl_->buffers.get(command.buffer);
            if (b == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->id);
            impl_->boundIndexBuffer_ = b->id;
            impl_->boundIndexFormat_ = command.indexFormat;
            break;
        }
        case CommandKind::SetViewport: {
            glViewport(0, 0, static_cast<GLsizei>(command.viewport.width),
                       static_cast<GLsizei>(command.viewport.height));
            break;
        }
        case CommandKind::SetScissor: {
            if (!impl_->glScissorTest_) {
                glEnable(GL_SCISSOR_TEST);
                impl_->glScissorTest_ = true;
            }
            glScissor(0, 0, static_cast<GLsizei>(command.scissor.width),
                      static_cast<GLsizei>(command.scissor.height));
            break;
        }
        case CommandKind::Draw: {
            if (impl_->boundPipeline_ == nullptr ||
                impl_->boundPipeline_->rec.desc.computeShader.isValid()) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            if (!impl_->renderPassActive_) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            if (command.instanceCount > 1) {
                glDrawArraysInstanced(impl_->boundPipeline_->mode, 0,
                                      static_cast<GLsizei>(command.vertexCount),
                                      static_cast<GLsizei>(command.instanceCount));
            } else {
                glDrawArrays(impl_->boundPipeline_->mode, 0,
                             static_cast<GLsizei>(command.vertexCount));
            }
            ++impl_->stats_.drawCalls;
            ++impl_->stats_.batches;
            impl_->stats_.instances += command.instanceCount;
            impl_->stats_.vertices +=
                static_cast<std::uint64_t>(command.vertexCount) * command.instanceCount;
            impl_->stats_.primitives +=
                countPrimitives(impl_->boundTopology_, command.vertexCount) * command.instanceCount;
            break;
        }
        case CommandKind::DrawIndexed: {
            if (impl_->boundPipeline_ == nullptr ||
                impl_->boundPipeline_->rec.desc.computeShader.isValid() ||
                impl_->boundIndexBuffer_ == 0) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            if (!impl_->renderPassActive_) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            const void* indexPtr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                command.firstIndex * indexFormatSize(impl_->boundIndexFormat_)));
            if (command.instanceCount > 1) {
                glDrawElementsInstanced(impl_->boundPipeline_->mode,
                                        static_cast<GLsizei>(command.indexCount),
                                        glIndexType(impl_->boundIndexFormat_), indexPtr,
                                        static_cast<GLsizei>(command.instanceCount));
            } else {
                glDrawElements(impl_->boundPipeline_->mode,
                               static_cast<GLsizei>(command.indexCount),
                               glIndexType(impl_->boundIndexFormat_), indexPtr);
            }
            ++impl_->stats_.drawIndexedCalls;
            ++impl_->stats_.batches;
            impl_->stats_.instances += command.instanceCount;
            impl_->stats_.vertices +=
                static_cast<std::uint64_t>(command.indexCount) * command.instanceCount;
            impl_->stats_.primitives +=
                countPrimitives(impl_->boundTopology_, command.indexCount) * command.instanceCount;
            break;
        }
        case CommandKind::Dispatch:
            if (impl_->renderPassActive_ || impl_->boundPipeline_ == nullptr ||
                !impl_->boundPipeline_->rec.desc.computeShader.isValid() ||
                command.groupCountX == 0 || command.groupCountY == 0 ||
                command.groupCountZ == 0) {
                impl_->lastSubmitError_ = GfxError::BadDescriptor;
                return GfxError::BadDescriptor;
            }
            glDispatchCompute(command.groupCountX, command.groupCountY,
                              command.groupCountZ);
            ++impl_->stats_.dispatchCalls;
            break;
        case CommandKind::BindResources: {
            for (std::uint8_t ri = 0; ri < command.resourceCount; ++ri) {
                const BoundResource& res = command.resources[ri];
                if (res.bindPoint != 0) {
                    impl_->lastSubmitError_ = GfxError::BadDescriptor;
                    return GfxError::BadDescriptor;
                }
                if (impl_->boundPipeline_ != nullptr &&
                    !impl_->boundPipeline_->rec.desc.layout.groups.isEmpty()) {
                    bool declared = false;
                    for (const BindingSlot& slot :
                         impl_->boundPipeline_->rec.desc.layout.groups[0].slots) {
                        declared = declared || (slot.type == res.type && slot.slot == res.slot);
                    }
                    if (!declared) {
                        impl_->lastSubmitError_ = GfxError::BadDescriptor;
                        return GfxError::BadDescriptor;
                    }
                }
                if (res.type == BindingType::Texture) {
                    if (res.slot >= static_cast<std::uint32_t>(impl_->maxTextureUnits_)) {
                        impl_->lastSubmitError_ = GfxError::BadDescriptor;
                        return GfxError::BadDescriptor;
                    }
                    TextureHandle th;
                    th.index = res.handleIndex;
                    th.generation = res.handleGeneration;
                    GlTexture* t = impl_->textures.get(th);
                    if (t == nullptr) {
                        impl_->lastSubmitError_ = GfxError::InvalidHandle;
                        return GfxError::InvalidHandle;
                    }
                    // Convention: a texture at slot N lives on texture unit N.
                    const GLenum unit = GL_TEXTURE0 + res.slot;
                    glActiveTexture(unit);
                    glBindTexture(GL_TEXTURE_2D, t->id);
                    // Point the fragment sampler uniform at this unit. Sampler
                    // names follow the convention uTex (slot 0) / uTex<N>.
                    if (impl_->glProgram_ != 0) {
                        const std::string uniformName =
                            res.slot == 0 ? "uTex" : "uTex" + std::to_string(res.slot);
                        const GLint loc =
                            glGetUniformLocation(impl_->glProgram_, uniformName.c_str());
                        if (loc >= 0) {
                            glUniform1i(loc, static_cast<GLint>(res.slot));
                        }
                    }
                } else if (res.type == BindingType::Sampler) {
                    if (res.slot >= static_cast<std::uint32_t>(impl_->maxTextureUnits_)) {
                        impl_->lastSubmitError_ = GfxError::BadDescriptor;
                        return GfxError::BadDescriptor;
                    }
                    SamplerHandle sh;
                    sh.index = res.handleIndex;
                    sh.generation = res.handleGeneration;
                    GlSampler* s = impl_->samplers.get(sh);
                    if (s == nullptr) {
                        impl_->lastSubmitError_ = GfxError::InvalidHandle;
                        return GfxError::InvalidHandle;
                    }
                    glBindSampler(res.slot, s->id);
                } else {
                    BufferHandle bh;
                    bh.index = res.handleIndex;
                    bh.generation = res.handleGeneration;
                    GlBuffer* b = impl_->buffers.get(bh);
                    if (b == nullptr) {
                        impl_->lastSubmitError_ = GfxError::InvalidHandle;
                        return GfxError::InvalidHandle;
                    }
                    if (res.type == BindingType::UniformBuffer &&
                        hasUsage(b->rec.usage, BufferUsage::Uniform)) {
                        if (res.slot >=
                            static_cast<std::uint32_t>(impl_->maxUniformBufferBindings_)) {
                            impl_->lastSubmitError_ = GfxError::BadDescriptor;
                            return GfxError::BadDescriptor;
                        }
                        glBindBufferBase(GL_UNIFORM_BUFFER, res.slot, b->id);
                    } else if (res.type == BindingType::StorageBuffer &&
                               hasUsage(b->rec.usage, BufferUsage::Storage)) {
                        if (res.slot >=
                            static_cast<std::uint32_t>(impl_->maxStorageBufferBindings_)) {
                            impl_->lastSubmitError_ = GfxError::BadDescriptor;
                            return GfxError::BadDescriptor;
                        }
                        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, res.slot, b->id);
                    } else {
                        impl_->lastSubmitError_ = GfxError::BadDescriptor;
                        return GfxError::BadDescriptor;
                    }
                }
            }
            break;
        }
        }
    }
    return GfxError::None;
}

auto OpenGLDevice::waitIdle() -> void { glFinish(); }

auto OpenGLDevice::stats() const -> DeviceStats {
    DeviceStats s = impl_->stats_;
    s.bufferCount = impl_->buffers.liveCount();
    s.bufferBytes = 0;
    impl_->buffers.forEach([&](GlBuffer* b) { s.bufferBytes += b->rec.sizeBytes; });
    s.textureCount = impl_->textures.liveCount();
    s.textureBytes = 0;
    impl_->textures.forEach([&](GlTexture* t) { s.textureBytes += textureBytes(t->rec); });
    s.samplerCount = impl_->samplers.liveCount();
    s.shaderCount = impl_->shaders.liveCount();
    s.pipelineCount = impl_->pipelines.liveCount();
    s.framebufferCount = impl_->framebuffers.liveCount();
    s.swapchainCount = impl_->swapchains.liveCount();
    return s;
}

auto OpenGLDevice::resetFrameStats() -> void { impl_->stats_ = DeviceStats{}; }

auto createOpenGLDevice() -> IDevice* {
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    const bool hasCoreClipControl = major > 4 || (major == 4 && minor >= 5);
    const bool hasExtensionClipControl = epoxy_has_gl_extension("GL_ARB_clip_control") != 0;
    if (!hasCoreClipControl && !hasExtensionClipControl) {
        F4_LOG_ERROR() << "OpenGL backend requires GL_ARB_clip_control for the shared 0..1 "
                          "clip-depth convention";
        return nullptr;
    }

    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    GLint clipDepthMode = 0;
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &clipDepthMode);
    if (clipDepthMode != GL_ZERO_TO_ONE) {
        F4_LOG_ERROR() << "OpenGL failed to select the shared 0..1 clip-depth convention";
        return nullptr;
    }

    return new OpenGLDevice();
}

} // namespace f4::gfx
