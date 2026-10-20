#include "backends/webgpu/webgpu_device.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <webgpu/webgpu.h>

#include "core/logging/logger.h"
#include "graphics/device/object_store.h"

namespace f4::gfx {

namespace {

[[nodiscard]] auto viewOf(std::string_view text) -> WGPUStringView {
    return WGPUStringView{text.data(), text.size()};
}

[[nodiscard]] auto textOf(WGPUStringView text) -> std::string {
    if (text.data == nullptr) return {};
    const std::size_t length = text.length == WGPU_STRLEN ? std::strlen(text.data) : text.length;
    return std::string(text.data, length);
}

void logWebGpuError(std::string_view operation, WGPUStringView message) {
    F4_LOG_ERROR() << "WebGPU " << operation << ": " << textOf(message);
}

[[nodiscard]] constexpr auto alignToFour(std::uint64_t value) -> std::uint64_t {
    return (value + 3u) & ~std::uint64_t{3u};
}

[[nodiscard]] auto webGpuFormat(TextureFormat format) -> WGPUTextureFormat {
    switch (format) {
    case TextureFormat::RGBA8_UNORM: return WGPUTextureFormat_RGBA8Unorm;
    case TextureFormat::BGRA8_UNORM: return WGPUTextureFormat_BGRA8Unorm;
    case TextureFormat::R8_UNORM: return WGPUTextureFormat_R8Unorm;
    case TextureFormat::RG8_UNORM: return WGPUTextureFormat_RG8Unorm;
    case TextureFormat::R16_FLOAT: return WGPUTextureFormat_R16Float;
    case TextureFormat::RG16_FLOAT: return WGPUTextureFormat_RG16Float;
    case TextureFormat::RGBA16_FLOAT: return WGPUTextureFormat_RGBA16Float;
    case TextureFormat::RGBA32_FLOAT: return WGPUTextureFormat_RGBA32Float;
    case TextureFormat::Depth32F: return WGPUTextureFormat_Depth32Float;
    case TextureFormat::Depth24Stencil8: return WGPUTextureFormat_Depth24PlusStencil8;
    }
    return WGPUTextureFormat_Undefined;
}

[[nodiscard]] auto bufferUsage(BufferUsage usage) -> WGPUBufferUsage {
    WGPUBufferUsage result = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    if (hasUsage(usage, BufferUsage::Vertex)) result |= WGPUBufferUsage_Vertex;
    if (hasUsage(usage, BufferUsage::Index)) result |= WGPUBufferUsage_Index;
    if (hasUsage(usage, BufferUsage::Uniform)) result |= WGPUBufferUsage_Uniform;
    if (hasUsage(usage, BufferUsage::Storage)) result |= WGPUBufferUsage_Storage;
    return result;
}

[[nodiscard]] auto webGpuAddress(AddressMode mode) -> WGPUAddressMode {
    switch (mode) {
    case AddressMode::Repeat: return WGPUAddressMode_Repeat;
    case AddressMode::MirroredRepeat: return WGPUAddressMode_MirrorRepeat;
    case AddressMode::ClampToEdge: return WGPUAddressMode_ClampToEdge;
    }
    return WGPUAddressMode_Undefined;
}

[[nodiscard]] auto webGpuCompare(CompareOp op) -> WGPUCompareFunction {
    switch (op) {
    case CompareOp::Never: return WGPUCompareFunction_Never;
    case CompareOp::Less: return WGPUCompareFunction_Less;
    case CompareOp::Equal: return WGPUCompareFunction_Equal;
    case CompareOp::LessEqual: return WGPUCompareFunction_LessEqual;
    case CompareOp::Greater: return WGPUCompareFunction_Greater;
    case CompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
    case CompareOp::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
    case CompareOp::Always: return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Undefined;
}

[[nodiscard]] auto webGpuCull(CullMode mode) -> WGPUCullMode {
    switch (mode) {
    case CullMode::None: return WGPUCullMode_None;
    case CullMode::Front: return WGPUCullMode_Front;
    case CullMode::Back: return WGPUCullMode_Back;
    }
    return WGPUCullMode_None;
}

[[nodiscard]] auto webGpuTopology(PrimitiveTopology topology) -> WGPUPrimitiveTopology {
    switch (topology) {
    case PrimitiveTopology::PointList: return WGPUPrimitiveTopology_PointList;
    case PrimitiveTopology::LineList: return WGPUPrimitiveTopology_LineList;
    case PrimitiveTopology::TriangleList: return WGPUPrimitiveTopology_TriangleList;
    case PrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
    }
    return WGPUPrimitiveTopology_Undefined;
}

[[nodiscard]] auto webGpuIndexFormat(IndexFormat format) -> WGPUIndexFormat {
    return format == IndexFormat::U16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
}

[[nodiscard]] auto webGpuPresentMode(PresentMode mode) -> WGPUPresentMode {
    switch (mode) {
    case PresentMode::Immediate: return WGPUPresentMode_Immediate;
    case PresentMode::Mailbox: return WGPUPresentMode_Mailbox;
    case PresentMode::Fifo: return WGPUPresentMode_Fifo;
    }
    return WGPUPresentMode_Undefined;
}

[[nodiscard]] auto webGpuVertexFormat(VertexFormat format) -> WGPUVertexFormat {
    switch (format) {
    case VertexFormat::Float32x2: return WGPUVertexFormat_Float32x2;
    case VertexFormat::Float32x3: return WGPUVertexFormat_Float32x3;
    case VertexFormat::Float32x4: return WGPUVertexFormat_Float32x4;
    case VertexFormat::UNorm8x4: return WGPUVertexFormat_Unorm8x4;
    }
    return WGPUVertexFormat_Float32x2;
}

[[nodiscard]] auto webGpuBlendFactor(BlendFactor factor) -> WGPUBlendFactor {
    switch (factor) {
    case BlendFactor::Zero: return WGPUBlendFactor_Zero;
    case BlendFactor::One: return WGPUBlendFactor_One;
    case BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case BlendFactor::SrcColor: return WGPUBlendFactor_Src;
    case BlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
    }
    return WGPUBlendFactor_Undefined;
}

[[nodiscard]] auto webGpuBlendOp(BlendOp op) -> WGPUBlendOperation {
    switch (op) {
    case BlendOp::Add: return WGPUBlendOperation_Add;
    case BlendOp::Sub: return WGPUBlendOperation_Subtract;
    case BlendOp::RevSub: return WGPUBlendOperation_ReverseSubtract;
    case BlendOp::Min: return WGPUBlendOperation_Min;
    case BlendOp::Max: return WGPUBlendOperation_Max;
    }
    return WGPUBlendOperation_Undefined;
}

[[nodiscard]] auto webGpuWriteMask(std::uint32_t mask) -> WGPUColorWriteMask {
    WGPUColorWriteMask result = WGPUColorWriteMask_None;
    if ((mask & ColorWriteMask::R) != 0) result |= WGPUColorWriteMask_Red;
    if ((mask & ColorWriteMask::G) != 0) result |= WGPUColorWriteMask_Green;
    if ((mask & ColorWriteMask::B) != 0) result |= WGPUColorWriteMask_Blue;
    if ((mask & ColorWriteMask::A) != 0) result |= WGPUColorWriteMask_Alpha;
    return result;
}

[[nodiscard]] auto webGpuShaderStages(std::uint32_t mask,
                                      WGPUShaderStage defaultStages) -> WGPUShaderStage {
    if (mask == 0) return defaultStages;
    WGPUShaderStage result = WGPUShaderStage_None;
    if ((mask & shaderStageBit(ShaderStage::Vertex)) != 0) result |= WGPUShaderStage_Vertex;
    if ((mask & shaderStageBit(ShaderStage::Fragment)) != 0) result |= WGPUShaderStage_Fragment;
    if ((mask & shaderStageBit(ShaderStage::Compute)) != 0) result |= WGPUShaderStage_Compute;
    return result;
}

[[nodiscard]] auto countPrimitives(PrimitiveTopology topology, std::uint64_t count)
    -> std::uint64_t {
    switch (topology) {
    case PrimitiveTopology::PointList: return count;
    case PrimitiveTopology::LineList: return count / 2;
    case PrimitiveTopology::TriangleList: return count / 3;
    case PrimitiveTopology::TriangleStrip: return count >= 3 ? count - 2 : 0;
    }
    return 0;
}

struct AdapterRequest {
    WGPUAdapter adapter = nullptr;
    bool success = false;
};

void onAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
               void* userdata1, void*) {
    auto* request = static_cast<AdapterRequest*>(userdata1);
    request->success = status == WGPURequestAdapterStatus_Success && adapter != nullptr;
    request->adapter = adapter;
    if (!request->success) logWebGpuError("adapter request failed", message);
}

struct DeviceRequest {
    WGPUDevice device = nullptr;
    bool success = false;
};

void onDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
              void* userdata1, void*) {
    auto* request = static_cast<DeviceRequest*>(userdata1);
    request->success = status == WGPURequestDeviceStatus_Success && device != nullptr;
    request->device = device;
    if (!request->success) logWebGpuError("device request failed", message);
}

struct MapRequest {
    bool success = false;
};

void onMap(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*) {
    auto* request = static_cast<MapRequest*>(userdata1);
    request->success = status == WGPUMapAsyncStatus_Success;
    if (!request->success) logWebGpuError("buffer map failed", message);
}

struct QueueRequest {
    bool success = false;
};

void onQueueDone(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1, void*) {
    auto* request = static_cast<QueueRequest*>(userdata1);
    request->success = status == WGPUQueueWorkDoneStatus_Success;
    if (!request->success) logWebGpuError("queue wait failed", message);
}

struct CompilationRequest {
    bool success = false;
    bool hasErrors = false;
};

void onCompilationInfo(WGPUCompilationInfoRequestStatus status,
                       const WGPUCompilationInfo* info, void* userdata1, void*) {
    auto* request = static_cast<CompilationRequest*>(userdata1);
    request->success = status == WGPUCompilationInfoRequestStatus_Success && info != nullptr;
    if (!request->success) {
        F4_LOG_ERROR() << "WebGPU shader compilation information request failed";
        return;
    }
    for (std::size_t index = 0; index < info->messageCount; ++index) {
        const WGPUCompilationMessage& message = info->messages[index];
        if (message.type == WGPUCompilationMessageType_Error) {
            request->hasErrors = true;
            logWebGpuError("shader compilation", message.message);
        } else if (message.type == WGPUCompilationMessageType_Warning) {
            F4_LOG_WARNING() << "WebGPU shader compilation: " << textOf(message.message);
        }
    }
}

struct ComputePipelineRequest {
    WGPUComputePipeline pipeline = nullptr;
    bool success = false;
};

void onComputePipeline(WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline,
                       WGPUStringView message, void* userdata1, void*) {
    auto* request = static_cast<ComputePipelineRequest*>(userdata1);
    request->success = status == WGPUCreatePipelineAsyncStatus_Success && pipeline != nullptr;
    request->pipeline = pipeline;
    if (!request->success) logWebGpuError("compute pipeline creation failed", message);
}

struct RenderPipelineRequest {
    WGPURenderPipeline pipeline = nullptr;
    bool success = false;
};

void onRenderPipeline(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline,
                      WGPUStringView message, void* userdata1, void*) {
    auto* request = static_cast<RenderPipelineRequest*>(userdata1);
    request->success = status == WGPUCreatePipelineAsyncStatus_Success && pipeline != nullptr;
    request->pipeline = pipeline;
    if (!request->success) logWebGpuError("render pipeline creation failed", message);
}

[[nodiscard]] auto waitFor(WGPUInstance instance, WGPUFuture future) -> bool {
    WGPUFutureWaitInfo wait = WGPU_FUTURE_WAIT_INFO_INIT;
    wait.future = future;
    return wgpuInstanceWaitAny(instance, 1, &wait, std::numeric_limits<std::uint64_t>::max()) ==
               WGPUWaitStatus_Success &&
           wait.completed == WGPU_TRUE;
}

struct WebGpuBuffer {
    GpuBuffer rec;
    WGPUBuffer object = nullptr;
    std::uint64_t allocationBytes = 0;
};

struct WebGpuTexture {
    GpuTexture rec;
    WGPUTexture object = nullptr;
    WGPUTextureView view = nullptr;
};

struct WebGpuSampler {
    GpuSampler rec;
    WGPUSampler object = nullptr;
};

struct WebGpuShader {
    GpuShader rec;
    WGPUShaderModule object = nullptr;
};

struct WebGpuPipeline {
    GpuPipeline rec;
    WGPUPipelineLayout layout = nullptr;
    std::vector<WGPUBindGroupLayout> bindGroupLayouts;
    WGPURenderPipeline render = nullptr;
    WGPUComputePipeline compute = nullptr;
};

struct WebGpuSwapchain {
    GpuSwapchain rec;
};

struct WebGpuFramebuffer {
    GpuFramebuffer rec;
};

[[nodiscard]] auto isDepthFormat(TextureFormat format) -> bool {
    return format == TextureFormat::Depth32F || format == TextureFormat::Depth24Stencil8;
}

} // namespace

struct WebGpuDevice::Impl {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    TextureFormat preferredFormat = TextureFormat::BGRA8_UNORM;
    SwapchainHandle activeSwapchain{};
    bool deviceLost = false;
    bool limitsValid = false;
    bool depthClipControl = false;
    std::uint64_t validationErrors = 0;
    WGPULimits limits = WGPU_LIMITS_INIT;
    std::vector<TextureFormat> surfaceFormats;
    std::vector<WGPUPresentMode> presentModes;
    DeviceStats stats;

    ObjectStore<WebGpuBuffer, GpuBufferTag> buffers;
    ObjectStore<WebGpuTexture, GpuTextureTag> textures;
    ObjectStore<WebGpuSampler, GpuSamplerTag> samplers;
    ObjectStore<WebGpuShader, GpuShaderTag> shaders;
    ObjectStore<WebGpuPipeline, GpuPipelineTag> pipelines;
    ObjectStore<WebGpuSwapchain, GpuSwapchainTag> swapchains;
    ObjectStore<WebGpuFramebuffer, GpuFramebufferTag> framebuffers;

    void configureSurface(const WebGpuSwapchain& swapchain) {
        WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
        config.device = device;
        config.format = webGpuFormat(swapchain.rec.surfaceFormat);
        config.usage = WGPUTextureUsage_RenderAttachment;
        config.width = swapchain.rec.extent.width;
        config.height = swapchain.rec.extent.height;
        config.presentMode = webGpuPresentMode(swapchain.rec.presentMode);
        wgpuSurfaceConfigure(surface, &config);
    }
};

WebGpuDevice::WebGpuDevice(const char* canvasSelector) : impl_(new Impl()) {
    const WGPUInstanceFeatureName feature = WGPUInstanceFeatureName_TimedWaitAny;
    WGPUInstanceDescriptor instanceDesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    instanceDesc.requiredFeatureCount = 1;
    instanceDesc.requiredFeatures = &feature;
    impl_->instance = wgpuCreateInstance(&instanceDesc);
    if (impl_->instance == nullptr) return;

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = viewOf(canvasSelector);
    WGPUSurfaceDescriptor surfaceDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surfaceDesc.nextInChain = &canvas.chain;
    impl_->surface = wgpuInstanceCreateSurface(impl_->instance, &surfaceDesc);
    if (impl_->surface == nullptr) return;

    AdapterRequest adapterRequest;
    WGPURequestAdapterOptions adapterOptions = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    adapterOptions.featureLevel = WGPUFeatureLevel_Core;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;
    adapterOptions.compatibleSurface = impl_->surface;
    WGPURequestAdapterCallbackInfo adapterCallback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterCallback.mode = WGPUCallbackMode_WaitAnyOnly;
    adapterCallback.callback = onAdapter;
    adapterCallback.userdata1 = &adapterRequest;
    const WGPUFuture adapterFuture =
        wgpuInstanceRequestAdapter(impl_->instance, &adapterOptions, adapterCallback);
    if (!waitFor(impl_->instance, adapterFuture) || !adapterRequest.success) return;
    impl_->adapter = adapterRequest.adapter;

    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(impl_->surface, impl_->adapter, &capabilities) !=
        WGPUStatus_Success) return;
    for (std::size_t i = 0; i < capabilities.formatCount; ++i) {
        TextureFormat format{};
        if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
            format = TextureFormat::BGRA8_UNORM;
        } else if (capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            format = TextureFormat::RGBA8_UNORM;
        } else {
            continue;
        }
        if (impl_->surfaceFormats.empty()) impl_->preferredFormat = format;
        if (std::find(impl_->surfaceFormats.begin(), impl_->surfaceFormats.end(), format) ==
            impl_->surfaceFormats.end()) {
            impl_->surfaceFormats.push_back(format);
        }
    }
    if (capabilities.presentModeCount != 0) {
        impl_->presentModes.assign(capabilities.presentModes,
                                   capabilities.presentModes + capabilities.presentModeCount);
    }
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    // Emdawn's browser surface reports formats/modes but currently leaves the
    // optional usage mask empty; canvas surfaces are render attachments.
    if (impl_->surfaceFormats.empty() || impl_->presentModes.empty()) return;

    DeviceRequest deviceRequest;
    const WGPUFeatureName requiredFeature = WGPUFeatureName_Float32Filterable;
    if (wgpuAdapterHasFeature(impl_->adapter, requiredFeature) != WGPU_TRUE) {
        F4_LOG_ERROR() << "WebGPU adapter lacks float32-filterable, required by the "
                          "production environment-map contract";
        return;
    }
    std::vector<WGPUFeatureName> requiredFeatures{requiredFeature};
    if (wgpuAdapterHasFeature(impl_->adapter, WGPUFeatureName_DepthClipControl) == WGPU_TRUE) {
        requiredFeatures.push_back(WGPUFeatureName_DepthClipControl);
        impl_->depthClipControl = true;
    }
    WGPUDeviceDescriptor deviceDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    deviceDesc.requiredFeatureCount = requiredFeatures.size();
    deviceDesc.requiredFeatures = requiredFeatures.data();
    deviceDesc.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType, WGPUStringView message, void* userdata1, void*) {
            auto* errors = static_cast<std::uint64_t*>(userdata1);
            ++(*errors);
            logWebGpuError("validation", message);
        };
    deviceDesc.uncapturedErrorCallbackInfo.userdata1 = &impl_->validationErrors;
    deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceDesc.deviceLostCallbackInfo.callback =
        [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message,
           void* userdata1, void*) {
            if (reason != WGPUDeviceLostReason_Destroyed) {
                *static_cast<bool*>(userdata1) = true;
                logWebGpuError("device lost", message);
            }
        };
    deviceDesc.deviceLostCallbackInfo.userdata1 = &impl_->deviceLost;
    WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    deviceCallback.mode = WGPUCallbackMode_WaitAnyOnly;
    deviceCallback.callback = onDevice;
    deviceCallback.userdata1 = &deviceRequest;
    const WGPUFuture deviceFuture =
        wgpuAdapterRequestDevice(impl_->adapter, &deviceDesc, deviceCallback);
    if (!waitFor(impl_->instance, deviceFuture) || !deviceRequest.success) return;
    impl_->device = deviceRequest.device;
    impl_->queue = wgpuDeviceGetQueue(impl_->device);
    impl_->limitsValid =
        wgpuDeviceGetLimits(impl_->device, &impl_->limits) == WGPUStatus_Success;
}

WebGpuDevice::~WebGpuDevice() {
    if (impl_ == nullptr) return;
    if (impl_->device != nullptr) waitIdle();
    impl_->pipelines.forEach([](WebGpuPipeline* pipeline) {
        if (pipeline->render != nullptr) wgpuRenderPipelineRelease(pipeline->render);
        if (pipeline->compute != nullptr) wgpuComputePipelineRelease(pipeline->compute);
        if (pipeline->layout != nullptr) wgpuPipelineLayoutRelease(pipeline->layout);
        for (WGPUBindGroupLayout layout : pipeline->bindGroupLayouts) {
            wgpuBindGroupLayoutRelease(layout);
        }
    });
    impl_->shaders.forEach([](WebGpuShader* shader) {
        if (shader->object != nullptr) wgpuShaderModuleRelease(shader->object);
    });
    impl_->samplers.forEach([](WebGpuSampler* sampler) {
        if (sampler->object != nullptr) wgpuSamplerRelease(sampler->object);
    });
    impl_->textures.forEach([](WebGpuTexture* texture) {
        if (texture->view != nullptr) wgpuTextureViewRelease(texture->view);
        if (texture->object != nullptr) {
            wgpuTextureDestroy(texture->object);
            wgpuTextureRelease(texture->object);
        }
    });
    impl_->buffers.forEach([](WebGpuBuffer* buffer) {
        if (buffer->object != nullptr) {
            wgpuBufferDestroy(buffer->object);
            wgpuBufferRelease(buffer->object);
        }
    });
    if (impl_->surface != nullptr) {
        wgpuSurfaceUnconfigure(impl_->surface);
        wgpuSurfaceRelease(impl_->surface);
    }
    if (impl_->queue != nullptr) wgpuQueueRelease(impl_->queue);
    if (impl_->device != nullptr) {
        wgpuDeviceDestroy(impl_->device);
        wgpuDeviceRelease(impl_->device);
    }
    if (impl_->adapter != nullptr) wgpuAdapterRelease(impl_->adapter);
    if (impl_->instance != nullptr) wgpuInstanceRelease(impl_->instance);
    delete impl_;
}

auto WebGpuDevice::backend() const -> Backend { return Backend::WebGPU; }

auto WebGpuDevice::ready() const -> bool {
    return impl_ != nullptr && impl_->instance != nullptr && impl_->surface != nullptr &&
           impl_->adapter != nullptr && impl_->device != nullptr && impl_->queue != nullptr &&
           impl_->limitsValid && !impl_->deviceLost;
}

auto WebGpuDevice::preferredSurfaceFormat() const -> TextureFormat {
    return impl_->preferredFormat;
}

auto WebGpuDevice::supportsSurfaceFormat(TextureFormat format) const -> bool {
    return impl_ != nullptr &&
           std::find(impl_->surfaceFormats.begin(), impl_->surfaceFormats.end(), format) !=
               impl_->surfaceFormats.end();
}

auto WebGpuDevice::maxTextureDimension2D() const -> std::uint32_t {
    return impl_ != nullptr && impl_->limitsValid ? impl_->limits.maxTextureDimension2D : 0;
}

auto WebGpuDevice::validationErrorCount() const -> std::uint64_t {
    return impl_->validationErrors;
}

auto WebGpuDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.sizeBytes == 0 || desc.usage == BufferUsage::None ||
        desc.sizeBytes > impl_->limits.maxBufferSize ||
        initialData.size() > desc.sizeBytes) return GfxError::BadDescriptor;

    WebGpuBuffer record;
    record.rec.sizeBytes = desc.sizeBytes;
    record.rec.usage = desc.usage;
    record.allocationBytes = alignToFour(desc.sizeBytes);
    WGPUBufferDescriptor nativeDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    nativeDesc.size = record.allocationBytes;
    nativeDesc.usage = bufferUsage(desc.usage);
    nativeDesc.mappedAtCreation = initialData.empty() ? WGPU_FALSE : WGPU_TRUE;
    record.object = wgpuDeviceCreateBuffer(impl_->device, &nativeDesc);
    if (record.object == nullptr) return GfxError::OutOfMemory;
    if (!initialData.empty()) {
        void* mapped = wgpuBufferGetMappedRange(record.object, 0,
                                                static_cast<std::size_t>(record.allocationBytes));
        if (mapped == nullptr) {
            wgpuBufferDestroy(record.object);
            wgpuBufferRelease(record.object);
            return GfxError::OutOfMemory;
        }
        std::memset(mapped, 0, static_cast<std::size_t>(record.allocationBytes));
        std::memcpy(mapped, initialData.data(), initialData.size());
        wgpuBufferUnmap(record.object);
    }
    return impl_->buffers.create(std::move(record));
}

auto WebGpuDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.width > impl_->limits.maxTextureDimension2D ||
        desc.height > impl_->limits.maxTextureDimension2D ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height)) {
        return GfxError::BadDescriptor;
    }
    const WGPUTextureFormat format = webGpuFormat(desc.format);
    if (format == WGPUTextureFormat_Undefined) return GfxError::BadDescriptor;

    WebGpuTexture record;
    record.rec = GpuTexture{desc.format, desc.width, desc.height, desc.mipLevels};
    WGPUTextureDescriptor nativeDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    nativeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                       WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
    nativeDesc.dimension = WGPUTextureDimension_2D;
    nativeDesc.size = WGPUExtent3D{desc.width, desc.height, 1};
    nativeDesc.format = format;
    nativeDesc.mipLevelCount = desc.mipLevels;
    record.object = wgpuDeviceCreateTexture(impl_->device, &nativeDesc);
    if (record.object == nullptr) return GfxError::OutOfMemory;
    record.view = wgpuTextureCreateView(record.object, nullptr);
    if (record.view == nullptr) {
        wgpuTextureDestroy(record.object);
        wgpuTextureRelease(record.object);
        return GfxError::OutOfMemory;
    }
    return impl_->textures.create(std::move(record));
}

auto WebGpuDevice::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (!std::isfinite(desc.maxAnisotropy) || desc.maxAnisotropy < 1.0f ||
        desc.maxAnisotropy > 16.0f) {
        return GfxError::BadDescriptor;
    }
    WebGpuSampler record;
    record.rec = GpuSampler{desc.filter, desc.mipFilter, desc.addressU, desc.addressV,
                            desc.addressW, desc.maxAnisotropy};
    WGPUSamplerDescriptor nativeDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    nativeDesc.addressModeU = webGpuAddress(desc.addressU);
    nativeDesc.addressModeV = webGpuAddress(desc.addressV);
    nativeDesc.addressModeW = webGpuAddress(desc.addressW);
    const bool anisotropic = desc.maxAnisotropy > 1.0f;
    const bool minLinear = anisotropic || desc.filter == TextureFilter::MinMagLinear ||
                           desc.filter == TextureFilter::MinLinearMagNearest;
    const bool magLinear = anisotropic || desc.filter == TextureFilter::MinMagLinear ||
                           desc.filter == TextureFilter::MinNearestMagLinear;
    nativeDesc.minFilter = minLinear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    nativeDesc.magFilter = magLinear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    nativeDesc.mipmapFilter = anisotropic || desc.mipFilter == MipFilter::Linear
                                  ? WGPUMipmapFilterMode_Linear
                                  : WGPUMipmapFilterMode_Nearest;
    nativeDesc.lodMinClamp = 0.0f;
    nativeDesc.lodMaxClamp = 32.0f;
    nativeDesc.maxAnisotropy = static_cast<std::uint16_t>(desc.maxAnisotropy);
    record.object = wgpuDeviceCreateSampler(impl_->device, &nativeDesc);
    if (record.object == nullptr) return GfxError::OutOfMemory;
    return impl_->samplers.create(std::move(record));
}

auto WebGpuDevice::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.blob.isEmpty() || (desc.stage != ShaderStage::Vertex &&
                                desc.stage != ShaderStage::Fragment &&
                                desc.stage != ShaderStage::Compute)) {
        return GfxError::BadDescriptor;
    }
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = WGPUStringView{reinterpret_cast<const char*>(desc.blob.data()),
                                 desc.blob.size()};
    WGPUShaderModuleDescriptor nativeDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    nativeDesc.nextInChain = &source.chain;
    WebGpuShader record;
    record.rec.stage = desc.stage;
    record.object = wgpuDeviceCreateShaderModule(impl_->device, &nativeDesc);
    if (record.object == nullptr) return GfxError::BadDescriptor;
    CompilationRequest request;
    WGPUCompilationInfoCallbackInfo callback = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = onCompilationInfo;
    callback.userdata1 = &request;
    const WGPUFuture future = wgpuShaderModuleGetCompilationInfo(record.object, callback);
    if (!waitFor(impl_->instance, future) || !request.success || request.hasErrors) {
        wgpuShaderModuleRelease(record.object);
        return GfxError::BadDescriptor;
    }
    ++impl_->stats.shaderCompiled;
    return impl_->shaders.create(std::move(record));
}

auto WebGpuDevice::createPipeline(const PipelineStateDesc& desc)
    -> Result<PipelineHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.layout.pushConstantSizeBytes != 0) return GfxError::BadDescriptor;
    const bool compute = desc.computeShader.isValid();
    const bool hasVertex = desc.vertexShader.isValid();
    const bool hasFragment = desc.fragmentShader.isValid();
    if ((compute && (hasVertex || hasFragment)) ||
        (!compute && (!hasVertex || !hasFragment)) ||
        (compute && (!desc.vertexInput.bindings.isEmpty() ||
                     !desc.vertexInput.attributes.isEmpty() ||
                     !desc.colorAttachments.isEmpty() ||
                     desc.depthStencil.usesAttachment() ||
                     desc.raster.depthClampEnable))) {
        return GfxError::BadDescriptor;
    }
    if (!compute && desc.raster.depthClampEnable && !impl_->depthClipControl) {
        return GfxError::BackendUnavailable;
    }
    if (desc.vertexInput.bindings.size() > impl_->limits.maxVertexBuffers ||
        desc.vertexInput.attributes.size() > impl_->limits.maxVertexAttributes) {
        return GfxError::BadDescriptor;
    }

    WebGpuPipeline record;
    record.rec.desc = desc;
    std::vector<const BindGroup*> groups;
    if (!desc.layout.groups.isEmpty()) {
        std::uint32_t maxBindPoint = 0;
        for (const BindGroup& group : desc.layout.groups) {
            maxBindPoint = std::max(maxBindPoint, group.bindPoint);
        }
        if (maxBindPoint >= impl_->limits.maxBindGroups) return GfxError::BadDescriptor;
        if (static_cast<std::uint64_t>(maxBindPoint) + 1u +
                desc.vertexInput.bindings.size() >
            impl_->limits.maxBindGroupsPlusVertexBuffers) {
            return GfxError::BadDescriptor;
        }
        groups.resize(static_cast<std::size_t>(maxBindPoint) + 1, nullptr);
        for (const BindGroup& group : desc.layout.groups) {
            if (groups[group.bindPoint] != nullptr) return GfxError::BadDescriptor;
            groups[group.bindPoint] = &group;
        }
    }

    auto destroyLayouts = [&]() {
        if (record.layout != nullptr) {
            wgpuPipelineLayoutRelease(record.layout);
            record.layout = nullptr;
        }
        for (WGPUBindGroupLayout layout : record.bindGroupLayouts) {
            wgpuBindGroupLayoutRelease(layout);
        }
        record.bindGroupLayouts.clear();
    };
    const std::uint32_t allowedStageMask = compute
        ? shaderStageBit(ShaderStage::Compute)
        : shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment);
    const WGPUShaderStage defaultStages = compute
        ? WGPUShaderStage_Compute
        : WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    for (const BindGroup* group : groups) {
        std::vector<WGPUBindGroupLayoutEntry> entries;
        if (group != nullptr) {
            if (group->slots.size() > impl_->limits.maxBindingsPerBindGroup) {
                destroyLayouts();
                return GfxError::BadDescriptor;
            }
            entries.reserve(group->slots.size());
            for (const BindingSlot& slot : group->slots) {
                if (slot.count != 1 ||
                    slot.slot > (std::numeric_limits<std::uint32_t>::max() - 3u) / 4u ||
                    bindingIndex(slot.type, slot.slot) >= impl_->limits.maxBindingsPerBindGroup ||
                    (slot.stageMask & ~allowedStageMask) != 0) {
                    destroyLayouts();
                    return GfxError::BadDescriptor;
                }
                const std::uint32_t binding = bindingIndex(slot.type, slot.slot);
                if (std::any_of(entries.begin(), entries.end(), [&](const auto& existing) {
                        return existing.binding == binding;
                    })) {
                    destroyLayouts();
                    return GfxError::BadDescriptor;
                }
                WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                entry.binding = binding;
                entry.visibility = webGpuShaderStages(slot.stageMask, defaultStages);
                switch (slot.type) {
                case BindingType::UniformBuffer:
                    entry.buffer = WGPU_BUFFER_BINDING_LAYOUT_INIT;
                    entry.buffer.type = WGPUBufferBindingType_Uniform;
                    break;
                case BindingType::StorageBuffer:
                    entry.buffer = WGPU_BUFFER_BINDING_LAYOUT_INIT;
                    entry.buffer.type = compute ? WGPUBufferBindingType_Storage
                                                : WGPUBufferBindingType_ReadOnlyStorage;
                    break;
                case BindingType::Texture:
                    entry.texture = WGPU_TEXTURE_BINDING_LAYOUT_INIT;
                    entry.texture.sampleType = WGPUTextureSampleType_Float;
                    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
                    break;
                case BindingType::Sampler:
                    entry.sampler = WGPU_SAMPLER_BINDING_LAYOUT_INIT;
                    entry.sampler.type = WGPUSamplerBindingType_Filtering;
                    break;
                }
                entries.push_back(entry);
            }
        }
        WGPUBindGroupLayoutDescriptor layoutDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layoutDesc.entryCount = entries.size();
        layoutDesc.entries = entries.empty() ? nullptr : entries.data();
        WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(impl_->device, &layoutDesc);
        if (layout == nullptr) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        record.bindGroupLayouts.push_back(layout);
    }
    WGPUPipelineLayoutDescriptor layoutDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.bindGroupLayoutCount = record.bindGroupLayouts.size();
    layoutDesc.bindGroupLayouts = record.bindGroupLayouts.empty()
                                      ? nullptr
                                      : record.bindGroupLayouts.data();
    record.layout = wgpuDeviceCreatePipelineLayout(impl_->device, &layoutDesc);
    if (record.layout == nullptr) {
        destroyLayouts();
        return GfxError::BadDescriptor;
    }
    if (compute) {
        const WebGpuShader* shader = impl_->shaders.get(desc.computeShader);
        if (shader == nullptr) {
            destroyLayouts();
            return GfxError::InvalidHandle;
        }
        if (shader->rec.stage != ShaderStage::Compute) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        WGPUComputePipelineDescriptor nativeDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        nativeDesc.layout = record.layout;
        nativeDesc.compute.module = shader->object;
        nativeDesc.compute.entryPoint = viewOf("cs_main");
        ComputePipelineRequest request;
        WGPUCreateComputePipelineAsyncCallbackInfo callback =
            WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = onComputePipeline;
        callback.userdata1 = &request;
        const WGPUFuture future =
            wgpuDeviceCreateComputePipelineAsync(impl_->device, &nativeDesc, callback);
        if (!waitFor(impl_->instance, future) || !request.success) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        record.compute = request.pipeline;
        return impl_->pipelines.create(std::move(record));
    }

    const WebGpuShader* vertex = impl_->shaders.get(desc.vertexShader);
    const WebGpuShader* fragment = impl_->shaders.get(desc.fragmentShader);
    if (vertex == nullptr || fragment == nullptr) {
        destroyLayouts();
        return GfxError::InvalidHandle;
    }
    if (vertex->rec.stage != ShaderStage::Vertex ||
        fragment->rec.stage != ShaderStage::Fragment) {
        destroyLayouts();
        return GfxError::BadDescriptor;
    }

    std::vector<std::vector<WGPUVertexAttribute>> attributes(desc.vertexInput.bindings.size());
    for (const auto& binding : desc.vertexInput.bindings) {
        if (binding.binding >= attributes.size() || binding.strideBytes == 0 ||
            binding.strideBytes > impl_->limits.maxVertexBufferArrayStride ||
            binding.divisor > 1) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
    }
    for (const auto& attribute : desc.vertexInput.attributes) {
        if (attribute.binding >= attributes.size() ||
            attribute.offsetBytes + vertexFormatSize(attribute.format) >
                desc.vertexInput.bindings[attribute.binding].strideBytes) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        WGPUVertexAttribute native = WGPU_VERTEX_ATTRIBUTE_INIT;
        native.format = webGpuVertexFormat(attribute.format);
        native.offset = attribute.offsetBytes;
        native.shaderLocation = attribute.location;
        attributes[attribute.binding].push_back(native);
    }
    std::vector<WGPUVertexBufferLayout> layouts(desc.vertexInput.bindings.size());
    for (std::size_t i = 0; i < desc.vertexInput.bindings.size(); ++i) {
        const auto& binding = desc.vertexInput.bindings[i];
        if (binding.binding != i) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        layouts[i] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        layouts[i].arrayStride = binding.strideBytes;
        layouts[i].stepMode = binding.divisor == 0 ? WGPUVertexStepMode_Vertex
                                                   : WGPUVertexStepMode_Instance;
        layouts[i].attributeCount = attributes[i].size();
        layouts[i].attributes = attributes[i].data();
    }

    const std::size_t targetCount = desc.colorAttachments.isEmpty()
                                        ? 1
                                        : desc.colorAttachments.size();
    if (targetCount > impl_->limits.maxColorAttachments) {
        destroyLayouts();
        return GfxError::BadDescriptor;
    }
    std::vector<WGPUBlendState> blends(targetCount);
    std::vector<WGPUColorTargetState> targets(targetCount);
    for (std::size_t i = 0; i < targetCount; ++i) {
        const ColorAttachmentState state = desc.colorAttachments.isEmpty()
                                               ? ColorAttachmentState{}
                                               : desc.colorAttachments[i];
        targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
        targets[i].format = webGpuFormat(state.format);
        targets[i].writeMask = webGpuWriteMask(state.writeMask);
        if (targets[i].format == WGPUTextureFormat_Undefined || isDepthFormat(state.format)) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        if (state.blendEnable) {
            blends[i] = WGPU_BLEND_STATE_INIT;
            blends[i].color.srcFactor = webGpuBlendFactor(state.srcColor);
            blends[i].color.dstFactor = webGpuBlendFactor(state.dstColor);
            blends[i].color.operation = webGpuBlendOp(state.colorOp);
            blends[i].alpha.srcFactor = webGpuBlendFactor(state.srcAlpha);
            blends[i].alpha.dstFactor = webGpuBlendFactor(state.dstAlpha);
            blends[i].alpha.operation = webGpuBlendOp(state.alphaOp);
            targets[i].blend = &blends[i];
        }
    }

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = fragment->object;
    fragmentState.entryPoint = viewOf("fs_main");
    fragmentState.targetCount = targets.size();
    fragmentState.targets = targets.data();

    WGPUDepthStencilState depthState = WGPU_DEPTH_STENCIL_STATE_INIT;
    WGPURenderPipelineDescriptor nativeDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    nativeDesc.layout = record.layout;
    nativeDesc.vertex.module = vertex->object;
    nativeDesc.vertex.entryPoint = viewOf("vs_main");
    nativeDesc.vertex.bufferCount = layouts.size();
    nativeDesc.vertex.buffers = layouts.data();
    nativeDesc.primitive.topology = webGpuTopology(desc.topology);
    if (desc.topology == PrimitiveTopology::TriangleStrip) {
        nativeDesc.primitive.stripIndexFormat = webGpuIndexFormat(desc.stripIndexFormat);
    }
    nativeDesc.primitive.frontFace = WGPUFrontFace_CCW;
    nativeDesc.primitive.cullMode = webGpuCull(desc.raster.cullMode);
    nativeDesc.primitive.unclippedDepth = desc.raster.depthClampEnable ? WGPU_TRUE : WGPU_FALSE;
    nativeDesc.fragment = &fragmentState;
    if (desc.depthStencil.usesAttachment()) {
        if (!isDepthFormat(desc.depthStencil.format)) {
            destroyLayouts();
            return GfxError::BadDescriptor;
        }
        depthState.format = webGpuFormat(desc.depthStencil.format);
        depthState.depthWriteEnabled = desc.depthStencil.depthWriteEnable
                                           ? WGPUOptionalBool_True
                                           : WGPUOptionalBool_False;
        depthState.depthCompare = desc.depthStencil.depthTestEnable
                                      ? webGpuCompare(desc.depthStencil.depthCompare)
                                      : WGPUCompareFunction_Always;
        nativeDesc.depthStencil = &depthState;
    }
    RenderPipelineRequest request;
    WGPUCreateRenderPipelineAsyncCallbackInfo callback =
        WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = onRenderPipeline;
    callback.userdata1 = &request;
    const WGPUFuture future =
        wgpuDeviceCreateRenderPipelineAsync(impl_->device, &nativeDesc, callback);
    if (!waitFor(impl_->instance, future) || !request.success) {
        destroyLayouts();
        return GfxError::BadDescriptor;
    }
    record.render = request.pipeline;
    return impl_->pipelines.create(std::move(record));
}

auto WebGpuDevice::createSwapchain(const SwapchainDesc& desc)
    -> Result<SwapchainHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.extent.width > impl_->limits.maxTextureDimension2D ||
        desc.extent.height > impl_->limits.maxTextureDimension2D ||
        !supportsSurfaceFormat(desc.surfaceFormat) || impl_->activeSwapchain.isValid() ||
        std::find(impl_->presentModes.begin(), impl_->presentModes.end(),
                  webGpuPresentMode(desc.presentMode)) == impl_->presentModes.end()) {
        return GfxError::BadDescriptor;
    }
    WebGpuSwapchain record;
    record.rec = GpuSwapchain{desc.surfaceFormat, desc.extent, desc.presentMode};
    const SwapchainHandle handle = impl_->swapchains.create(std::move(record));
    impl_->activeSwapchain = handle;
    impl_->configureSurface(*impl_->swapchains.get(handle));
    return handle;
}

auto WebGpuDevice::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (!ready()) return GfxError::BackendUnavailable;
    if (desc.extent.width == 0 || desc.extent.height == 0 || desc.attachments.isEmpty()) {
        return GfxError::BadDescriptor;
    }
    std::size_t surfaceAttachmentCount = 0;
    for (const AttachmentDesc& attachment : desc.attachments) {
        if (!attachment.texture.isValid()) {
            if (attachment.type != AttachmentType::Color || !impl_->activeSwapchain.isValid()) {
                return GfxError::BadDescriptor;
            }
            const WebGpuSwapchain* swapchain =
                impl_->swapchains.get(impl_->activeSwapchain);
            if (swapchain == nullptr || swapchain->rec.extent != desc.extent ||
                ++surfaceAttachmentCount > 1) return GfxError::BadDescriptor;
            continue;
        }
        const WebGpuTexture* texture = impl_->textures.get(attachment.texture);
        if (texture == nullptr) return GfxError::InvalidHandle;
        if (texture->rec.width != desc.extent.width || texture->rec.height != desc.extent.height ||
            (attachment.type == AttachmentType::Depth) != isDepthFormat(texture->rec.format)) {
            return GfxError::BadDescriptor;
        }
    }
    WebGpuFramebuffer record;
    record.rec.extent = desc.extent;
    record.rec.attachments = desc.attachments;
    return impl_->framebuffers.create(std::move(record));
}

auto WebGpuDevice::destroyBuffer(BufferHandle handle) -> GfxError {
    WebGpuBuffer* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    wgpuBufferDestroy(record->object);
    wgpuBufferRelease(record->object);
    record->object = nullptr;
    impl_->buffers.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroyTexture(TextureHandle handle) -> GfxError {
    WebGpuTexture* record = impl_->textures.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    wgpuTextureViewRelease(record->view);
    wgpuTextureDestroy(record->object);
    wgpuTextureRelease(record->object);
    record->view = nullptr;
    record->object = nullptr;
    impl_->textures.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroySampler(SamplerHandle handle) -> GfxError {
    WebGpuSampler* record = impl_->samplers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    wgpuSamplerRelease(record->object);
    record->object = nullptr;
    impl_->samplers.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroyShader(ShaderHandle handle) -> GfxError {
    WebGpuShader* record = impl_->shaders.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    wgpuShaderModuleRelease(record->object);
    record->object = nullptr;
    impl_->shaders.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroyPipeline(PipelineHandle handle) -> GfxError {
    WebGpuPipeline* record = impl_->pipelines.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (record->render != nullptr) wgpuRenderPipelineRelease(record->render);
    if (record->compute != nullptr) wgpuComputePipelineRelease(record->compute);
    if (record->layout != nullptr) wgpuPipelineLayoutRelease(record->layout);
    for (WGPUBindGroupLayout layout : record->bindGroupLayouts) {
        wgpuBindGroupLayoutRelease(layout);
    }
    record->render = nullptr;
    record->compute = nullptr;
    record->layout = nullptr;
    record->bindGroupLayouts.clear();
    impl_->pipelines.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroySwapchain(SwapchainHandle handle) -> GfxError {
    if (!impl_->swapchains.contains(handle)) return GfxError::StaleHandle;
    if (handle == impl_->activeSwapchain) {
        wgpuSurfaceUnconfigure(impl_->surface);
        impl_->activeSwapchain = {};
    }
    impl_->swapchains.destroy(handle);
    return GfxError::None;
}

auto WebGpuDevice::destroyFramebuffer(FramebufferHandle handle) -> GfxError {
    return impl_->framebuffers.destroy(handle) ? GfxError::None : GfxError::StaleHandle;
}

auto WebGpuDevice::writeBuffer(BufferHandle handle, std::uint64_t offset,
                               std::span<const std::byte> data) -> GfxError {
    WebGpuBuffer* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (offset > record->rec.sizeBytes || data.size() > record->rec.sizeBytes - offset ||
        (offset & 3u) != 0 || (data.size() & 3u) != 0) return GfxError::BadDescriptor;
    if (!data.empty()) {
        wgpuQueueWriteBuffer(impl_->queue, record->object, offset, data.data(), data.size());
    }
    return GfxError::None;
}

auto WebGpuDevice::readBuffer(BufferHandle handle, std::uint64_t offset,
                              std::span<std::byte> destination) -> GfxError {
    WebGpuBuffer* source = impl_->buffers.get(handle);
    if (source == nullptr) return GfxError::StaleHandle;
    if (offset > source->rec.sizeBytes || destination.size() > source->rec.sizeBytes - offset ||
        (offset & 3u) != 0 || (destination.size() & 3u) != 0) return GfxError::BadDescriptor;
    if (destination.empty()) return GfxError::None;

    WGPUBufferDescriptor stagingDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    stagingDesc.size = destination.size();
    stagingDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(impl_->device, &stagingDesc);
    if (staging == nullptr) return GfxError::OutOfMemory;
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    if (encoder == nullptr) {
        wgpuBufferRelease(staging);
        return GfxError::OutOfMemory;
    }
    wgpuCommandEncoderCopyBufferToBuffer(encoder, source->object, offset, staging, 0,
                                         destination.size());
    WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (command == nullptr) {
        wgpuBufferRelease(staging);
        return GfxError::DeviceLost;
    }
    wgpuQueueSubmit(impl_->queue, 1, &command);
    wgpuCommandBufferRelease(command);

    MapRequest request;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = onMap;
    callback.userdata1 = &request;
    const WGPUFuture future = wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0,
                                                 destination.size(), callback);
    const bool mapped = waitFor(impl_->instance, future) && request.success;
    if (mapped) {
        const void* bytes = wgpuBufferGetConstMappedRange(staging, 0, destination.size());
        if (bytes != nullptr) std::memcpy(destination.data(), bytes, destination.size());
        else request.success = false;
        wgpuBufferUnmap(staging);
    }
    wgpuBufferDestroy(staging);
    wgpuBufferRelease(staging);
    return request.success ? GfxError::None : GfxError::DeviceLost;
}

auto WebGpuDevice::readTexture(TextureHandle handle, std::uint32_t mipLevel,
                               std::span<std::byte> destination) -> GfxError {
    WebGpuTexture* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels || isDepthFormat(texture->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(1u, texture->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, texture->rec.height >> mipLevel);
    const std::uint64_t rowBytes =
        static_cast<std::uint64_t>(width) * textureFormatBytes(texture->rec.format);
    if (rowBytes == 0 || rowBytes > std::numeric_limits<std::uint32_t>::max() ||
        destination.size() != rowBytes * height) {
        return GfxError::BadDescriptor;
    }
    constexpr std::uint64_t copyRowAlignment = 256;
    const std::uint64_t paddedRowBytes =
        (rowBytes + copyRowAlignment - 1u) & ~(copyRowAlignment - 1u);
    if (paddedRowBytes > std::numeric_limits<std::uint32_t>::max() ||
        height > impl_->limits.maxBufferSize / paddedRowBytes) {
        return GfxError::BadDescriptor;
    }
    const std::uint64_t stagingSize = paddedRowBytes * height;

    WGPUBufferDescriptor stagingDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    stagingDesc.size = stagingSize;
    stagingDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(impl_->device, &stagingDesc);
    if (staging == nullptr) return GfxError::OutOfMemory;
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    if (encoder == nullptr) {
        wgpuBufferRelease(staging);
        return GfxError::OutOfMemory;
    }
    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    source.texture = texture->object;
    source.mipLevel = mipLevel;
    source.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo target = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    target.layout.bytesPerRow = static_cast<std::uint32_t>(paddedRowBytes);
    target.layout.rowsPerImage = height;
    target.buffer = staging;
    const WGPUExtent3D extent{width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &target, &extent);
    WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (command == nullptr) {
        wgpuBufferRelease(staging);
        return GfxError::DeviceLost;
    }
    wgpuQueueSubmit(impl_->queue, 1, &command);
    wgpuCommandBufferRelease(command);

    MapRequest request;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = onMap;
    callback.userdata1 = &request;
    const WGPUFuture future =
        wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, stagingSize, callback);
    const bool mapped = waitFor(impl_->instance, future) && request.success;
    if (mapped) {
        const auto* sourceBytes = static_cast<const std::byte*>(
            wgpuBufferGetConstMappedRange(staging, 0, stagingSize));
        if (sourceBytes != nullptr) {
            for (std::uint32_t row = 0; row < height; ++row) {
                std::memcpy(destination.data() + static_cast<std::size_t>(rowBytes) * row,
                            sourceBytes + static_cast<std::size_t>(paddedRowBytes) * row,
                            static_cast<std::size_t>(rowBytes));
            }
        } else {
            request.success = false;
        }
        wgpuBufferUnmap(staging);
    }
    wgpuBufferDestroy(staging);
    wgpuBufferRelease(staging);
    return request.success ? GfxError::None : GfxError::DeviceLost;
}

auto WebGpuDevice::writeTexture(TextureHandle handle, std::uint32_t mipLevel,
                                std::span<const std::byte> data) -> GfxError {
    WebGpuTexture* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels || isDepthFormat(texture->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(1u, texture->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, texture->rec.height >> mipLevel);
    const std::uint64_t bytesPerRow =
        static_cast<std::uint64_t>(width) * textureFormatBytes(texture->rec.format);
    if (data.size() != bytesPerRow * height ||
        bytesPerRow > std::numeric_limits<std::uint32_t>::max()) {
        return GfxError::BadDescriptor;
    }
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture->object;
    destination.mipLevel = mipLevel;
    destination.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = static_cast<std::uint32_t>(bytesPerRow);
    layout.rowsPerImage = height;
    const WGPUExtent3D extent{width, height, 1};
    wgpuQueueWriteTexture(impl_->queue, &destination, data.data(), data.size(), &layout, &extent);
    return GfxError::None;
}

auto WebGpuDevice::submit(CommandBuffer& commands) -> GfxError {
    if (!ready()) return impl_->deviceLost ? GfxError::DeviceLost : GfxError::BackendUnavailable;
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(impl_->device, nullptr);
    if (encoder == nullptr) return GfxError::DeviceLost;

    WGPURenderPassEncoder renderPass = nullptr;
    WGPUComputePassEncoder computePass = nullptr;
    WebGpuPipeline* pipeline = nullptr;
    bool surfaceAcquired = false;
    WGPUTexture surfaceTexture = nullptr;
    WGPUTextureView surfaceView = nullptr;
    bool indexBound = false;
    IndexFormat boundIndexFormat = IndexFormat::U16;
    std::vector<WGPUBindGroup> transientBindGroups;
    std::vector<BoundResource> boundResources;
    DeviceStats delta;
    GfxError error = GfxError::None;

    auto findBound = [&](std::uint32_t bindPoint, const BindingSlot& slot)
        -> const BoundResource* {
        const auto found = std::find_if(
            boundResources.begin(), boundResources.end(), [&](const BoundResource& resource) {
                return resource.bindPoint == bindPoint && resource.slot == slot.slot &&
                       resource.type == slot.type;
            });
        return found == boundResources.end() ? nullptr : &*found;
    };
    auto bindGroups = [&](WebGpuPipeline& boundPipeline) -> GfxError {
        for (const BindGroup& group : boundPipeline.rec.desc.layout.groups) {
            std::vector<WGPUBindGroupEntry> entries;
            entries.reserve(group.slots.size());
            for (const BindingSlot& slot : group.slots) {
                const BoundResource* bound = findBound(group.bindPoint, slot);
                if (bound == nullptr) return GfxError::BadDescriptor;
                WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
                entry.binding = bindingIndex(slot.type, slot.slot);
                if (slot.type == BindingType::Texture) {
                    TextureHandle handle{bound->handleIndex, bound->handleGeneration};
                    WebGpuTexture* texture = impl_->textures.get(handle);
                    if (texture == nullptr) return GfxError::InvalidHandle;
                    if (isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
                    entry.textureView = texture->view;
                } else if (slot.type == BindingType::Sampler) {
                    SamplerHandle handle{bound->handleIndex, bound->handleGeneration};
                    WebGpuSampler* sampler = impl_->samplers.get(handle);
                    if (sampler == nullptr) return GfxError::InvalidHandle;
                    entry.sampler = sampler->object;
                } else {
                    BufferHandle handle{bound->handleIndex, bound->handleGeneration};
                    WebGpuBuffer* buffer = impl_->buffers.get(handle);
                    if (buffer == nullptr) return GfxError::InvalidHandle;
                    const bool uniform = slot.type == BindingType::UniformBuffer;
                    const BufferUsage required = uniform ? BufferUsage::Uniform
                                                         : BufferUsage::Storage;
                    const std::uint64_t limit = uniform
                        ? impl_->limits.maxUniformBufferBindingSize
                        : impl_->limits.maxStorageBufferBindingSize;
                    if (!hasUsage(buffer->rec.usage, required) || buffer->rec.sizeBytes > limit ||
                        (!uniform && (buffer->rec.sizeBytes & 3u) != 0)) {
                        return GfxError::BadDescriptor;
                    }
                    entry.buffer = buffer->object;
                    entry.size = buffer->rec.sizeBytes;
                }
                entries.push_back(entry);
            }
            WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            desc.layout = boundPipeline.bindGroupLayouts[group.bindPoint];
            desc.entryCount = entries.size();
            desc.entries = entries.empty() ? nullptr : entries.data();
            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(impl_->device, &desc);
            if (bindGroup == nullptr) return GfxError::BadDescriptor;
            transientBindGroups.push_back(bindGroup);
            if (renderPass != nullptr && boundPipeline.render != nullptr) {
                wgpuRenderPassEncoderSetBindGroup(renderPass, group.bindPoint, bindGroup, 0,
                                                  nullptr);
            } else if (computePass != nullptr && boundPipeline.compute != nullptr) {
                wgpuComputePassEncoderSetBindGroup(computePass, group.bindPoint, bindGroup, 0,
                                                   nullptr);
            } else {
                return GfxError::BadDescriptor;
            }
        }
        return GfxError::None;
    };

    for (const Command& command : commands.commands()) {
        if (error != GfxError::None) break;
        switch (command.kind) {
        case CommandKind::ResourceBarrier:
            if (command.barrierType == BarrierResourceType::Buffer) {
                if (!impl_->buffers.contains(command.buffer)) error = GfxError::InvalidHandle;
            } else if (!impl_->textures.contains(command.texture)) {
                error = GfxError::InvalidHandle;
            }
            break;
        case CommandKind::BeginRenderPass: {
            if (renderPass != nullptr || computePass != nullptr) {
                error = GfxError::BadDescriptor;
                break;
            }
            WebGpuFramebuffer* framebuffer = impl_->framebuffers.get(command.framebuffer);
            if (framebuffer == nullptr) {
                error = GfxError::InvalidHandle;
                break;
            }
            std::vector<WGPURenderPassColorAttachment> colors;
            WGPURenderPassDepthStencilAttachment depth =
                WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            bool hasDepth = false;
            for (const AttachmentDesc& attachment : framebuffer->rec.attachments) {
                WGPUTextureView view = nullptr;
                TextureFormat attachmentFormat = impl_->preferredFormat;
                if (attachment.texture.isValid()) {
                    WebGpuTexture* texture = impl_->textures.get(attachment.texture);
                    if (texture == nullptr) {
                        error = GfxError::InvalidHandle;
                        break;
                    }
                    view = texture->view;
                    attachmentFormat = texture->rec.format;
                } else {
                    if (surfaceAcquired || !impl_->activeSwapchain.isValid()) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
                    wgpuSurfaceGetCurrentTexture(impl_->surface, &current);
                    if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                        current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                        WebGpuSwapchain* swapchain =
                            impl_->swapchains.get(impl_->activeSwapchain);
                        if (swapchain != nullptr &&
                            (current.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
                             current.status == WGPUSurfaceGetCurrentTextureStatus_Lost)) {
                            impl_->configureSurface(*swapchain);
                            current = WGPU_SURFACE_TEXTURE_INIT;
                            wgpuSurfaceGetCurrentTexture(impl_->surface, &current);
                        }
                    }
                    if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                        current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                        error = GfxError::DeviceLost;
                        break;
                    }
                    surfaceTexture = current.texture;
                    surfaceView = wgpuTextureCreateView(surfaceTexture, nullptr);
                    if (surfaceView == nullptr) {
                        error = GfxError::DeviceLost;
                        break;
                    }
                    view = surfaceView;
                    surfaceAcquired = true;
                }
                if (attachment.type == AttachmentType::Color) {
                    WGPURenderPassColorAttachment color =
                        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                    color.view = view;
                    color.loadOp = attachment.clearOnLoad ? WGPULoadOp_Clear : WGPULoadOp_Load;
                    color.storeOp = WGPUStoreOp_Store;
                    color.clearValue = WGPUColor{attachment.clear.color[0],
                                                 attachment.clear.color[1],
                                                 attachment.clear.color[2],
                                                 attachment.clear.color[3]};
                    colors.push_back(color);
                } else {
                    if (hasDepth) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                    hasDepth = true;
                    depth.view = view;
                    depth.depthLoadOp = attachment.clearOnLoad ? WGPULoadOp_Clear : WGPULoadOp_Load;
                    depth.depthStoreOp = WGPUStoreOp_Store;
                    depth.depthClearValue = attachment.clear.depth;
                    depth.depthReadOnly = WGPU_FALSE;
                    if (attachmentFormat == TextureFormat::Depth24Stencil8) {
                        depth.stencilLoadOp = attachment.clearOnLoad
                                                  ? WGPULoadOp_Clear
                                                  : WGPULoadOp_Load;
                        depth.stencilStoreOp = WGPUStoreOp_Store;
                        depth.stencilClearValue = attachment.clear.stencil;
                        depth.stencilReadOnly = WGPU_FALSE;
                    }
                }
            }
            if (error != GfxError::None) break;
            WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            passDesc.colorAttachmentCount = colors.size();
            passDesc.colorAttachments = colors.empty() ? nullptr : colors.data();
            passDesc.depthStencilAttachment = hasDepth ? &depth : nullptr;
            renderPass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
            if (renderPass == nullptr) error = GfxError::DeviceLost;
            else {
                pipeline = nullptr;
                indexBound = false;
            }
            break;
        }
        case CommandKind::EndRenderPass:
            if (renderPass == nullptr) error = GfxError::BadDescriptor;
            else {
                wgpuRenderPassEncoderEnd(renderPass);
                wgpuRenderPassEncoderRelease(renderPass);
                renderPass = nullptr;
            }
            break;
        case CommandKind::BindPipeline:
            pipeline = impl_->pipelines.get(command.pipeline);
            if (pipeline == nullptr) {
                error = GfxError::InvalidHandle;
            } else if (renderPass != nullptr && pipeline->render != nullptr) {
                wgpuRenderPassEncoderSetPipeline(renderPass, pipeline->render);
            } else if (renderPass == nullptr && pipeline->compute != nullptr) {
                if (computePass == nullptr) {
                    computePass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
                }
                wgpuComputePassEncoderSetPipeline(computePass, pipeline->compute);
            } else {
                error = GfxError::BadDescriptor;
            }
            break;
        case CommandKind::BindVertexBuffer: {
            WebGpuBuffer* buffer = impl_->buffers.get(command.buffer);
            if (renderPass == nullptr || buffer == nullptr) {
                error = buffer == nullptr ? GfxError::InvalidHandle : GfxError::BadDescriptor;
            } else if (!hasUsage(buffer->rec.usage, BufferUsage::Vertex) ||
                       command.vertexSlot >= impl_->limits.maxVertexBuffers ||
                       command.strideBytes == 0 ||
                       command.strideBytes > buffer->rec.sizeBytes) {
                error = GfxError::BadDescriptor;
            } else {
                wgpuRenderPassEncoderSetVertexBuffer(renderPass, command.vertexSlot,
                                                     buffer->object, 0, buffer->rec.sizeBytes);
            }
            break;
        }
        case CommandKind::BindIndexBuffer: {
            WebGpuBuffer* buffer = impl_->buffers.get(command.buffer);
            if (renderPass == nullptr || buffer == nullptr) {
                error = buffer == nullptr ? GfxError::InvalidHandle : GfxError::BadDescriptor;
            } else if (!hasUsage(buffer->rec.usage, BufferUsage::Index)) {
                error = GfxError::BadDescriptor;
            } else {
                wgpuRenderPassEncoderSetIndexBuffer(renderPass, buffer->object,
                                                    webGpuIndexFormat(command.indexFormat), 0,
                                                    buffer->rec.sizeBytes);
                indexBound = true;
                boundIndexFormat = command.indexFormat;
            }
            break;
        }
        case CommandKind::BindResources:
            for (std::uint8_t index = 0; index < command.resourceCount; ++index) {
                const BoundResource& resource = command.resources[index];
                if (pipeline != nullptr) {
                    const auto group = std::find_if(
                        pipeline->rec.desc.layout.groups.begin(),
                        pipeline->rec.desc.layout.groups.end(),
                        [&](const BindGroup& item) {
                            return item.bindPoint == resource.bindPoint;
                        });
                    if (group == pipeline->rec.desc.layout.groups.end() ||
                        std::none_of(group->slots.begin(), group->slots.end(),
                                     [&](const BindingSlot& slot) {
                                         return slot.slot == resource.slot &&
                                                slot.type == resource.type;
                                     })) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                }
                const auto existing = std::find_if(
                    boundResources.begin(), boundResources.end(),
                    [&](const BoundResource& item) {
                        return item.bindPoint == resource.bindPoint &&
                               item.slot == resource.slot && item.type == resource.type;
                    });
                if (existing == boundResources.end()) boundResources.push_back(resource);
                else *existing = resource;
            }
            break;
        case CommandKind::SetViewport:
            if (renderPass == nullptr || command.viewport.width == 0 ||
                command.viewport.height == 0) error = GfxError::BadDescriptor;
            else wgpuRenderPassEncoderSetViewport(renderPass,
                                                  static_cast<float>(command.viewportX),
                                                  static_cast<float>(command.viewportY),
                                                  static_cast<float>(command.viewport.width),
                                                  static_cast<float>(command.viewport.height),
                                                  0.0f, 1.0f);
            break;
        case CommandKind::SetScissor:
            if (renderPass == nullptr || command.scissor.width == 0 ||
                command.scissor.height == 0) error = GfxError::BadDescriptor;
            else wgpuRenderPassEncoderSetScissorRect(renderPass, command.scissorX,
                                                     command.scissorY, command.scissor.width,
                                                     command.scissor.height);
            break;
        case CommandKind::Draw:
            if (renderPass == nullptr || pipeline == nullptr || pipeline->render == nullptr) {
                error = GfxError::BadDescriptor;
            } else if ((error = bindGroups(*pipeline)) != GfxError::None) {
                break;
            } else {
                wgpuRenderPassEncoderDraw(renderPass, command.vertexCount,
                                          command.instanceCount, 0, 0);
                ++delta.drawCalls;
                ++delta.batches;
                delta.instances += command.instanceCount;
                delta.vertices +=
                    static_cast<std::uint64_t>(command.vertexCount) * command.instanceCount;
                delta.primitives += countPrimitives(pipeline->rec.desc.topology,
                                                    command.vertexCount) *
                                    command.instanceCount;
            }
            break;
        case CommandKind::DrawIndexed:
            if (renderPass == nullptr || pipeline == nullptr || pipeline->render == nullptr) {
                error = GfxError::BadDescriptor;
            } else if (!indexBound ||
                       (pipeline->rec.desc.topology == PrimitiveTopology::TriangleStrip &&
                        pipeline->rec.desc.stripIndexFormat != boundIndexFormat)) {
                error = GfxError::BadDescriptor;
            } else if ((error = bindGroups(*pipeline)) != GfxError::None) {
                break;
            } else {
                wgpuRenderPassEncoderDrawIndexed(renderPass, command.indexCount,
                                                 command.instanceCount, command.firstIndex, 0, 0);
                ++delta.drawIndexedCalls;
                ++delta.batches;
                delta.instances += command.instanceCount;
                delta.vertices +=
                    static_cast<std::uint64_t>(command.indexCount) * command.instanceCount;
                delta.primitives += countPrimitives(pipeline->rec.desc.topology,
                                                    command.indexCount) *
                                    command.instanceCount;
            }
            break;
        case CommandKind::Dispatch:
            if (computePass == nullptr || pipeline == nullptr || pipeline->compute == nullptr) {
                error = GfxError::BadDescriptor;
            } else if (command.groupCountX == 0 || command.groupCountY == 0 ||
                       command.groupCountZ == 0 ||
                       command.groupCountX > impl_->limits.maxComputeWorkgroupsPerDimension ||
                       command.groupCountY > impl_->limits.maxComputeWorkgroupsPerDimension ||
                       command.groupCountZ > impl_->limits.maxComputeWorkgroupsPerDimension) {
                error = GfxError::BadDescriptor;
            } else if ((error = bindGroups(*pipeline)) != GfxError::None) {
                break;
            } else {
                wgpuComputePassEncoderDispatchWorkgroups(computePass, command.groupCountX,
                                                         command.groupCountY, command.groupCountZ);
                ++delta.dispatchCalls;
            }
            break;
        }
    }

    if (renderPass != nullptr) {
        wgpuRenderPassEncoderEnd(renderPass);
        wgpuRenderPassEncoderRelease(renderPass);
        if (error == GfxError::None) error = GfxError::BadDescriptor;
    }
    if (computePass != nullptr) {
        wgpuComputePassEncoderEnd(computePass);
        wgpuComputePassEncoderRelease(computePass);
    }
    if (error == GfxError::None) {
        WGPUCommandBuffer nativeCommands = wgpuCommandEncoderFinish(encoder, nullptr);
        if (nativeCommands == nullptr) error = GfxError::DeviceLost;
        else {
            wgpuQueueSubmit(impl_->queue, 1, &nativeCommands);
            wgpuCommandBufferRelease(nativeCommands);
            // Browser canvas textures are presented at the requestAnimationFrame
            // boundary. Emdawn deliberately rejects wgpuSurfacePresent().
            impl_->stats.drawCalls += delta.drawCalls;
            impl_->stats.drawIndexedCalls += delta.drawIndexedCalls;
            impl_->stats.dispatchCalls += delta.dispatchCalls;
            impl_->stats.batches += delta.batches;
            impl_->stats.instances += delta.instances;
            impl_->stats.primitives += delta.primitives;
            impl_->stats.vertices += delta.vertices;
        }
    }
    wgpuCommandEncoderRelease(encoder);
    for (WGPUBindGroup bindGroup : transientBindGroups) wgpuBindGroupRelease(bindGroup);
    if (surfaceView != nullptr) wgpuTextureViewRelease(surfaceView);
    if (surfaceTexture != nullptr) wgpuTextureRelease(surfaceTexture);
    return error;
}

auto WebGpuDevice::waitIdle() -> void {
    if (!ready()) return;
    QueueRequest request;
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = onQueueDone;
    callback.userdata1 = &request;
    const WGPUFuture future = wgpuQueueOnSubmittedWorkDone(impl_->queue, callback);
    static_cast<void>(waitFor(impl_->instance, future));
}

auto WebGpuDevice::stats() const -> DeviceStats {
    DeviceStats result = impl_->stats;
    result.bufferCount = impl_->buffers.liveCount();
    result.bufferBytes = 0;
    impl_->buffers.forEach([&](WebGpuBuffer* buffer) {
        result.bufferBytes += buffer->rec.sizeBytes;
    });
    result.textureCount = impl_->textures.liveCount();
    result.textureBytes = 0;
    impl_->textures.forEach([&](WebGpuTexture* texture) {
        result.textureBytes += textureBytes(texture->rec);
    });
    result.samplerCount = impl_->samplers.liveCount();
    result.shaderCount = impl_->shaders.liveCount();
    result.pipelineCount = impl_->pipelines.liveCount();
    result.framebufferCount = impl_->framebuffers.liveCount();
    result.swapchainCount = impl_->swapchains.liveCount();
    return result;
}

auto WebGpuDevice::resetFrameStats() -> void {
    impl_->stats.drawCalls = 0;
    impl_->stats.drawIndexedCalls = 0;
    impl_->stats.dispatchCalls = 0;
    impl_->stats.batches = 0;
    impl_->stats.instances = 0;
    impl_->stats.primitives = 0;
    impl_->stats.vertices = 0;
}

auto createWebGpuDevice(const char* canvasSelector) -> IDevice* {
    auto* device = new WebGpuDevice(canvasSelector);
    if (!device->ready()) {
        delete device;
        return nullptr;
    }
    return device;
}

} // namespace f4::gfx
