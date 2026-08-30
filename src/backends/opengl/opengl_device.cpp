#include "backends/opengl/opengl_device.h"

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
    default: return GL_UNSIGNED_BYTE;
    }
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
    bool glCullFace_ = false;
    bool glBlend_ = false;
    GLenum glDepthFunc_ = GL_LESS;
    GLboolean glDepthMask_ = GL_TRUE;
    GLenum glBlendSrc_ = GL_ONE;
    GLenum glBlendDst_ = GL_ZERO;
    GLuint glProgram_ = 0;
    GLuint glVao_ = 0;
};

OpenGLDevice::OpenGLDevice() : impl_(new Impl()) {
    F4_ASSERT(impl_ != nullptr, "OpenGLDevice impl allocation failed");
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
    if (desc.sizeBytes == 0) {
        return GfxError::BadDescriptor;
    }
    GlBuffer b;
    b.rec.sizeBytes = desc.sizeBytes;
    b.rec.usage = desc.usage;
    b.target = bufferTarget(desc.usage);
    glGenBuffers(1, &b.id);
    glBindBuffer(b.target, b.id);
    glBufferData(b.target, static_cast<GLsizeiptr>(desc.sizeBytes), initialData.data(),
                 GL_STATIC_DRAW);
    return impl_->buffers.create(b);
}

auto OpenGLDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (desc.width == 0 || desc.height == 0) {
        return GfxError::BadDescriptor;
    }
    GlTexture t;
    t.rec.format = desc.format;
    t.rec.width = desc.width;
    t.rec.height = desc.height;
    t.rec.mipLevels = desc.mipLevels;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexImage2D(GL_TEXTURE_2D, 0, glTextureInternal(desc.format),
                 static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    return impl_->textures.create(t);
}

auto OpenGLDevice::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    GlSampler s;
    s.rec.filter = desc.filter;
    s.rec.addressU = desc.addressU;
    s.rec.addressV = desc.addressV;
    s.rec.addressW = desc.addressW;
    s.rec.maxAnisotropy = desc.maxAnisotropy;
    glGenSamplers(1, &s.id);
    const GLenum filter = desc.filter == TextureFilter::MinMagLinear ||
                                  desc.filter == TextureFilter::MinLinearMagNearest
                              ? GL_LINEAR
                              : GL_NEAREST;
    glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    if (desc.maxAnisotropy > 1.0f) {
        glSamplerParameterf(s.id, GL_TEXTURE_MAX_ANISOTROPY, desc.maxAnisotropy);
    }
    return impl_->samplers.create(s);
}

auto OpenGLDevice::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    if (desc.stage != ShaderStage::Vertex && desc.stage != ShaderStage::Fragment) {
        // Compute/geometry/tess not yet wired in M7.
        return GfxError::BadDescriptor;
    }
    const GLenum type = desc.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    const char* stageName = desc.stage == ShaderStage::Vertex ? "vertex" : "fragment";
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
    GlShader* vs = impl_->shaders.get(desc.vertexShader);
    GlShader* fs = impl_->shaders.get(desc.fragmentShader);
    if (vs == nullptr || fs == nullptr) {
        return GfxError::InvalidHandle;
    }
    if (vs->rec.stage != ShaderStage::Vertex || fs->rec.stage != ShaderStage::Fragment) {
        return GfxError::BadDescriptor;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vs->id);
    glAttachShader(program, fs->id);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return GfxError::BadDescriptor;
    }

    // Build the VAO from the vertex input layout using the 4.3 vertex-attrib
    // binding API (bindings carry stride; attributes carry binding + offset).
    GLuint vao = 0;
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
                             glVertexFormatType(attribute.format),
                             normalized,
                             static_cast<GLuint>(attribute.offsetBytes));
        glVertexAttribBinding(attribute.location, attribute.binding);
    }
    glBindVertexArray(0);

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

        GLenum drawBuffers[8]{};
        std::uint32_t colorIndex = 0;

        for (const auto& att : desc.attachments) {
            if (att.type == AttachmentType::Color && att.texture.isValid()) {
                GlTexture* tex = impl_->textures.get(att.texture);
                if (tex == nullptr) {
                    glDeleteFramebuffers(1, &fb.fbo);
                    return GfxError::InvalidHandle;
                }
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + colorIndex,
                                       GL_TEXTURE_2D, tex->id, 0);
                drawBuffers[colorIndex] = GL_COLOR_ATTACHMENT0 + colorIndex;
                ++colorIndex;
            } else if (att.type == AttachmentType::Depth && att.texture.isValid()) {
                GlTexture* tex = impl_->textures.get(att.texture);
                if (tex == nullptr) {
                    glDeleteFramebuffers(1, &fb.fbo);
                    return GfxError::InvalidHandle;
                }
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_TEXTURE_2D, tex->id, 0);
            }
        }

        if (colorIndex > 0) {
            glDrawBuffers(static_cast<GLsizei>(colorIndex), drawBuffers);
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
    if (p->program != 0) {
        glDeleteProgram(p->program);
    }
    if (p->vao != 0) {
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
    if (hasUsage(b->rec.usage, BufferUsage::Uniform)) {
        glBufferData(b->target, static_cast<GLsizeiptr>(b->rec.sizeBytes),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(b->target, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(data.size()), data.data());
    return GfxError::None;
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
    // Reset per-submit state machine and GL state cache.
    impl_->lastSubmitError_ = GfxError::None;
    impl_->renderPassActive_ = false;
    impl_->boundPipeline_ = nullptr;
    impl_->boundIndexBuffer_ = 0;
    impl_->boundFramebuffer_ = nullptr;
    impl_->lastPipeline_ = nullptr;
    impl_->glDepthTest_ = false;
    impl_->glCullFace_ = false;
    impl_->glBlend_ = false;
    impl_->glDepthFunc_ = GL_LESS;
    impl_->glDepthMask_ = GL_TRUE;
    impl_->glProgram_ = 0;
    impl_->glVao_ = 0;

    for (const Command& command : cb.commands()) {
        switch (command.kind) {
        case CommandKind::BeginRenderPass: {
            GlFramebuffer* fb = impl_->framebuffers.get(command.framebuffer);
            if (fb == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
            glViewport(0, 0, static_cast<GLsizei>(fb->rec.extent.width),
                       static_cast<GLsizei>(fb->rec.extent.height));

            // Build the clear mask from actual attachment flags — only clear
            // attachments whose load operation is clearOnLoad.
            GLbitfield clearBits = 0;
            for (const auto& att : fb->rec.attachments) {
                if (!att.clearOnLoad) {
                    continue;
                }
                if (att.type == AttachmentType::Color) {
                    glClearColor(att.clear.color[0], att.clear.color[1],
                                 att.clear.color[2], att.clear.color[3]);
                    clearBits |= GL_COLOR_BUFFER_BIT;
                } else if (att.type == AttachmentType::Depth) {
                    glClearDepth(static_cast<GLclampd>(att.clear.depth));
                    clearBits |= GL_DEPTH_BUFFER_BIT;
                }
            }
            if (clearBits != 0) {
                glClear(clearBits);
            }

            impl_->renderPassActive_ = true;
            impl_->boundFramebuffer_ = fb;
            break;
        }
        case CommandKind::EndRenderPass: {
            impl_->renderPassActive_ = false;
            impl_->boundFramebuffer_ = nullptr;
            // Update state cache: EndRenderPass disables depth test.
            if (impl_->glDepthTest_) {
                glDisable(GL_DEPTH_TEST);
                impl_->glDepthTest_ = false;
            }
            break;
        }
        case CommandKind::BindPipeline: {
            GlPipeline* p = impl_->pipelines.get(command.pipeline);
            if (p == nullptr) {
                impl_->lastSubmitError_ = GfxError::InvalidHandle;
                return GfxError::InvalidHandle;
            }
            // State cache: skip redundant state changes for same-pipeline binds.
            if (p != impl_->lastPipeline_) {
                if (p->program != impl_->glProgram_) {
                    glUseProgram(p->program);
                    impl_->glProgram_ = p->program;
                }
                if (p->vao != impl_->glVao_) {
                    glBindVertexArray(p->vao);
                    impl_->glVao_ = p->vao;
                }
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
                const GLboolean wantMask = p->rec.desc.depthStencil.depthWriteEnable
                                               ? GL_TRUE
                                               : GL_FALSE;
                if (wantMask != impl_->glDepthMask_) {
                    glDepthMask(wantMask);
                    impl_->glDepthMask_ = wantMask;
                }
                // Blending (first color attachment only — enough for a fixed
                // overlay; M8 material work can generalize this).
                const ColorAttachmentState* blend = nullptr;
                if (!p->rec.desc.colorAttachments.isEmpty()) {
                    blend = &p->rec.desc.colorAttachments[0];
                }
                const bool wantBlend = blend != nullptr && blend->blendEnable;
                if (wantBlend != impl_->glBlend_) {
                    if (wantBlend) {
                        glEnable(GL_BLEND);
                    } else {
                        glDisable(GL_BLEND);
                    }
                    impl_->glBlend_ = wantBlend;
                }
                if (wantBlend && blend != nullptr) {
                    const GLenum src = glBlendFactor(blend->srcColor);
                    const GLenum dst = glBlendFactor(blend->dstColor);
                    if (src != impl_->glBlendSrc_ || dst != impl_->glBlendDst_) {
                        glBlendFuncSeparate(src, dst, src, dst);
                        impl_->glBlendSrc_ = src;
                        impl_->glBlendDst_ = dst;
                    }
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
            glScissor(0, 0, static_cast<GLsizei>(command.scissor.width),
                      static_cast<GLsizei>(command.scissor.height));
            break;
        }
        case CommandKind::Draw: {
            if (impl_->boundPipeline_ == nullptr) {
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
            if (impl_->boundPipeline_ == nullptr || impl_->boundIndexBuffer_ == 0) {
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
        case CommandKind::BindResources: {
            for (std::uint8_t ri = 0; ri < command.resourceCount; ++ri) {
                const BoundResource& res = command.resources[ri];
                if (res.type == BindingType::Texture) {
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
                    std::string uniformName = res.slot == 0 ? "uTex" : "uTex" + std::to_string(res.slot);
                    const GLint loc = glGetUniformLocation(impl_->glProgram_, uniformName.c_str());
                    if (loc >= 0) {
                        glUniform1i(loc, static_cast<GLint>(res.slot));
                    }
                } else if (res.type == BindingType::Sampler) {
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
                    if (hasUsage(b->rec.usage, BufferUsage::Uniform)) {
                        glBindBufferBase(GL_UNIFORM_BUFFER, res.slot, b->id);
                    } else if (hasUsage(b->rec.usage, BufferUsage::Storage)) {
                        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, res.slot, b->id);
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

auto createOpenGLDevice() -> IDevice* { return new OpenGLDevice(); }

} // namespace f4::gfx
