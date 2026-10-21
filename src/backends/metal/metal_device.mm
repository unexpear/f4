#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "backends/metal/metal_device.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "graphics/device/object_store.h"
#include "platform/window.h"

namespace f4::gfx {

namespace {

constexpr std::uint32_t kFramesInFlight = 3;
constexpr std::uint32_t kVertexBufferCount = 16;
constexpr std::uint32_t kResourceBufferBase = 16;
// Metal guarantees 31 buffer argument entries and 16 sampler entries. Reserving
// 0-15 for vertex input leaves 14 portable resource slots across two groups.
constexpr std::uint32_t kSlotsPerBindPoint = 7;
constexpr std::uint32_t kBindPointCount = 2;

struct MetalBufferRecord {
    GpuBuffer rec;
    __strong id<MTLBuffer> object = nil;
};

struct MetalTextureRecord {
    GpuTexture rec;
    __strong id<MTLTexture> object = nil;
};

struct MetalSamplerRecord {
    GpuSampler rec;
    __strong id<MTLSamplerState> object = nil;
};

struct MetalShaderRecord {
    GpuShader rec;
    __strong id<MTLLibrary> library = nil;
    __strong id<MTLFunction> function = nil;
};

struct MetalPipelineRecord {
    GpuPipeline rec;
    __strong id<MTLRenderPipelineState> render = nil;
    __strong id<MTLComputePipelineState> compute = nil;
    __strong id<MTLDepthStencilState> depthStencil = nil;
};

struct MetalSwapchainRecord {
    GpuSwapchain rec;
    MTLPixelFormat format = MTLPixelFormatInvalid;
};

struct MetalFramebufferRecord {
    GpuFramebuffer rec;
};

struct RetiredObject {
    __strong id object = nil;
    std::uint64_t afterSerial = 0;
};

[[nodiscard]] auto isDepthFormat(TextureFormat format) -> bool {
    return format == TextureFormat::Depth32F || format == TextureFormat::Depth24Stencil8;
}

[[nodiscard]] auto metalPixelFormat(TextureFormat format, id<MTLDevice> device)
    -> MTLPixelFormat {
    switch (format) {
    case TextureFormat::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case TextureFormat::BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case TextureFormat::R8_UNORM: return MTLPixelFormatR8Unorm;
    case TextureFormat::RG8_UNORM: return MTLPixelFormatRG8Unorm;
    case TextureFormat::R16_FLOAT: return MTLPixelFormatR16Float;
    case TextureFormat::RG16_FLOAT: return MTLPixelFormatRG16Float;
    case TextureFormat::RGBA16_FLOAT: return MTLPixelFormatRGBA16Float;
    case TextureFormat::RGBA32_FLOAT: return MTLPixelFormatRGBA32Float;
    case TextureFormat::Depth32F: return MTLPixelFormatDepth32Float;
    case TextureFormat::Depth24Stencil8:
        return device.depth24Stencil8PixelFormatSupported
                   ? MTLPixelFormatDepth24Unorm_Stencil8
                   : MTLPixelFormatDepth32Float_Stencil8;
    }
    return MTLPixelFormatInvalid;
}

[[nodiscard]] auto metalVertexFormat(VertexFormat format) -> MTLVertexFormat {
    switch (format) {
    case VertexFormat::Float32x2: return MTLVertexFormatFloat2;
    case VertexFormat::Float32x3: return MTLVertexFormatFloat3;
    case VertexFormat::Float32x4: return MTLVertexFormatFloat4;
    case VertexFormat::UNorm8x4: return MTLVertexFormatUChar4Normalized;
    }
    return MTLVertexFormatInvalid;
}

[[nodiscard]] auto metalTopology(PrimitiveTopology topology) -> MTLPrimitiveType {
    switch (topology) {
    case PrimitiveTopology::PointList: return MTLPrimitiveTypePoint;
    case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
    case PrimitiveTopology::TriangleList: return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

[[nodiscard]] auto metalTopologyClass(PrimitiveTopology topology)
    -> MTLPrimitiveTopologyClass {
    switch (topology) {
    case PrimitiveTopology::PointList: return MTLPrimitiveTopologyClassPoint;
    case PrimitiveTopology::LineList: return MTLPrimitiveTopologyClassLine;
    case PrimitiveTopology::TriangleList:
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTopologyClassTriangle;
    }
    return MTLPrimitiveTopologyClassUnspecified;
}

[[nodiscard]] auto metalCompare(CompareOp compare) -> MTLCompareFunction {
    switch (compare) {
    case CompareOp::Never: return MTLCompareFunctionNever;
    case CompareOp::Less: return MTLCompareFunctionLess;
    case CompareOp::Equal: return MTLCompareFunctionEqual;
    case CompareOp::LessEqual: return MTLCompareFunctionLessEqual;
    case CompareOp::Greater: return MTLCompareFunctionGreater;
    case CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
    case CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

[[nodiscard]] auto metalCull(CullMode mode) -> MTLCullMode {
    switch (mode) {
    case CullMode::None: return MTLCullModeNone;
    case CullMode::Front: return MTLCullModeFront;
    case CullMode::Back: return MTLCullModeBack;
    }
    return MTLCullModeNone;
}

[[nodiscard]] auto metalAddress(AddressMode mode) -> MTLSamplerAddressMode {
    switch (mode) {
    case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
    case AddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
    case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
    }
    return MTLSamplerAddressModeClampToEdge;
}

[[nodiscard]] auto metalMinFilter(TextureFilter filter) -> MTLSamplerMinMagFilter {
    return filter == TextureFilter::MinMagNearest ||
                   filter == TextureFilter::MinNearestMagLinear
               ? MTLSamplerMinMagFilterNearest
               : MTLSamplerMinMagFilterLinear;
}

[[nodiscard]] auto metalMagFilter(TextureFilter filter) -> MTLSamplerMinMagFilter {
    return filter == TextureFilter::MinMagNearest ||
                   filter == TextureFilter::MinLinearMagNearest
               ? MTLSamplerMinMagFilterNearest
               : MTLSamplerMinMagFilterLinear;
}

[[nodiscard]] auto metalMipFilter(MipFilter filter) -> MTLSamplerMipFilter {
    switch (filter) {
    case MipFilter::None: return MTLSamplerMipFilterNotMipmapped;
    case MipFilter::Nearest: return MTLSamplerMipFilterNearest;
    case MipFilter::Linear: return MTLSamplerMipFilterLinear;
    }
    return MTLSamplerMipFilterNotMipmapped;
}

[[nodiscard]] auto metalBlendFactor(BlendFactor factor) -> MTLBlendFactor {
    switch (factor) {
    case BlendFactor::Zero: return MTLBlendFactorZero;
    case BlendFactor::One: return MTLBlendFactorOne;
    case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::SrcColor: return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
    }
    return MTLBlendFactorOne;
}

[[nodiscard]] auto metalBlendOp(BlendOp operation) -> MTLBlendOperation {
    switch (operation) {
    case BlendOp::Add: return MTLBlendOperationAdd;
    case BlendOp::Sub: return MTLBlendOperationSubtract;
    case BlendOp::RevSub: return MTLBlendOperationReverseSubtract;
    case BlendOp::Min: return MTLBlendOperationMin;
    case BlendOp::Max: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

[[nodiscard]] auto metalWriteMask(std::uint32_t mask) -> MTLColorWriteMask {
    MTLColorWriteMask result = MTLColorWriteMaskNone;
    if ((mask & ColorWriteMask::R) != 0) result |= MTLColorWriteMaskRed;
    if ((mask & ColorWriteMask::G) != 0) result |= MTLColorWriteMaskGreen;
    if ((mask & ColorWriteMask::B) != 0) result |= MTLColorWriteMaskBlue;
    if ((mask & ColorWriteMask::A) != 0) result |= MTLColorWriteMaskAlpha;
    return result;
}

[[nodiscard]] auto metalIndexType(IndexFormat format) -> MTLIndexType {
    return format == IndexFormat::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

[[nodiscard]] auto shaderFunctionType(ShaderStage stage, MTLFunctionType& type) -> bool {
    switch (stage) {
    case ShaderStage::Vertex: type = MTLFunctionTypeVertex; return true;
    case ShaderStage::Fragment: type = MTLFunctionTypeFragment; return true;
    case ShaderStage::Compute: type = MTLFunctionTypeKernel; return true;
    case ShaderStage::Geometry:
    case ShaderStage::TessControl:
    case ShaderStage::TessEvaluation: return false;
    }
    return false;
}

[[nodiscard]] auto resourceBufferIndex(std::uint32_t bindPoint, std::uint32_t slot)
    -> NSUInteger {
    return static_cast<NSUInteger>(kResourceBufferBase + bindPoint * kSlotsPerBindPoint + slot);
}

[[nodiscard]] auto textureIndex(std::uint32_t bindPoint, std::uint32_t slot) -> NSUInteger {
    return static_cast<NSUInteger>(bindPoint * kSlotsPerBindPoint + slot);
}

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

[[nodiscard]] auto aligned(std::uint64_t value, std::uint64_t alignment) -> std::uint64_t {
    return (value + alignment - 1) & ~(alignment - 1);
}

void logMetalError(const char* operation, NSError* error) {
    const char* detail = error != nil ? error.localizedDescription.UTF8String : "unknown error";
    std::fprintf(stderr, "Metal %s failed: %s\n", operation, detail != nullptr ? detail : "");
}

} // namespace

struct MetalDevice::Impl {
    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> queue = nil;
    __strong CAMetalLayer* layer = nil;
    platform::Window* window = nullptr;
    dispatch_semaphore_t frameSlots = nullptr;
    std::string deviceName;
    bool initialized = false;
    std::atomic<std::uint64_t> submittedSerial{0};
    std::atomic<std::uint64_t> completedSerial{0};
    std::atomic<std::uint64_t> validationWarnings{0};
    std::atomic<std::uint64_t> validationErrors{0};
    DeviceStats stats;
    ObjectStore<MetalBufferRecord, GpuBufferTag> buffers;
    ObjectStore<MetalTextureRecord, GpuTextureTag> textures;
    ObjectStore<MetalSamplerRecord, GpuSamplerTag> samplers;
    ObjectStore<MetalShaderRecord, GpuShaderTag> shaders;
    ObjectStore<MetalPipelineRecord, GpuPipelineTag> pipelines;
    ObjectStore<MetalSwapchainRecord, GpuSwapchainTag> swapchains;
    ObjectStore<MetalFramebufferRecord, GpuFramebufferTag> framebuffers;
    SwapchainHandle activeSwapchain{};
    std::mutex retirementMutex;
    std::vector<RetiredObject> retired;

    void retire(id object) {
        if (object == nil) return;
        std::scoped_lock lock(retirementMutex);
        retired.push_back({object, submittedSerial.load(std::memory_order_acquire)});
    }
};

MetalDevice::MetalDevice(platform::Window* window) : impl_(new Impl()) {
    if (!initialize(window)) impl_->initialized = false;
}

MetalDevice::~MetalDevice() {
    if (impl_ == nullptr) return;
    waitIdle();
    {
        std::scoped_lock lock(impl_->retirementMutex);
        impl_->retired.clear();
    }
    if (impl_->layer != nil) impl_->layer.device = nil;
    delete impl_;
}

auto MetalDevice::initialize(platform::Window* window) -> bool {
    @autoreleasepool {
        impl_->device = MTLCreateSystemDefaultDevice();
        if (impl_->device == nil) return false;
        impl_->queue = [impl_->device newCommandQueue];
        if (impl_->queue == nil) return false;
        impl_->frameSlots = dispatch_semaphore_create(kFramesInFlight);
        if (impl_->frameSlots == nullptr) return false;
        impl_->window = window;
        const char* name = impl_->device.name.UTF8String;
        impl_->deviceName = name != nullptr ? name : "Metal device";
        if (window != nullptr) {
            void* opaqueLayer = window->metalLayer();
            if (opaqueLayer == nullptr) return false;
            impl_->layer = (__bridge CAMetalLayer*)opaqueLayer;
            impl_->layer.device = impl_->device;
            impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            impl_->layer.framebufferOnly = NO;
            impl_->layer.maximumDrawableCount = kFramesInFlight;
            impl_->layer.drawableSize = CGSizeMake(window->width(), window->height());
        }
        impl_->initialized = true;
        return true;
    }
}

auto MetalDevice::ready() const -> bool {
    return impl_ != nullptr && impl_->initialized && impl_->device != nil && impl_->queue != nil;
}

auto MetalDevice::backend() const -> Backend { return Backend::Metal; }

void MetalDevice::collectRetired() {
    if (impl_ == nullptr) return;
    const std::uint64_t completed = impl_->completedSerial.load(std::memory_order_acquire);
    std::scoped_lock lock(impl_->retirementMutex);
    std::erase_if(impl_->retired, [completed](const RetiredObject& retired) {
        return retired.afterSerial <= completed;
    });
}

auto MetalDevice::createBuffer(const BufferDesc& desc,
                               std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    if (!ready() || desc.sizeBytes == 0 || initialData.size() > desc.sizeBytes ||
        desc.sizeBytes > std::numeric_limits<NSUInteger>::max()) {
        return GfxError::BadDescriptor;
    }
    @autoreleasepool {
        id<MTLBuffer> object = [impl_->device
            newBufferWithLength:static_cast<NSUInteger>(desc.sizeBytes)
                       options:MTLResourceStorageModeShared];
        if (object == nil) return GfxError::OutOfMemory;
        if (!initialData.empty()) {
            std::memcpy(object.contents, initialData.data(), initialData.size());
        }
        MetalBufferRecord record;
        record.rec = {desc.sizeBytes, desc.usage};
        record.object = object;
        return impl_->buffers.create(std::move(record));
    }
}

auto MetalDevice::createTexture(const TextureDesc& desc)
    -> Result<TextureHandle, GfxError> {
    if (!ready() || desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height)) {
        return GfxError::BadDescriptor;
    }
    @autoreleasepool {
        const MTLPixelFormat format = metalPixelFormat(desc.format, impl_->device);
        if (format == MTLPixelFormatInvalid) return GfxError::BadDescriptor;
        MTLTextureDescriptor* native = [MTLTextureDescriptor new];
        native.textureType = MTLTextureType2D;
        native.pixelFormat = format;
        native.width = desc.width;
        native.height = desc.height;
        native.depth = 1;
        native.mipmapLevelCount = desc.mipLevels;
        native.arrayLength = 1;
        native.sampleCount = 1;
        native.storageMode = MTLStorageModeShared;
        native.hazardTrackingMode = MTLHazardTrackingModeTracked;
        native.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> object = [impl_->device newTextureWithDescriptor:native];
        if (object == nil) return GfxError::OutOfMemory;
        MetalTextureRecord record;
        record.rec = {desc.format, desc.width, desc.height, desc.mipLevels};
        record.object = object;
        return impl_->textures.create(std::move(record));
    }
}

auto MetalDevice::createSampler(const SamplerDesc& desc)
    -> Result<SamplerHandle, GfxError> {
    if (!ready() || !std::isfinite(desc.maxAnisotropy) || desc.maxAnisotropy < 1.0f ||
        desc.maxAnisotropy > 16.0f) {
        return GfxError::BadDescriptor;
    }
    @autoreleasepool {
        MTLSamplerDescriptor* native = [MTLSamplerDescriptor new];
        native.minFilter = metalMinFilter(desc.filter);
        native.magFilter = metalMagFilter(desc.filter);
        native.mipFilter = metalMipFilter(desc.mipFilter);
        native.sAddressMode = metalAddress(desc.addressU);
        native.tAddressMode = metalAddress(desc.addressV);
        native.rAddressMode = metalAddress(desc.addressW);
        native.maxAnisotropy = static_cast<NSUInteger>(desc.maxAnisotropy);
        native.lodMinClamp = 0.0f;
        native.lodMaxClamp = FLT_MAX;
        id<MTLSamplerState> object = [impl_->device newSamplerStateWithDescriptor:native];
        if (object == nil) return GfxError::OutOfMemory;
        MetalSamplerRecord record;
        record.rec = {desc.filter, desc.mipFilter, desc.addressU, desc.addressV,
                      desc.addressW, desc.maxAnisotropy};
        record.object = object;
        return impl_->samplers.create(std::move(record));
    }
}

auto MetalDevice::createShader(const ShaderDesc& desc)
    -> Result<ShaderHandle, GfxError> {
    MTLFunctionType expected = MTLFunctionTypeVertex;
    if (!ready() || desc.blob.isEmpty() || !shaderFunctionType(desc.stage, expected)) {
        if (ready()) ++impl_->stats.shaderFailed;
        return GfxError::BadDescriptor;
    }
    @autoreleasepool {
        dispatch_data_t bytes = dispatch_data_create(
            desc.blob.data(), desc.blob.size(),
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (bytes == nullptr) {
            ++impl_->stats.shaderFailed;
            return GfxError::OutOfMemory;
        }
        NSError* error = nil;
        id<MTLLibrary> library = [impl_->device newLibraryWithData:bytes error:&error];
        if (library == nil) {
            logMetalError("newLibraryWithData", error);
            ++impl_->stats.shaderFailed;
            return GfxError::BadDescriptor;
        }
        id<MTLFunction> selected = nil;
        for (NSString* name in library.functionNames) {
            id<MTLFunction> candidate = [library newFunctionWithName:name];
            if (candidate.functionType != expected) continue;
            if (selected != nil) {
                ++impl_->stats.shaderFailed;
                return GfxError::BadDescriptor;
            }
            selected = candidate;
        }
        if (selected == nil) {
            ++impl_->stats.shaderFailed;
            return GfxError::BadDescriptor;
        }
        MetalShaderRecord record;
        record.rec.stage = desc.stage;
        record.library = library;
        record.function = selected;
        const ShaderHandle handle = impl_->shaders.create(std::move(record));
        ++impl_->stats.shaderCompiled;
        return handle;
    }
}

auto MetalDevice::createPipeline(const PipelineStateDesc& desc)
    -> Result<PipelineHandle, GfxError> {
    const bool compute = desc.computeShader.isValid();
    const bool graphics = desc.vertexShader.isValid() && desc.fragmentShader.isValid();
    if (!ready() || compute == graphics || desc.layout.pushConstantSizeBytes != 0 ||
        desc.colorAttachments.size() > 8 ||
        (compute && (!desc.vertexInput.bindings.isEmpty() ||
                     !desc.vertexInput.attributes.isEmpty() ||
                     !desc.colorAttachments.isEmpty() || desc.depthStencil.usesAttachment()))) {
        return GfxError::BadDescriptor;
    }

    const std::uint32_t allowedStages = compute
        ? shaderStageBit(ShaderStage::Compute)
        : shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment);
    for (std::size_t groupIndex = 0; groupIndex < desc.layout.groups.size(); ++groupIndex) {
        const BindGroup& group = desc.layout.groups[groupIndex];
        if (group.bindPoint >= kBindPointCount) return GfxError::BadDescriptor;
        for (std::size_t previous = 0; previous < groupIndex; ++previous) {
            if (desc.layout.groups[previous].bindPoint == group.bindPoint) {
                return GfxError::BadDescriptor;
            }
        }
        for (std::size_t slotIndex = 0; slotIndex < group.slots.size(); ++slotIndex) {
            const BindingSlot& slot = group.slots[slotIndex];
            if (slot.count != 1 || slot.slot >= kSlotsPerBindPoint ||
                (slot.stageMask & ~allowedStages) != 0) {
                return GfxError::BadDescriptor;
            }
            for (std::size_t previous = 0; previous < slotIndex; ++previous) {
                const BindingSlot& existing = group.slots[previous];
                const bool bothBuffers =
                    (slot.type == BindingType::UniformBuffer ||
                     slot.type == BindingType::StorageBuffer) &&
                    (existing.type == BindingType::UniformBuffer ||
                     existing.type == BindingType::StorageBuffer);
                if (existing.slot == slot.slot &&
                    (existing.type == slot.type || bothBuffers)) {
                    return GfxError::BadDescriptor;
                }
            }
        }
    }

    MetalPipelineRecord record;
    record.rec.desc = desc;
    @autoreleasepool {
        NSError* error = nil;
        if (compute) {
            MetalShaderRecord* shader = impl_->shaders.get(desc.computeShader);
            if (shader == nullptr) return GfxError::InvalidHandle;
            if (shader->rec.stage != ShaderStage::Compute) return GfxError::BadDescriptor;
            record.compute = [impl_->device newComputePipelineStateWithFunction:shader->function
                                                                          error:&error];
            if (record.compute == nil) {
                logMetalError("newComputePipelineState", error);
                return GfxError::BadDescriptor;
            }
            return impl_->pipelines.create(std::move(record));
        }

        MetalShaderRecord* vertex = impl_->shaders.get(desc.vertexShader);
        MetalShaderRecord* fragment = impl_->shaders.get(desc.fragmentShader);
        if (vertex == nullptr || fragment == nullptr) return GfxError::InvalidHandle;
        if (vertex->rec.stage != ShaderStage::Vertex ||
            fragment->rec.stage != ShaderStage::Fragment) {
            return GfxError::BadDescriptor;
        }

        MTLVertexDescriptor* vertexDesc = [MTLVertexDescriptor vertexDescriptor];
        for (const auto& binding : desc.vertexInput.bindings) {
            if (binding.binding >= kVertexBufferCount || binding.strideBytes == 0) {
                return GfxError::BadDescriptor;
            }
            MTLVertexBufferLayoutDescriptor* layout = vertexDesc.layouts[binding.binding];
            layout.stride = binding.strideBytes;
            layout.stepFunction = binding.divisor == 0
                                      ? MTLVertexStepFunctionPerVertex
                                      : MTLVertexStepFunctionPerInstance;
            layout.stepRate = binding.divisor == 0 ? 1 : binding.divisor;
        }
        for (const auto& attribute : desc.vertexInput.attributes) {
            if (attribute.location >= 31 || attribute.binding >= kVertexBufferCount) {
                return GfxError::BadDescriptor;
            }
            const auto binding = std::find_if(
                desc.vertexInput.bindings.begin(), desc.vertexInput.bindings.end(),
                [&](const auto& candidate) { return candidate.binding == attribute.binding; });
            if (binding == desc.vertexInput.bindings.end() ||
                attribute.offsetBytes + vertexFormatSize(attribute.format) > binding->strideBytes) {
                return GfxError::BadDescriptor;
            }
            MTLVertexAttributeDescriptor* native = vertexDesc.attributes[attribute.location];
            native.format = metalVertexFormat(attribute.format);
            native.offset = attribute.offsetBytes;
            native.bufferIndex = attribute.binding;
        }

        MTLRenderPipelineDescriptor* native = [MTLRenderPipelineDescriptor new];
        native.vertexFunction = vertex->function;
        native.fragmentFunction = fragment->function;
        native.vertexDescriptor = vertexDesc;
        native.rasterSampleCount = 1;
        native.inputPrimitiveTopology = metalTopologyClass(desc.topology);
        const std::size_t colorCount = desc.colorAttachments.isEmpty()
                                           ? 1u
                                           : desc.colorAttachments.size();
        for (std::size_t index = 0; index < colorCount; ++index) {
            const ColorAttachmentState state = desc.colorAttachments.isEmpty()
                                                   ? ColorAttachmentState{}
                                                   : desc.colorAttachments[index];
            if (isDepthFormat(state.format)) return GfxError::BadDescriptor;
            MTLRenderPipelineColorAttachmentDescriptor* color = native.colorAttachments[index];
            color.pixelFormat = metalPixelFormat(state.format, impl_->device);
            color.blendingEnabled = state.blendEnable;
            color.sourceRGBBlendFactor = metalBlendFactor(state.srcColor);
            color.destinationRGBBlendFactor = metalBlendFactor(state.dstColor);
            color.rgbBlendOperation = metalBlendOp(state.colorOp);
            color.sourceAlphaBlendFactor = metalBlendFactor(state.srcAlpha);
            color.destinationAlphaBlendFactor = metalBlendFactor(state.dstAlpha);
            color.alphaBlendOperation = metalBlendOp(state.alphaOp);
            color.writeMask = metalWriteMask(state.writeMask);
        }
        if (desc.depthStencil.usesAttachment()) {
            native.depthAttachmentPixelFormat =
                metalPixelFormat(desc.depthStencil.format, impl_->device);
            if (desc.depthStencil.format == TextureFormat::Depth24Stencil8) {
                native.stencilAttachmentPixelFormat = native.depthAttachmentPixelFormat;
            }
            MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
            depth.depthCompareFunction = desc.depthStencil.depthTestEnable
                                             ? metalCompare(desc.depthStencil.depthCompare)
                                             : MTLCompareFunctionAlways;
            depth.depthWriteEnabled = desc.depthStencil.depthWriteEnable;
            record.depthStencil = [impl_->device newDepthStencilStateWithDescriptor:depth];
            if (record.depthStencil == nil) return GfxError::OutOfMemory;
        }
        record.render = [impl_->device newRenderPipelineStateWithDescriptor:native error:&error];
        if (record.render == nil) {
            logMetalError("newRenderPipelineState", error);
            return GfxError::BadDescriptor;
        }
        return impl_->pipelines.create(std::move(record));
    }
}

auto MetalDevice::createSwapchain(const SwapchainDesc& desc)
    -> Result<SwapchainHandle, GfxError> {
    if (!ready() || impl_->layer == nil || impl_->activeSwapchain.isValid() ||
        desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.presentMode == PresentMode::Mailbox ||
        (desc.surfaceFormat != TextureFormat::BGRA8_UNORM &&
         desc.surfaceFormat != TextureFormat::RGBA16_FLOAT)) {
        return GfxError::BadDescriptor;
    }
    const MTLPixelFormat format = metalPixelFormat(desc.surfaceFormat, impl_->device);
    impl_->layer.pixelFormat = format;
    impl_->layer.drawableSize = CGSizeMake(desc.extent.width, desc.extent.height);
    impl_->layer.displaySyncEnabled = desc.presentMode != PresentMode::Immediate;
    MetalSwapchainRecord record;
    record.rec = {desc.surfaceFormat, desc.extent, desc.presentMode};
    record.format = format;
    const SwapchainHandle handle = impl_->swapchains.create(std::move(record));
    impl_->activeSwapchain = handle;
    return handle;
}

auto MetalDevice::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (!ready() || desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.attachments.isEmpty()) {
        return GfxError::BadDescriptor;
    }
    std::size_t colorCount = 0;
    std::size_t depthCount = 0;
    bool hasDrawable = false;
    for (const AttachmentDesc& attachment : desc.attachments) {
        if (!attachment.texture.isValid()) {
            if (attachment.type != AttachmentType::Color ||
                !impl_->activeSwapchain.isValid() || hasDrawable) {
                return GfxError::BadDescriptor;
            }
            MetalSwapchainRecord* swapchain =
                impl_->swapchains.get(impl_->activeSwapchain);
            if (swapchain == nullptr || swapchain->rec.extent != desc.extent) {
                return GfxError::BadDescriptor;
            }
            hasDrawable = true;
            ++colorCount;
            continue;
        }
        MetalTextureRecord* texture = impl_->textures.get(attachment.texture);
        if (texture == nullptr) return GfxError::InvalidHandle;
        if (texture->rec.width != desc.extent.width || texture->rec.height != desc.extent.height) {
            return GfxError::BadDescriptor;
        }
        if (attachment.type == AttachmentType::Color) {
            if (isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
            ++colorCount;
        } else {
            if (!isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
            ++depthCount;
        }
    }
    if (colorCount > 8 || depthCount > 1) return GfxError::BadDescriptor;
    MetalFramebufferRecord record;
    record.rec.extent = desc.extent;
    record.rec.attachments = desc.attachments;
    return impl_->framebuffers.create(std::move(record));
}

auto MetalDevice::destroyBuffer(BufferHandle handle) -> GfxError {
    MetalBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    impl_->retire(record->object);
    (void)impl_->buffers.destroy(handle);
    collectRetired();
    return GfxError::None;
}

auto MetalDevice::destroyTexture(TextureHandle handle) -> GfxError {
    MetalTextureRecord* record = impl_->textures.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    impl_->retire(record->object);
    (void)impl_->textures.destroy(handle);
    collectRetired();
    return GfxError::None;
}

auto MetalDevice::destroySampler(SamplerHandle handle) -> GfxError {
    MetalSamplerRecord* record = impl_->samplers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    impl_->retire(record->object);
    (void)impl_->samplers.destroy(handle);
    collectRetired();
    return GfxError::None;
}

auto MetalDevice::destroyShader(ShaderHandle handle) -> GfxError {
    MetalShaderRecord* record = impl_->shaders.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    impl_->retire(record->function);
    impl_->retire(record->library);
    (void)impl_->shaders.destroy(handle);
    collectRetired();
    return GfxError::None;
}

auto MetalDevice::destroyPipeline(PipelineHandle handle) -> GfxError {
    MetalPipelineRecord* record = impl_->pipelines.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    impl_->retire(record->render);
    impl_->retire(record->compute);
    impl_->retire(record->depthStencil);
    (void)impl_->pipelines.destroy(handle);
    collectRetired();
    return GfxError::None;
}

auto MetalDevice::destroySwapchain(SwapchainHandle handle) -> GfxError {
    if (!impl_->swapchains.destroy(handle)) return GfxError::StaleHandle;
    if (impl_->activeSwapchain == handle) impl_->activeSwapchain = {};
    return GfxError::None;
}

auto MetalDevice::destroyFramebuffer(FramebufferHandle handle) -> GfxError {
    return impl_->framebuffers.destroy(handle) ? GfxError::None : GfxError::StaleHandle;
}

auto MetalDevice::writeBuffer(BufferHandle handle, std::uint64_t offset,
                              std::span<const std::byte> data) -> GfxError {
    MetalBufferRecord* buffer = impl_->buffers.get(handle);
    if (buffer == nullptr) return GfxError::StaleHandle;
    if (offset > buffer->rec.sizeBytes || data.size() > buffer->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    if (!data.empty()) {
        std::memcpy(static_cast<std::byte*>(buffer->object.contents) + offset,
                    data.data(), data.size());
    }
    return GfxError::None;
}

auto MetalDevice::readBuffer(BufferHandle handle, std::uint64_t offset,
                             std::span<std::byte> destination) -> GfxError {
    MetalBufferRecord* buffer = impl_->buffers.get(handle);
    if (buffer == nullptr) return GfxError::StaleHandle;
    if (offset > buffer->rec.sizeBytes || destination.size() > buffer->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    waitIdle();
    if (!destination.empty()) {
        std::memcpy(destination.data(),
                    static_cast<const std::byte*>(buffer->object.contents) + offset,
                    destination.size());
    }
    return GfxError::None;
}

auto MetalDevice::writeTexture(TextureHandle handle, std::uint32_t mipLevel,
                               std::span<const std::byte> data) -> GfxError {
    MetalTextureRecord* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels || isDepthFormat(texture->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(texture->rec.width >> mipLevel, 1u);
    const std::uint32_t height = std::max(texture->rec.height >> mipLevel, 1u);
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) *
                                   textureFormatBytes(texture->rec.format);
    if (data.size() != rowBytes * height) return GfxError::BadDescriptor;
    [texture->object replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:mipLevel
                         withBytes:data.data()
                       bytesPerRow:static_cast<NSUInteger>(rowBytes)];
    return GfxError::None;
}

auto MetalDevice::readTexture(TextureHandle handle, std::uint32_t mipLevel,
                              std::span<std::byte> destination) -> GfxError {
    MetalTextureRecord* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels || isDepthFormat(texture->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(texture->rec.width >> mipLevel, 1u);
    const std::uint32_t height = std::max(texture->rec.height >> mipLevel, 1u);
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) *
                                   textureFormatBytes(texture->rec.format);
    if (destination.size() != rowBytes * height) return GfxError::BadDescriptor;
    const std::uint64_t copyRowBytes = aligned(rowBytes, 256);
    if (copyRowBytes > std::numeric_limits<NSUInteger>::max() / height) {
        return GfxError::OutOfMemory;
    }
    waitIdle();
    @autoreleasepool {
        id<MTLBuffer> staging = [impl_->device
            newBufferWithLength:static_cast<NSUInteger>(copyRowBytes * height)
                       options:MTLResourceStorageModeShared];
        if (staging == nil) return GfxError::OutOfMemory;
        id<MTLCommandBuffer> commands = [impl_->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (commands == nil || blit == nil) return GfxError::DeviceLost;
        [blit copyFromTexture:texture->object
                 sourceSlice:0
                 sourceLevel:mipLevel
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                    toBuffer:staging
           destinationOffset:0
      destinationBytesPerRow:static_cast<NSUInteger>(copyRowBytes)
    destinationBytesPerImage:static_cast<NSUInteger>(copyRowBytes * height)];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError) {
            ++impl_->validationErrors;
            logMetalError("texture readback", commands.error);
            return GfxError::DeviceLost;
        }
        const auto* source = static_cast<const std::byte*>(staging.contents);
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(destination.data() + row * rowBytes,
                        source + row * copyRowBytes,
                        static_cast<std::size_t>(rowBytes));
        }
        return GfxError::None;
    }
}

auto MetalDevice::submit(CommandBuffer& commandBuffer) -> GfxError {
    if (!ready()) return GfxError::BackendUnavailable;
    if (commandBuffer.isRecording()) return GfxError::BadDescriptor;
    dispatch_semaphore_wait(impl_->frameSlots, DISPATCH_TIME_FOREVER);

    @autoreleasepool {
        id<MTLCommandBuffer> nativeCommands = [impl_->queue commandBuffer];
        if (nativeCommands == nil) {
            dispatch_semaphore_signal(impl_->frameSlots);
            return GfxError::DeviceLost;
        }
        id<MTLRenderCommandEncoder> render = nil;
        id<MTLComputeCommandEncoder> compute = nil;
        id<CAMetalDrawable> drawable = nil;
        MetalPipelineRecord* pipeline = nullptr;
        __strong id<MTLBuffer> indexBuffer = nil;
        IndexFormat indexFormat = IndexFormat::U16;
        std::vector<BoundResource> boundResources;
        std::vector<TextureFormat> passColorFormats;
        TextureFormat passDepthFormat = TextureFormat::Depth32F;
        bool passHasDepth = false;
        Extent passExtent{};
        DeviceStats delta;
        GfxError error = GfxError::None;

        const auto endCompute = [&]() {
            if (compute != nil) {
                [compute endEncoding];
                compute = nil;
            }
        };

        const auto applyBindings = [&](MetalPipelineRecord& boundPipeline) -> GfxError {
            const bool isCompute = boundPipeline.compute != nil;
            for (const BindGroup& group : boundPipeline.rec.desc.layout.groups) {
                for (const BindingSlot& slot : group.slots) {
                    const auto found = std::find_if(
                        boundResources.begin(), boundResources.end(),
                        [&](const BoundResource& resource) {
                            return resource.bindPoint == group.bindPoint &&
                                   resource.slot == slot.slot && resource.type == slot.type;
                        });
                    if (found == boundResources.end()) return GfxError::BadDescriptor;
                    const std::uint32_t stageMask = slot.stageMask == 0
                        ? (isCompute ? shaderStageBit(ShaderStage::Compute)
                                     : shaderStageBit(ShaderStage::Vertex) |
                                           shaderStageBit(ShaderStage::Fragment))
                        : slot.stageMask;
                    if (slot.type == BindingType::UniformBuffer ||
                        slot.type == BindingType::StorageBuffer) {
                        BufferHandle handle{found->handleIndex, found->handleGeneration};
                        MetalBufferRecord* buffer = impl_->buffers.get(handle);
                        if (buffer == nullptr) return GfxError::InvalidHandle;
                        const BufferUsage required = slot.type == BindingType::UniformBuffer
                            ? BufferUsage::Uniform
                            : BufferUsage::Storage;
                        if (!hasUsage(buffer->rec.usage, required)) return GfxError::BadDescriptor;
                        const NSUInteger index = resourceBufferIndex(group.bindPoint, slot.slot);
                        if (isCompute) {
                            [compute setBuffer:buffer->object offset:0 atIndex:index];
                        } else {
                            if ((stageMask & shaderStageBit(ShaderStage::Vertex)) != 0) {
                                [render setVertexBuffer:buffer->object offset:0 atIndex:index];
                            }
                            if ((stageMask & shaderStageBit(ShaderStage::Fragment)) != 0) {
                                [render setFragmentBuffer:buffer->object offset:0 atIndex:index];
                            }
                        }
                    } else if (slot.type == BindingType::Texture) {
                        TextureHandle handle{found->handleIndex, found->handleGeneration};
                        MetalTextureRecord* texture = impl_->textures.get(handle);
                        if (texture == nullptr) return GfxError::InvalidHandle;
                        const NSUInteger index = textureIndex(group.bindPoint, slot.slot);
                        if (isCompute) {
                            [compute setTexture:texture->object atIndex:index];
                        } else {
                            if ((stageMask & shaderStageBit(ShaderStage::Vertex)) != 0) {
                                [render setVertexTexture:texture->object atIndex:index];
                            }
                            if ((stageMask & shaderStageBit(ShaderStage::Fragment)) != 0) {
                                [render setFragmentTexture:texture->object atIndex:index];
                            }
                        }
                    } else {
                        SamplerHandle handle{found->handleIndex, found->handleGeneration};
                        MetalSamplerRecord* sampler = impl_->samplers.get(handle);
                        if (sampler == nullptr) return GfxError::InvalidHandle;
                        const NSUInteger index = textureIndex(group.bindPoint, slot.slot);
                        if (isCompute) {
                            [compute setSamplerState:sampler->object atIndex:index];
                        } else {
                            if ((stageMask & shaderStageBit(ShaderStage::Vertex)) != 0) {
                                [render setVertexSamplerState:sampler->object atIndex:index];
                            }
                            if ((stageMask & shaderStageBit(ShaderStage::Fragment)) != 0) {
                                [render setFragmentSamplerState:sampler->object atIndex:index];
                            }
                        }
                    }
                }
            }
            return GfxError::None;
        };

        for (const Command& command : commandBuffer.commands()) {
            if (error != GfxError::None) break;
            switch (command.kind) {
            case CommandKind::ResourceBarrier:
                if (command.barrierType == BarrierResourceType::Buffer) {
                    if (!impl_->buffers.contains(command.buffer)) error = GfxError::InvalidHandle;
                    else if (compute != nil) [compute memoryBarrierWithScope:MTLBarrierScopeBuffers];
                } else {
                    if (!impl_->textures.contains(command.texture)) error = GfxError::InvalidHandle;
                    else if (compute != nil) [compute memoryBarrierWithScope:MTLBarrierScopeTextures];
                }
                break;
            case CommandKind::BeginRenderPass: {
                if (render != nil) {
                    error = GfxError::BadDescriptor;
                    break;
                }
                endCompute();
                MetalFramebufferRecord* framebuffer = impl_->framebuffers.get(command.framebuffer);
                if (framebuffer == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                passExtent = framebuffer->rec.extent;
                passColorFormats.clear();
                passHasDepth = false;
                std::size_t colorIndex = 0;
                for (const AttachmentDesc& attachment : framebuffer->rec.attachments) {
                    id<MTLTexture> textureObject = nil;
                    TextureFormat format = TextureFormat::RGBA8_UNORM;
                    if (attachment.texture.isValid()) {
                        MetalTextureRecord* texture = impl_->textures.get(attachment.texture);
                        if (texture == nullptr) {
                            error = GfxError::InvalidHandle;
                            break;
                        }
                        textureObject = texture->object;
                        format = texture->rec.format;
                    } else {
                        if (drawable != nil || !impl_->activeSwapchain.isValid() ||
                            attachment.type != AttachmentType::Color) {
                            error = GfxError::BadDescriptor;
                            break;
                        }
                        drawable = [impl_->layer nextDrawable];
                        MetalSwapchainRecord* swapchain =
                            impl_->swapchains.get(impl_->activeSwapchain);
                        if (drawable == nil || swapchain == nullptr) {
                            error = GfxError::DeviceLost;
                            break;
                        }
                        textureObject = drawable.texture;
                        format = swapchain->rec.surfaceFormat;
                    }
                    if (attachment.type == AttachmentType::Color) {
                        MTLRenderPassColorAttachmentDescriptor* color =
                            pass.colorAttachments[colorIndex++];
                        color.texture = textureObject;
                        color.loadAction = attachment.clearOnLoad
                                               ? MTLLoadActionClear
                                               : MTLLoadActionLoad;
                        color.storeAction = MTLStoreActionStore;
                        color.clearColor = MTLClearColorMake(
                            attachment.clear.color[0], attachment.clear.color[1],
                            attachment.clear.color[2], attachment.clear.color[3]);
                        passColorFormats.push_back(format);
                    } else {
                        if (passHasDepth) {
                            error = GfxError::BadDescriptor;
                            break;
                        }
                        passHasDepth = true;
                        passDepthFormat = format;
                        pass.depthAttachment.texture = textureObject;
                        pass.depthAttachment.loadAction = attachment.clearOnLoad
                                                              ? MTLLoadActionClear
                                                              : MTLLoadActionLoad;
                        pass.depthAttachment.storeAction = MTLStoreActionStore;
                        pass.depthAttachment.clearDepth = attachment.clear.depth;
                        if (format == TextureFormat::Depth24Stencil8) {
                            pass.stencilAttachment.texture = textureObject;
                            pass.stencilAttachment.loadAction = attachment.clearOnLoad
                                                                    ? MTLLoadActionClear
                                                                    : MTLLoadActionLoad;
                            pass.stencilAttachment.storeAction = MTLStoreActionStore;
                            pass.stencilAttachment.clearStencil = attachment.clear.stencil;
                        }
                    }
                }
                if (error != GfxError::None) break;
                render = [nativeCommands renderCommandEncoderWithDescriptor:pass];
                if (render == nil) error = GfxError::DeviceLost;
                pipeline = nullptr;
                indexBuffer = nil;
                boundResources.clear();
                break;
            }
            case CommandKind::WriteTimestamp:
                // Timer queries are not wired on Metal (unverified backend).
                if (render != nil || command.timestampIndex >= kMaxGpuTimestamps) {
                    error = GfxError::BadDescriptor;
                }
                break;
            case CommandKind::EndRenderPass:
                if (render == nil) error = GfxError::BadDescriptor;
                else {
                    [render endEncoding];
                    render = nil;
                    pipeline = nullptr;
                }
                break;
            case CommandKind::BindPipeline: {
                pipeline = impl_->pipelines.get(command.pipeline);
                if (pipeline == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                if (render != nil && pipeline->render != nil) {
                    const auto& colors = pipeline->rec.desc.colorAttachments;
                    const std::size_t expectedCount = colors.isEmpty() ? 1u : colors.size();
                    if (expectedCount != passColorFormats.size()) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                    for (std::size_t index = 0; index < expectedCount; ++index) {
                        const TextureFormat expected = colors.isEmpty()
                                                           ? TextureFormat::RGBA8_UNORM
                                                           : colors[index].format;
                        if (expected != passColorFormats[index]) {
                            error = GfxError::BadDescriptor;
                            break;
                        }
                    }
                    if (error != GfxError::None) break;
                    if (pipeline->rec.desc.depthStencil.usesAttachment() != passHasDepth ||
                        (passHasDepth && pipeline->rec.desc.depthStencil.format != passDepthFormat)) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                    [render setRenderPipelineState:pipeline->render];
                    [render setDepthStencilState:pipeline->depthStencil];
                    [render setCullMode:metalCull(pipeline->rec.desc.raster.cullMode)];
                    [render setFrontFacingWinding:MTLWindingCounterClockwise];
                    [render setDepthClipMode:pipeline->rec.desc.raster.depthClampEnable
                                                 ? MTLDepthClipModeClamp
                                                 : MTLDepthClipModeClip];
                } else if (render == nil && pipeline->compute != nil) {
                    if (compute == nil) compute = [nativeCommands computeCommandEncoder];
                    if (compute == nil) error = GfxError::DeviceLost;
                    else [compute setComputePipelineState:pipeline->compute];
                } else {
                    error = GfxError::BadDescriptor;
                }
                break;
            }
            case CommandKind::BindVertexBuffer: {
                MetalBufferRecord* buffer = impl_->buffers.get(command.buffer);
                if (buffer == nullptr) error = GfxError::InvalidHandle;
                else if (render == nil || !hasUsage(buffer->rec.usage, BufferUsage::Vertex) ||
                         command.vertexSlot >= kVertexBufferCount || command.strideBytes == 0 ||
                         command.strideBytes > buffer->rec.sizeBytes) {
                    error = GfxError::BadDescriptor;
                } else {
                    [render setVertexBuffer:buffer->object offset:0 atIndex:command.vertexSlot];
                }
                break;
            }
            case CommandKind::BindIndexBuffer: {
                MetalBufferRecord* buffer = impl_->buffers.get(command.buffer);
                if (buffer == nullptr) error = GfxError::InvalidHandle;
                else if (render == nil || !hasUsage(buffer->rec.usage, BufferUsage::Index)) {
                    error = GfxError::BadDescriptor;
                } else {
                    indexBuffer = buffer->object;
                    indexFormat = command.indexFormat;
                }
                break;
            }
            case CommandKind::BindResources:
                for (std::uint8_t index = 0; index < command.resourceCount; ++index) {
                    const BoundResource& resource = command.resources[index];
                    const auto existing = std::find_if(
                        boundResources.begin(), boundResources.end(),
                        [&](const BoundResource& candidate) {
                            return candidate.bindPoint == resource.bindPoint &&
                                   candidate.slot == resource.slot &&
                                   candidate.type == resource.type;
                        });
                    if (existing == boundResources.end()) boundResources.push_back(resource);
                    else *existing = resource;
                }
                break;
            case CommandKind::SetViewport:
                if (render == nil || command.viewport.width == 0 || command.viewport.height == 0 ||
                    command.viewportX + command.viewport.width > passExtent.width ||
                    command.viewportY + command.viewport.height > passExtent.height) {
                    error = GfxError::BadDescriptor;
                } else {
                    [render setViewport:MTLViewport{static_cast<double>(command.viewportX),
                                                   static_cast<double>(command.viewportY),
                                                   static_cast<double>(command.viewport.width),
                                                   static_cast<double>(command.viewport.height),
                                                   0.0, 1.0}];
                }
                break;
            case CommandKind::SetScissor:
                if (render == nil || command.scissor.width == 0 || command.scissor.height == 0 ||
                    command.scissorX + command.scissor.width > passExtent.width ||
                    command.scissorY + command.scissor.height > passExtent.height) {
                    error = GfxError::BadDescriptor;
                } else {
                    [render setScissorRect:MTLScissorRect{command.scissorX, command.scissorY,
                                                         command.scissor.width,
                                                         command.scissor.height}];
                }
                break;
            case CommandKind::Draw:
                if (render == nil || pipeline == nullptr || pipeline->render == nil ||
                    command.vertexCount == 0 || command.instanceCount == 0) {
                    error = GfxError::BadDescriptor;
                } else if ((error = applyBindings(*pipeline)) != GfxError::None) {
                    break;
                } else {
                    [render drawPrimitives:metalTopology(pipeline->rec.desc.topology)
                               vertexStart:0
                               vertexCount:command.vertexCount
                             instanceCount:command.instanceCount];
                    ++delta.drawCalls;
                    ++delta.batches;
                    delta.instances += command.instanceCount;
                    delta.vertices += static_cast<std::uint64_t>(command.vertexCount) *
                                      command.instanceCount;
                    delta.primitives += countPrimitives(pipeline->rec.desc.topology,
                                                        command.vertexCount) *
                                        command.instanceCount;
                }
                break;
            case CommandKind::DrawIndexed: {
                const std::uint64_t offset = static_cast<std::uint64_t>(command.firstIndex) *
                                             indexFormatSize(indexFormat);
                if (render == nil || pipeline == nullptr || pipeline->render == nil ||
                    indexBuffer == nil || command.indexCount == 0 || command.instanceCount == 0 ||
                    offset > indexBuffer.length ||
                    static_cast<std::uint64_t>(command.indexCount) *
                            indexFormatSize(indexFormat) >
                        indexBuffer.length - offset) {
                    error = GfxError::BadDescriptor;
                } else if ((error = applyBindings(*pipeline)) != GfxError::None) {
                    break;
                } else {
                    [render drawIndexedPrimitives:metalTopology(pipeline->rec.desc.topology)
                                       indexCount:command.indexCount
                                        indexType:metalIndexType(indexFormat)
                                      indexBuffer:indexBuffer
                                indexBufferOffset:static_cast<NSUInteger>(offset)
                                    instanceCount:command.instanceCount];
                    ++delta.drawIndexedCalls;
                    ++delta.batches;
                    delta.instances += command.instanceCount;
                    delta.vertices += static_cast<std::uint64_t>(command.indexCount) *
                                      command.instanceCount;
                    delta.primitives += countPrimitives(pipeline->rec.desc.topology,
                                                        command.indexCount) *
                                        command.instanceCount;
                }
                break;
            }
            case CommandKind::Dispatch:
                if (render != nil || pipeline == nullptr || pipeline->compute == nil ||
                    compute == nil || command.groupCountX == 0 || command.groupCountY == 0 ||
                    command.groupCountZ == 0) {
                    error = GfxError::BadDescriptor;
                } else if ((error = applyBindings(*pipeline)) != GfxError::None) {
                    break;
                } else {
                    [compute dispatchThreadgroups:MTLSizeMake(command.groupCountX,
                                                              command.groupCountY,
                                                              command.groupCountZ)
                          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                    ++delta.dispatchCalls;
                }
                break;
            }
        }

        if (render != nil) {
            [render endEncoding];
            render = nil;
            if (error == GfxError::None) error = GfxError::BadDescriptor;
        }
        endCompute();
        if (error != GfxError::None) {
            dispatch_semaphore_signal(impl_->frameSlots);
            return error;
        }
        if (drawable != nil) [nativeCommands presentDrawable:drawable];

        const std::uint64_t serial =
            impl_->submittedSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
        Impl* state = impl_;
        [nativeCommands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if (completed.status == MTLCommandBufferStatusError) {
                state->validationErrors.fetch_add(1, std::memory_order_relaxed);
                logMetalError("command buffer", completed.error);
            }
            std::uint64_t observed = state->completedSerial.load(std::memory_order_acquire);
            while (observed < serial &&
                   !state->completedSerial.compare_exchange_weak(
                       observed, serial, std::memory_order_release,
                       std::memory_order_acquire)) {
            }
            dispatch_semaphore_signal(state->frameSlots);
        }];
        [nativeCommands commit];
        impl_->stats.drawCalls += delta.drawCalls;
        impl_->stats.drawIndexedCalls += delta.drawIndexedCalls;
        impl_->stats.dispatchCalls += delta.dispatchCalls;
        impl_->stats.batches += delta.batches;
        impl_->stats.instances += delta.instances;
        impl_->stats.primitives += delta.primitives;
        impl_->stats.vertices += delta.vertices;
        collectRetired();
        return GfxError::None;
    }
}

auto MetalDevice::waitIdle() -> void {
    if (!ready()) return;
    @autoreleasepool {
        id<MTLCommandBuffer> commands = [impl_->queue commandBuffer];
        if (commands == nil) return;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError) {
            ++impl_->validationErrors;
            logMetalError("waitIdle", commands.error);
        }
        // A later command completing proves queue execution is idle, but its
        // wait does not establish that earlier completion blocks have returned.
        // Drain every frame token before collecting resources or deleting Impl.
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            dispatch_semaphore_wait(impl_->frameSlots, DISPATCH_TIME_FOREVER);
        }
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            dispatch_semaphore_signal(impl_->frameSlots);
        }
        impl_->completedSerial.store(
            impl_->submittedSerial.load(std::memory_order_acquire),
            std::memory_order_release);
        collectRetired();
    }
}

auto MetalDevice::stats() const -> DeviceStats {
    DeviceStats result = impl_->stats;
    result.bufferCount = impl_->buffers.liveCount();
    result.bufferBytes = 0;
    impl_->buffers.forEach([&](const MetalBufferRecord* buffer) {
        result.bufferBytes += buffer->rec.sizeBytes;
    });
    result.textureCount = impl_->textures.liveCount();
    result.textureBytes = 0;
    impl_->textures.forEach([&](const MetalTextureRecord* texture) {
        result.textureBytes += textureBytes(texture->rec);
    });
    result.samplerCount = impl_->samplers.liveCount();
    result.shaderCount = impl_->shaders.liveCount();
    result.pipelineCount = impl_->pipelines.liveCount();
    result.swapchainCount = impl_->swapchains.liveCount();
    result.framebufferCount = impl_->framebuffers.liveCount();
    return result;
}

auto MetalDevice::resetFrameStats() -> void {
    impl_->stats.drawCalls = 0;
    impl_->stats.drawIndexedCalls = 0;
    impl_->stats.dispatchCalls = 0;
    impl_->stats.batches = 0;
    impl_->stats.instances = 0;
    impl_->stats.primitives = 0;
    impl_->stats.vertices = 0;
}

auto MetalDevice::deviceName() const -> const char* {
    return impl_ != nullptr ? impl_->deviceName.c_str() : "";
}

auto MetalDevice::hasWindow() const -> bool {
    return impl_ != nullptr && impl_->window != nullptr && impl_->layer != nil;
}

auto MetalDevice::preferredSurfaceFormat() const -> TextureFormat {
    return TextureFormat::BGRA8_UNORM;
}

auto MetalDevice::framesInFlight() const -> std::uint32_t { return kFramesInFlight; }

auto MetalDevice::validationWarningCount() const -> std::uint64_t {
    return impl_ != nullptr
               ? impl_->validationWarnings.load(std::memory_order_acquire)
               : 0;
}

auto MetalDevice::validationErrorCount() const -> std::uint64_t {
    return impl_ != nullptr
               ? impl_->validationErrors.load(std::memory_order_acquire)
               : 0;
}

auto createMetalDevice(platform::Window* window) -> IDevice* {
    auto* device = new MetalDevice(window);
    if (!device->ready()) {
        delete device;
        return nullptr;
    }
    return device;
}

} // namespace f4::gfx
