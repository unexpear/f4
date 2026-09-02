#include "backends/dx12/dx12_device.h"

#if !defined(_WIN32)
#error "The DirectX 12 backend is Windows-only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/logging/logger.h"
#include "graphics/device/object_store.h"
#if defined(F4_DX12_HAS_PLATFORM)
#include "platform/window.h"
#endif

namespace f4::gfx {

namespace {

constexpr std::size_t kFramesInFlight = 2;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    auto operator=(const ComPtr&) -> ComPtr& = delete;
    ComPtr(ComPtr&& other) noexcept : value_(other.detach()) {}
    auto operator=(ComPtr&& other) noexcept -> ComPtr& {
        if (this != &other) {
            reset();
            value_ = other.detach();
        }
        return *this;
    }

    [[nodiscard]] auto get() const -> T* { return value_; }
    [[nodiscard]] auto operator->() const -> T* { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }
    auto put() -> T** {
        reset();
        return &value_;
    }
    auto putVoid() -> void** { return reinterpret_cast<void**>(put()); }
    auto detach() -> T* {
        T* result = value_;
        value_ = nullptr;
        return result;
    }
    void reset(T* replacement = nullptr) {
        if (value_ != nullptr) value_->Release();
        value_ = replacement;
    }

private:
    T* value_ = nullptr;
};

[[nodiscard]] constexpr auto alignUp(std::uint64_t value, std::uint64_t alignment)
    -> std::uint64_t {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

[[nodiscard]] auto toGfxError(HRESULT result) -> GfxError {
    if (SUCCEEDED(result)) return GfxError::None;
    if (result == E_OUTOFMEMORY) return GfxError::OutOfMemory;
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        return GfxError::DeviceLost;
    }
    return GfxError::BackendUnavailable;
}

[[nodiscard]] constexpr auto isDepthFormat(TextureFormat format) -> bool {
    return format == TextureFormat::Depth32F || format == TextureFormat::Depth24Stencil8;
}

[[nodiscard]] constexpr auto dxgiFormat(TextureFormat format) -> DXGI_FORMAT {
    switch (format) {
    case TextureFormat::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case TextureFormat::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case TextureFormat::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case TextureFormat::RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case TextureFormat::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::Depth32F: return DXGI_FORMAT_D32_FLOAT;
    case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] constexpr auto resourceFormat(TextureFormat format) -> DXGI_FORMAT {
    switch (format) {
    case TextureFormat::Depth32F: return DXGI_FORMAT_R32_TYPELESS;
    case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_R24G8_TYPELESS;
    default: return dxgiFormat(format);
    }
}

[[nodiscard]] constexpr auto shaderViewFormat(TextureFormat format) -> DXGI_FORMAT {
    switch (format) {
    case TextureFormat::Depth32F: return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return dxgiFormat(format);
    }
}

[[nodiscard]] constexpr auto dxVertexFormat(VertexFormat format) -> DXGI_FORMAT {
    switch (format) {
    case VertexFormat::Float32x2: return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float32x3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float32x4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::UNorm8x4: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] constexpr auto dxCompare(CompareOp op) -> D3D12_COMPARISON_FUNC {
    switch (op) {
    case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

[[nodiscard]] constexpr auto dxBlend(BlendFactor factor) -> D3D12_BLEND {
    switch (factor) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    }
    return D3D12_BLEND_ONE;
}

[[nodiscard]] constexpr auto dxBlendOp(BlendOp op) -> D3D12_BLEND_OP {
    switch (op) {
    case BlendOp::Add: return D3D12_BLEND_OP_ADD;
    case BlendOp::Sub: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::RevSub: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min: return D3D12_BLEND_OP_MIN;
    case BlendOp::Max: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

[[nodiscard]] constexpr auto dxTopology(PrimitiveTopology topology)
    -> D3D_PRIMITIVE_TOPOLOGY {
    switch (topology) {
    case PrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case PrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

[[nodiscard]] constexpr auto dxTopologyType(PrimitiveTopology topology)
    -> D3D12_PRIMITIVE_TOPOLOGY_TYPE {
    switch (topology) {
    case PrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case PrimitiveTopology::LineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case PrimitiveTopology::TriangleList:
    case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

[[nodiscard]] constexpr auto dxAddress(AddressMode mode) -> D3D12_TEXTURE_ADDRESS_MODE {
    switch (mode) {
    case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

[[nodiscard]] auto dxFilter(const SamplerDesc& desc) -> D3D12_FILTER {
    if (desc.maxAnisotropy > 1.0f) return D3D12_FILTER_ANISOTROPIC;
    const bool minLinear = desc.filter == TextureFilter::MinMagLinear ||
                           desc.filter == TextureFilter::MinLinearMagNearest;
    const bool magLinear = desc.filter == TextureFilter::MinMagLinear ||
                           desc.filter == TextureFilter::MinNearestMagLinear;
    const bool mipLinear = desc.mipFilter == MipFilter::Linear;
    if (minLinear && magLinear && mipLinear) return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    if (minLinear && magLinear) return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (minLinear && mipLinear) return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    if (magLinear && mipLinear) return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    if (minLinear) return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (magLinear) return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    if (mipLinear) return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    return D3D12_FILTER_MIN_MAG_MIP_POINT;
}

[[nodiscard]] constexpr auto dxResourceState(const ResourceState& state,
                                              bool texture) -> D3D12_RESOURCE_STATES {
    if (texture) {
        switch (state.layout) {
        case TextureLayout::General: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case TextureLayout::ShaderRead:
            return static_cast<D3D12_RESOURCE_STATES>(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        case TextureLayout::ColorAttachment: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case TextureLayout::DepthStencilAttachment: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case TextureLayout::TransferSource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case TextureLayout::TransferDestination: return D3D12_RESOURCE_STATE_COPY_DEST;
        case TextureLayout::Present: return D3D12_RESOURCE_STATE_PRESENT;
        case TextureLayout::Undefined: return D3D12_RESOURCE_STATE_COMMON;
        }
    }
    const std::uint32_t access = state.access;
    if ((access & ResourceAccessMask::TransferWrite) != 0) return D3D12_RESOURCE_STATE_COPY_DEST;
    if ((access & ResourceAccessMask::TransferRead) != 0) return D3D12_RESOURCE_STATE_COPY_SOURCE;
    if ((access & ResourceAccessMask::ShaderWrite) != 0) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
    if ((access & ResourceAccessMask::VertexRead) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((access & ResourceAccessMask::IndexRead) != 0) result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if ((access & ResourceAccessMask::UniformRead) != 0) result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if ((access & ResourceAccessMask::ShaderRead) != 0) {
        result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    return result;
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

struct DxBufferRecord {
    GpuBuffer rec;
    ComPtr<ID3D12Resource> resource;
    std::uint64_t allocationBytes = 0;
    bool upload = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct DxTextureRecord {
    GpuTexture rec;
    ComPtr<ID3D12Resource> resource;
    std::vector<D3D12_RESOURCE_STATES> states;
};

struct DxSamplerRecord { GpuSampler rec; };

struct DxShaderRecord {
    GpuShader rec;
    std::vector<std::byte> blob;
};

struct RootTable {
    std::uint32_t rootIndex = 0;
    std::uint32_t bindPoint = 0;
    bool samplers = false;
    std::vector<BindingSlot> slots;
};

struct DxPipelineRecord {
    GpuPipeline rec;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<RootTable> tables;
    std::vector<DXGI_FORMAT> colorFormats;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
};

struct DxFramebufferRecord {
    GpuFramebuffer rec;
    std::vector<DXGI_FORMAT> colorFormats;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    SwapchainHandle swapchain{};
};

struct DxSwapchainRecord {
    GpuSwapchain rec;
    ComPtr<IDXGISwapChain3> swapchain;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<D3D12_RESOURCE_STATES> states;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool allowTearing = false;
};

struct DxFrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12DescriptorHeap> resources;
    ComPtr<ID3D12DescriptorHeap> samplers;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12DescriptorHeap> dsvs;
    std::uint64_t fenceValue = 0;
};

struct RetiredObject {
    IUnknown* object = nullptr;
    std::uint64_t afterFence = 0;
};

} // namespace

struct Dx12Device::Impl {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> infoQueue;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    std::array<DxFrameSlot, kFramesInFlight> frames;
    std::size_t frameIndex = 0;
    std::uint64_t lastSubmittedFence = 0;
    std::uint64_t completedFence = 0;
    HANDLE fenceEvent = nullptr;
    HWND window = nullptr;
    char deviceName[128]{};
    std::uint32_t featureLevel = 0;
    bool debugLayer = false;
    bool initialized = false;
    DeviceStats stats;
    ObjectStore<DxBufferRecord, GpuBufferTag> buffers;
    ObjectStore<DxTextureRecord, GpuTextureTag> textures;
    ObjectStore<DxSamplerRecord, GpuSamplerTag> samplers;
    ObjectStore<DxShaderRecord, GpuShaderTag> shaders;
    ObjectStore<DxPipelineRecord, GpuPipelineTag> pipelines;
    ObjectStore<DxFramebufferRecord, GpuFramebufferTag> framebuffers;
    ObjectStore<DxSwapchainRecord, GpuSwapchainTag> swapchains;
    SwapchainHandle activeSwapchain{};
    std::vector<RetiredObject> retired;

    auto waitFor(std::uint64_t value) -> HRESULT {
        if (value == 0 || fence->GetCompletedValue() >= value) {
            completedFence = std::max(completedFence, value);
            return S_OK;
        }
        HRESULT result = fence->SetEventOnCompletion(value, fenceEvent);
        if (FAILED(result)) return result;
        if (WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0) return E_FAIL;
        completedFence = std::max(completedFence, value);
        return S_OK;
    }

    auto executeImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record)
        -> HRESULT {
        ComPtr<ID3D12CommandAllocator> allocator;
        HRESULT result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
            allocator.putVoid());
        if (FAILED(result)) return result;
        ComPtr<ID3D12GraphicsCommandList> list;
        result = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
            IID_ID3D12GraphicsCommandList, list.putVoid());
        if (FAILED(result)) return result;
        record(list.get());
        result = list->Close();
        if (FAILED(result)) return result;
        ID3D12CommandList* lists[] = {list.get()};
        queue->ExecuteCommandLists(1, lists);
        const std::uint64_t signal = ++lastSubmittedFence;
        result = queue->Signal(fence.get(), signal);
        if (FAILED(result)) return result;
        return waitFor(signal);
    }
};

namespace {

[[nodiscard]] auto createHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                              std::uint32_t count, bool shaderVisible,
                              ComPtr<ID3D12DescriptorHeap>& output) -> HRESULT {
    if (count == 0) {
        output.reset();
        return S_OK;
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    return device->CreateDescriptorHeap(
        &desc, IID_ID3D12DescriptorHeap, output.putVoid());
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES target,
                std::uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    if (current == target) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = current;
    barrier.Transition.StateAfter = target;
    commands->ResourceBarrier(1, &barrier);
    current = target;
}

void transitionTextureMip(ID3D12GraphicsCommandList* commands, DxTextureRecord& texture,
                          std::uint32_t mipLevel, D3D12_RESOURCE_STATES target) {
    D3D12_RESOURCE_STATES& current = texture.states[mipLevel];
    if (current == target) return;
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
    const UINT planeCount = texture.rec.format == TextureFormat::Depth24Stencil8 ? 2u : 1u;
    for (UINT plane = 0; plane < planeCount; ++plane) {
        D3D12_RESOURCE_BARRIER& barrier = barriers[plane];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.get();
        barrier.Transition.Subresource = mipLevel + plane * texture.rec.mipLevels;
        barrier.Transition.StateBefore = current;
        barrier.Transition.StateAfter = target;
    }
    commands->ResourceBarrier(planeCount, barriers.data());
    current = target;
}

[[nodiscard]] auto heapProperties(D3D12_HEAP_TYPE type) -> D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] auto bufferResourceDesc(std::uint64_t size) -> D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

template <typename ImplType>
void retainForRetirement(ImplType& impl, IUnknown* object) {
    if (object == nullptr) return;
    object->AddRef();
    impl.retired.push_back({object, impl.lastSubmittedFence});
}

template <typename ImplType>
[[nodiscard]] auto countMessages(const ImplType& impl,
                                 D3D12_MESSAGE_SEVERITY severity) -> std::uint64_t {
    if (!impl.infoQueue) return 0;
    std::uint64_t result = 0;
    const std::uint64_t count = impl.infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (std::uint64_t i = 0; i < count; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(impl.infoQueue->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(impl.infoQueue->GetMessage(i, message, &bytes)) &&
            message->Severity == severity) {
            if (severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                F4_LOG_ERROR() << "DX12 validation: " << message->pDescription;
            }
            ++result;
        }
    }
    return result;
}

} // namespace

Dx12Device::Dx12Device(platform::Window* window) : impl_(new Impl()) {
    if (!initialize(window)) impl_->initialized = false;
}

Dx12Device::~Dx12Device() {
    if (impl_ == nullptr) return;
    waitIdle();
    for (RetiredObject& retired : impl_->retired) {
        if (retired.object != nullptr) retired.object->Release();
    }
    impl_->retired.clear();
    if (impl_->fenceEvent != nullptr) CloseHandle(impl_->fenceEvent);
    delete impl_;
}

auto Dx12Device::initialize(platform::Window* window) -> bool {
#if defined(F4_DX12_HAS_PLATFORM)
    if (window != nullptr) impl_->window = static_cast<HWND>(window->nativeHandle());
#else
    (void)window;
#endif
    if (window != nullptr && impl_->window == nullptr) return false;

    UINT factoryFlags = 0;
#if !defined(NDEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_ID3D12Debug, debug.putVoid()))) {
        debug->EnableDebugLayer();
        impl_->debugLayer = true;
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    HRESULT result = CreateDXGIFactory2(
        factoryFlags, IID_IDXGIFactory4, impl_->factory.putVoid());
    if (FAILED(result)) return false;

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        result = impl_->factory->EnumAdapters1(index, candidate.put());
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(candidate.get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_ID3D12Device, nullptr))) {
            impl_->adapter = std::move(candidate);
            break;
        }
    }
    if (!impl_->adapter) {
        result = impl_->factory->EnumWarpAdapter(
            IID_IDXGIAdapter1, impl_->adapter.putVoid());
        if (FAILED(result)) return false;
    }

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    for (D3D_FEATURE_LEVEL level : levels) {
        result = D3D12CreateDevice(
            impl_->adapter.get(), level, IID_ID3D12Device, impl_->device.putVoid());
        if (SUCCEEDED(result)) {
            impl_->featureLevel = static_cast<std::uint32_t>(level);
            break;
        }
    }
    if (!impl_->device) return false;

    DXGI_ADAPTER_DESC1 adapterDesc{};
    if (SUCCEEDED(impl_->adapter->GetDesc1(&adapterDesc))) {
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1,
                            impl_->deviceName, static_cast<int>(sizeof(impl_->deviceName)),
                            nullptr, nullptr);
    }

    if (impl_->debugLayer) {
        (void)impl_->device->QueryInterface(
            IID_ID3D12InfoQueue, impl_->infoQueue.putVoid());
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = impl_->device->CreateCommandQueue(
        &queueDesc, IID_ID3D12CommandQueue, impl_->queue.putVoid());
    if (FAILED(result)) return false;
    for (DxFrameSlot& frame : impl_->frames) {
        result = impl_->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
            frame.allocator.putVoid());
        if (FAILED(result)) return false;
    }
    result = impl_->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->frames[0].allocator.get(), nullptr,
        IID_ID3D12GraphicsCommandList, impl_->commands.putVoid());
    if (FAILED(result) || FAILED(impl_->commands->Close())) return false;
    result = impl_->device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, impl_->fence.putVoid());
    if (FAILED(result)) return false;
    impl_->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->fenceEvent == nullptr) return false;
    impl_->initialized = true;
    return true;
}

auto Dx12Device::ready() const -> bool {
    return impl_ != nullptr && impl_->initialized;
}

auto Dx12Device::backend() const -> Backend { return Backend::DirectX12; }

void Dx12Device::collectRetired() {
    if (!ready()) return;
    impl_->completedFence = std::max(impl_->completedFence, impl_->fence->GetCompletedValue());
    auto end = std::remove_if(impl_->retired.begin(), impl_->retired.end(), [&](RetiredObject& item) {
        if (item.afterFence > impl_->completedFence) return false;
        if (item.object != nullptr) item.object->Release();
        return true;
    });
    impl_->retired.erase(end, impl_->retired.end());
}

auto Dx12Device::createBuffer(const BufferDesc& desc,
                              std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    if (!ready() || desc.sizeBytes == 0 || initialData.size() > desc.sizeBytes) {
        return GfxError::BadDescriptor;
    }
    DxBufferRecord record;
    record.rec = {desc.sizeBytes, desc.usage};
    record.upload = !hasUsage(desc.usage, BufferUsage::Storage);
    record.allocationBytes = hasUsage(desc.usage, BufferUsage::Uniform)
                                 ? alignUp(desc.sizeBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                                 : desc.sizeBytes;
    D3D12_RESOURCE_DESC resourceDesc = bufferResourceDesc(record.allocationBytes);
    if (!record.upload) resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(
        record.upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
    record.state = record.upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                                 : D3D12_RESOURCE_STATE_COMMON;
    HRESULT result = impl_->device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resourceDesc, record.state, nullptr,
        IID_ID3D12Resource, record.resource.putVoid());
    if (FAILED(result)) return toGfxError(result);

    if (!initialData.empty()) {
        if (record.upload) {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, 0};
            result = record.resource->Map(0, &readRange, &mapped);
            if (FAILED(result)) return toGfxError(result);
            std::memcpy(mapped, initialData.data(), initialData.size());
            record.resource->Unmap(0, nullptr);
        } else {
            ComPtr<ID3D12Resource> upload;
            const D3D12_HEAP_PROPERTIES uploadProperties = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RESOURCE_DESC uploadDesc = bufferResourceDesc(initialData.size());
            result = impl_->device->CreateCommittedResource(
                &uploadProperties, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_ID3D12Resource, upload.putVoid());
            if (FAILED(result)) return toGfxError(result);
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, 0};
            result = upload->Map(0, &readRange, &mapped);
            if (FAILED(result)) return toGfxError(result);
            std::memcpy(mapped, initialData.data(), initialData.size());
            upload->Unmap(0, nullptr);
            result = impl_->executeImmediate([&](ID3D12GraphicsCommandList* list) {
                transition(list, record.resource.get(), record.state,
                           D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(record.resource.get(), 0, upload.get(), 0,
                                       initialData.size());
                transition(list, record.resource.get(), record.state,
                           static_cast<D3D12_RESOURCE_STATES>(
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            });
            if (FAILED(result)) return toGfxError(result);
        }
    }
    const BufferHandle handle = impl_->buffers.create(std::move(record));
    ++impl_->stats.bufferCount;
    impl_->stats.bufferBytes += desc.sizeBytes;
    return handle;
}

auto Dx12Device::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (!ready() || desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height) ||
        dxgiFormat(desc.format) == DXGI_FORMAT_UNKNOWN) {
        return GfxError::BadDescriptor;
    }
    DxTextureRecord record;
    record.rec = {desc.format, desc.width, desc.height, desc.mipLevels};
    record.states.resize(desc.mipLevels, D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resourceDesc.Format = resourceFormat(desc.format);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = isDepthFormat(desc.format)
                             ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                             : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const HRESULT result = impl_->device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource,
        record.resource.putVoid());
    if (FAILED(result)) return toGfxError(result);
    const TextureHandle handle = impl_->textures.create(std::move(record));
    ++impl_->stats.textureCount;
    impl_->stats.textureBytes += textureBytes({desc.format, desc.width, desc.height,
                                               desc.mipLevels});
    return handle;
}

auto Dx12Device::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    if (!ready() || !std::isfinite(desc.maxAnisotropy) || desc.maxAnisotropy < 1.0f ||
        desc.maxAnisotropy > static_cast<float>(D3D12_REQ_MAXANISOTROPY)) {
        return GfxError::BadDescriptor;
    }
    DxSamplerRecord record;
    record.rec = {desc.filter, desc.mipFilter, desc.addressU, desc.addressV,
                  desc.addressW, desc.maxAnisotropy};
    const SamplerHandle handle = impl_->samplers.create(std::move(record));
    ++impl_->stats.samplerCount;
    return handle;
}

auto Dx12Device::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    if (!ready() || desc.blob.size() < 4 ||
        std::memcmp(desc.blob.data(), "DXBC", 4) != 0 ||
        (desc.stage != ShaderStage::Vertex && desc.stage != ShaderStage::Fragment)) {
        if (ready()) ++impl_->stats.shaderFailed;
        return GfxError::BadDescriptor;
    }
    DxShaderRecord record;
    record.rec.stage = desc.stage;
    record.blob.assign(desc.blob.begin(), desc.blob.end());
    const ShaderHandle handle = impl_->shaders.create(std::move(record));
    ++impl_->stats.shaderCount;
    ++impl_->stats.shaderCompiled;
    return handle;
}

auto Dx12Device::createPipeline(const PipelineStateDesc& desc)
    -> Result<PipelineHandle, GfxError> {
    if (!ready() || !desc.vertexShader.isValid() || !desc.fragmentShader.isValid() ||
        desc.layout.pushConstantSizeBytes != 0 || desc.colorAttachments.size() > 8) {
        return GfxError::BadDescriptor;
    }
    DxShaderRecord* vertex = impl_->shaders.get(desc.vertexShader);
    DxShaderRecord* fragment = impl_->shaders.get(desc.fragmentShader);
    if (vertex == nullptr || fragment == nullptr) return GfxError::InvalidHandle;
    if (vertex->rec.stage != ShaderStage::Vertex ||
        fragment->rec.stage != ShaderStage::Fragment) {
        return GfxError::BadDescriptor;
    }

    DxPipelineRecord record;
    record.rec.desc = desc;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> rangeStorage;
    rangeStorage.reserve(desc.layout.groups.size() * 2u);
    std::vector<D3D12_ROOT_PARAMETER> parameters;
    parameters.reserve(desc.layout.groups.size() * 2u);
    for (std::size_t groupIndex = 0; groupIndex < desc.layout.groups.size(); ++groupIndex) {
        const BindGroup& group = desc.layout.groups[groupIndex];
        for (std::size_t previous = 0; previous < groupIndex; ++previous) {
            if (desc.layout.groups[previous].bindPoint == group.bindPoint) {
                return GfxError::BadDescriptor;
            }
        }
        for (bool samplerTable : {false, true}) {
            rangeStorage.emplace_back();
            RootTable table;
            table.rootIndex = static_cast<std::uint32_t>(parameters.size());
            table.bindPoint = group.bindPoint;
            table.samplers = samplerTable;
            for (std::size_t slotIndex = 0; slotIndex < group.slots.size(); ++slotIndex) {
                const BindingSlot& slot = group.slots[slotIndex];
                if (slot.count != 1 || slot.slot > D3D12_COMMONSHADER_INPUT_RESOURCE_REGISTER_COUNT) {
                    return GfxError::BadDescriptor;
                }
                for (std::size_t previous = 0; previous < slotIndex; ++previous) {
                    if (group.slots[previous].slot == slot.slot &&
                        group.slots[previous].type == slot.type) {
                        return GfxError::BadDescriptor;
                    }
                }
                if ((slot.type == BindingType::Sampler) != samplerTable) continue;
                D3D12_DESCRIPTOR_RANGE range{};
                if (slot.type == BindingType::Sampler) {
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                } else if (slot.type == BindingType::UniformBuffer) {
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                } else {
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                }
                range.NumDescriptors = 1;
                range.BaseShaderRegister = slot.slot;
                range.RegisterSpace = group.bindPoint;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                rangeStorage.back().push_back(range);
                table.slots.push_back(slot);
            }
            if (rangeStorage.back().empty()) {
                rangeStorage.pop_back();
                continue;
            }
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges =
                static_cast<UINT>(rangeStorage.back().size());
            parameter.DescriptorTable.pDescriptorRanges = rangeStorage.back().data();
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters.push_back(parameter);
            record.tables.push_back(std::move(table));
        }
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(parameters.size());
    rootDesc.pParameters = parameters.data();
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put());
    if (FAILED(result)) {
        if (errors) {
            F4_LOG_ERROR() << "DX12 root signature: "
                           << static_cast<const char*>(errors->GetBufferPointer());
        }
        return GfxError::BadDescriptor;
    }
    result = impl_->device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_ID3D12RootSignature, record.rootSignature.putVoid());
    if (FAILED(result)) return toGfxError(result);

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    inputElements.reserve(desc.vertexInput.attributes.size());
    for (const auto& attribute : desc.vertexInput.attributes) {
        const auto binding = std::find_if(
            desc.vertexInput.bindings.begin(), desc.vertexInput.bindings.end(),
            [&](const auto& item) { return item.binding == attribute.binding; });
        if (binding == desc.vertexInput.bindings.end() || binding->strideBytes == 0 ||
            attribute.offsetBytes + vertexFormatSize(attribute.format) > binding->strideBytes) {
            return GfxError::BadDescriptor;
        }
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = attribute.location;
        element.Format = dxVertexFormat(attribute.format);
        element.InputSlot = attribute.binding;
        element.AlignedByteOffset = attribute.offsetBytes;
        element.InputSlotClass = binding->divisor == 0
                                     ? D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
                                     : D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        element.InstanceDataStepRate = binding->divisor;
        inputElements.push_back(element);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = record.rootSignature.get();
    pipelineDesc.VS = {vertex->blob.data(), vertex->blob.size()};
    pipelineDesc.PS = {fragment->blob.data(), fragment->blob.size()};
    pipelineDesc.BlendState.AlphaToCoverageEnable = FALSE;
    pipelineDesc.BlendState.IndependentBlendEnable = TRUE;
    const std::size_t colorCount = desc.colorAttachments.isEmpty()
                                       ? 1u : desc.colorAttachments.size();
    for (std::size_t i = 0; i < colorCount; ++i) {
        const ColorAttachmentState state = desc.colorAttachments.isEmpty()
                                               ? ColorAttachmentState{}
                                               : desc.colorAttachments[i];
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pipelineDesc.BlendState.RenderTarget[i];
        blend.BlendEnable = state.blendEnable;
        blend.LogicOpEnable = FALSE;
        blend.SrcBlend = dxBlend(state.srcColor);
        blend.DestBlend = dxBlend(state.dstColor);
        blend.BlendOp = dxBlendOp(state.colorOp);
        blend.SrcBlendAlpha = dxBlend(state.srcAlpha);
        blend.DestBlendAlpha = dxBlend(state.dstAlpha);
        blend.BlendOpAlpha = dxBlendOp(state.alphaOp);
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = static_cast<UINT8>(state.writeMask & 0xfu);
        pipelineDesc.RTVFormats[i] = dxgiFormat(state.format);
        record.colorFormats.push_back(pipelineDesc.RTVFormats[i]);
    }
    pipelineDesc.SampleMask = std::numeric_limits<UINT>::max();
    pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDesc.RasterizerState.CullMode = desc.raster.cullMode == CullMode::None
                                               ? D3D12_CULL_MODE_NONE
                                               : (desc.raster.cullMode == CullMode::Front
                                                      ? D3D12_CULL_MODE_FRONT
                                                      : D3D12_CULL_MODE_BACK);
    pipelineDesc.RasterizerState.FrontCounterClockwise = TRUE;
    pipelineDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDesc.RasterizerState.DepthClipEnable = !desc.raster.depthClampEnable;
    pipelineDesc.RasterizerState.MultisampleEnable = FALSE;
    pipelineDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    pipelineDesc.RasterizerState.ForcedSampleCount = 0;
    pipelineDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pipelineDesc.DepthStencilState.DepthEnable = desc.depthStencil.depthTestEnable;
    pipelineDesc.DepthStencilState.DepthWriteMask = desc.depthStencil.depthWriteEnable
                                                        ? D3D12_DEPTH_WRITE_MASK_ALL
                                                        : D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.DepthStencilState.DepthFunc = dxCompare(desc.depthStencil.depthCompare);
    pipelineDesc.DepthStencilState.StencilEnable = FALSE;
    pipelineDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pipelineDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    if (desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable) {
        pipelineDesc.DSVFormat = dxgiFormat(desc.depthStencil.format);
        record.depthFormat = pipelineDesc.DSVFormat;
    }
    pipelineDesc.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};
    pipelineDesc.PrimitiveTopologyType = dxTopologyType(desc.topology);
    pipelineDesc.NumRenderTargets = static_cast<UINT>(colorCount);
    pipelineDesc.SampleDesc.Count = 1;
    result = impl_->device->CreateGraphicsPipelineState(
        &pipelineDesc, IID_ID3D12PipelineState, record.pipeline.putVoid());
    if (FAILED(result)) {
        F4_LOG_ERROR() << "DX12 CreateGraphicsPipelineState failed: "
                       << static_cast<long>(result);
        return GfxError::BadDescriptor;
    }
    const PipelineHandle handle = impl_->pipelines.create(std::move(record));
    ++impl_->stats.pipelineCount;
    return handle;
}

auto Dx12Device::createSwapchain(const SwapchainDesc& desc)
    -> Result<SwapchainHandle, GfxError> {
    if (!ready() || impl_->window == nullptr || impl_->activeSwapchain.isValid() ||
        desc.extent.width == 0 || desc.extent.height == 0 ||
        isDepthFormat(desc.surfaceFormat)) {
        return GfxError::BadDescriptor;
    }
    DxSwapchainRecord record;
    record.rec = {desc.surfaceFormat, desc.extent, desc.presentMode};
    record.format = dxgiFormat(desc.surfaceFormat);
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = desc.extent.width;
    swapDesc.Height = desc.extent.height;
    swapDesc.Format = record.format;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = static_cast<UINT>(kFramesInFlight);
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> baseSwapchain;
    HRESULT result = impl_->factory->CreateSwapChainForHwnd(
        impl_->queue.get(), impl_->window, &swapDesc, nullptr, nullptr,
        baseSwapchain.put());
    if (FAILED(result)) return toGfxError(result);
    (void)impl_->factory->MakeWindowAssociation(impl_->window, DXGI_MWA_NO_ALT_ENTER);
    result = baseSwapchain->QueryInterface(
        IID_IDXGISwapChain3, record.swapchain.putVoid());
    if (FAILED(result)) return toGfxError(result);
    record.buffers.resize(kFramesInFlight);
    record.states.resize(kFramesInFlight, D3D12_RESOURCE_STATE_PRESENT);
    for (UINT index = 0; index < static_cast<UINT>(kFramesInFlight); ++index) {
        result = record.swapchain->GetBuffer(
            index, IID_ID3D12Resource, record.buffers[index].putVoid());
        if (FAILED(result)) return toGfxError(result);
    }
    const SwapchainHandle handle = impl_->swapchains.create(std::move(record));
    impl_->activeSwapchain = handle;
    ++impl_->stats.swapchainCount;
    return handle;
}

auto Dx12Device::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (!ready() || desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.attachments.isEmpty()) {
        return GfxError::BadDescriptor;
    }
    DxFramebufferRecord record;
    std::size_t colorCount = 0;
    bool hasDepth = false;
    for (const AttachmentDesc& attachment : desc.attachments) {
        if (!attachment.texture.isValid()) {
            if (attachment.type != AttachmentType::Color || record.swapchain.isValid()) {
                return GfxError::BadDescriptor;
            }
            DxSwapchainRecord* swapchain = impl_->swapchains.get(impl_->activeSwapchain);
            if (swapchain == nullptr || swapchain->rec.extent != desc.extent) {
                return GfxError::BadDescriptor;
            }
            record.swapchain = impl_->activeSwapchain;
            record.colorFormats.push_back(swapchain->format);
            ++colorCount;
            continue;
        }
        DxTextureRecord* texture = impl_->textures.get(attachment.texture);
        if (texture == nullptr) return GfxError::InvalidHandle;
        if (texture->rec.width != desc.extent.width || texture->rec.height != desc.extent.height) {
            return GfxError::BadDescriptor;
        }
        if (attachment.type == AttachmentType::Depth) {
            if (hasDepth || !isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
            hasDepth = true;
            record.depthFormat = dxgiFormat(texture->rec.format);
        } else {
            if (isDepthFormat(texture->rec.format) || colorCount >= 8) {
                return GfxError::BadDescriptor;
            }
            record.colorFormats.push_back(dxgiFormat(texture->rec.format));
            ++colorCount;
        }
    }
    if (colorCount == 0) return GfxError::BadDescriptor;
    record.rec.extent = desc.extent;
    record.rec.attachments = desc.attachments;
    const FramebufferHandle handle = impl_->framebuffers.create(std::move(record));
    ++impl_->stats.framebufferCount;
    return handle;
}

auto Dx12Device::destroyBuffer(BufferHandle handle) -> GfxError {
    DxBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    const std::uint64_t bytes = record->rec.sizeBytes;
    retainForRetirement(*impl_, record->resource.get());
    impl_->buffers.destroy(handle);
    --impl_->stats.bufferCount;
    impl_->stats.bufferBytes -= bytes;
    collectRetired();
    return GfxError::None;
}

auto Dx12Device::destroyTexture(TextureHandle handle) -> GfxError {
    DxTextureRecord* record = impl_->textures.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    const std::uint64_t bytes = textureBytes(record->rec);
    retainForRetirement(*impl_, record->resource.get());
    impl_->textures.destroy(handle);
    --impl_->stats.textureCount;
    impl_->stats.textureBytes -= bytes;
    collectRetired();
    return GfxError::None;
}

auto Dx12Device::destroySampler(SamplerHandle handle) -> GfxError {
    if (!impl_->samplers.destroy(handle)) return GfxError::StaleHandle;
    --impl_->stats.samplerCount;
    return GfxError::None;
}

auto Dx12Device::destroyShader(ShaderHandle handle) -> GfxError {
    if (!impl_->shaders.destroy(handle)) return GfxError::StaleHandle;
    --impl_->stats.shaderCount;
    return GfxError::None;
}

auto Dx12Device::destroyPipeline(PipelineHandle handle) -> GfxError {
    DxPipelineRecord* record = impl_->pipelines.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    retainForRetirement(*impl_, record->pipeline.get());
    retainForRetirement(*impl_, record->rootSignature.get());
    impl_->pipelines.destroy(handle);
    --impl_->stats.pipelineCount;
    collectRetired();
    return GfxError::None;
}

auto Dx12Device::destroySwapchain(SwapchainHandle handle) -> GfxError {
    DxSwapchainRecord* record = impl_->swapchains.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    waitIdle();
    impl_->swapchains.destroy(handle);
    if (impl_->activeSwapchain == handle) impl_->activeSwapchain = {};
    --impl_->stats.swapchainCount;
    return GfxError::None;
}

auto Dx12Device::destroyFramebuffer(FramebufferHandle handle) -> GfxError {
    if (!impl_->framebuffers.destroy(handle)) return GfxError::StaleHandle;
    --impl_->stats.framebufferCount;
    return GfxError::None;
}

auto Dx12Device::writeBuffer(BufferHandle handle, std::uint64_t offset,
                             std::span<const std::byte> data) -> GfxError {
    DxBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (offset > record->rec.sizeBytes || data.size() > record->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    if (data.empty()) return GfxError::None;
    if (record->upload) {
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, 0};
        const HRESULT result = record->resource->Map(0, &readRange, &mapped);
        if (FAILED(result)) return toGfxError(result);
        std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());
        const D3D12_RANGE writtenRange{static_cast<SIZE_T>(offset),
                                       static_cast<SIZE_T>(offset + data.size())};
        record->resource->Unmap(0, &writtenRange);
        return GfxError::None;
    }

    ComPtr<ID3D12Resource> upload;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc = bufferResourceDesc(data.size());
    HRESULT result = impl_->device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource,
        upload.putVoid());
    if (FAILED(result)) return toGfxError(result);
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    result = upload->Map(0, &readRange, &mapped);
    if (FAILED(result)) return toGfxError(result);
    std::memcpy(mapped, data.data(), data.size());
    upload->Unmap(0, nullptr);
    result = impl_->executeImmediate([&](ID3D12GraphicsCommandList* list) {
        transition(list, record->resource.get(), record->state,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(record->resource.get(), offset, upload.get(), 0, data.size());
        transition(list, record->resource.get(), record->state,
                   static_cast<D3D12_RESOURCE_STATES>(
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    });
    return toGfxError(result);
}

auto Dx12Device::writeTexture(TextureHandle handle, std::uint32_t mipLevel,
                              std::span<const std::byte> data) -> GfxError {
    DxTextureRecord* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels) return GfxError::BadDescriptor;
    const std::uint32_t width = std::max(1u, texture->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, texture->rec.height >> mipLevel);
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) *
                                   textureFormatBytes(texture->rec.format);
    const std::uint64_t expected = rowBytes * height;
    if (data.size() != expected) return GfxError::BadDescriptor;

    const D3D12_RESOURCE_DESC textureDesc = texture->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 tightRowBytes = 0;
    UINT64 totalBytes = 0;
    impl_->device->GetCopyableFootprints(
        &textureDesc, mipLevel, 1, 0, &footprint, &rowCount, &tightRowBytes, &totalBytes);
    if (rowCount != height || tightRowBytes != rowBytes) return GfxError::BadDescriptor;
    ComPtr<ID3D12Resource> upload;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc = bufferResourceDesc(totalBytes);
    HRESULT result = impl_->device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource,
        upload.putVoid());
    if (FAILED(result)) return toGfxError(result);
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    result = upload->Map(0, &readRange, &mapped);
    if (FAILED(result)) return toGfxError(result);
    auto* destination = static_cast<std::byte*>(mapped) + footprint.Offset;
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                    data.data() + static_cast<std::size_t>(row) * rowBytes,
                    static_cast<std::size_t>(rowBytes));
    }
    upload->Unmap(0, nullptr);
    result = impl_->executeImmediate([&](ID3D12GraphicsCommandList* list) {
        transitionTextureMip(list, *texture, mipLevel, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = texture->resource.get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = mipLevel;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = upload.get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        transitionTextureMip(
            list, *texture, mipLevel,
            static_cast<D3D12_RESOURCE_STATES>(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    });
    return toGfxError(result);
}

auto Dx12Device::submit(CommandBuffer& commandBuffer) -> GfxError {
    if (!ready() || commandBuffer.isRecording()) return GfxError::BadDescriptor;
    DxFrameSlot& frame = impl_->frames[impl_->frameIndex];
    HRESULT result = impl_->waitFor(frame.fenceValue);
    if (FAILED(result)) return toGfxError(result);
    collectRetired();

    std::uint64_t resourceDescriptorCount = 0;
    std::uint64_t samplerDescriptorCount = 0;
    std::uint64_t rtvCount = 0;
    std::uint64_t dsvCount = 0;
    DxPipelineRecord* scanPipeline = nullptr;
    DxSwapchainRecord* submissionSwapchain = nullptr;
    for (const Command& command : commandBuffer.commands()) {
        if (command.kind == CommandKind::BindPipeline) {
            scanPipeline = impl_->pipelines.get(command.pipeline);
            if (scanPipeline == nullptr) return GfxError::InvalidHandle;
        } else if (command.kind == CommandKind::Draw ||
                   command.kind == CommandKind::DrawIndexed) {
            if (scanPipeline != nullptr) {
                for (const RootTable& table : scanPipeline->tables) {
                    if (table.samplers) samplerDescriptorCount += table.slots.size();
                    else resourceDescriptorCount += table.slots.size();
                }
            }
        } else if (command.kind == CommandKind::BeginRenderPass) {
            DxFramebufferRecord* framebuffer = impl_->framebuffers.get(command.framebuffer);
            if (framebuffer == nullptr) return GfxError::InvalidHandle;
            for (const AttachmentDesc& attachment : framebuffer->rec.attachments) {
                if (attachment.type == AttachmentType::Color) ++rtvCount;
                else ++dsvCount;
            }
            if (framebuffer->swapchain.isValid()) {
                DxSwapchainRecord* swapchain = impl_->swapchains.get(framebuffer->swapchain);
                if (swapchain == nullptr) return GfxError::InvalidHandle;
                if (submissionSwapchain != nullptr && submissionSwapchain != swapchain) {
                    return GfxError::BadDescriptor;
                }
                submissionSwapchain = swapchain;
            }
        }
    }
    constexpr std::uint64_t maxDescriptors = std::numeric_limits<UINT>::max();
    if (resourceDescriptorCount > maxDescriptors || samplerDescriptorCount > maxDescriptors ||
        rtvCount > maxDescriptors || dsvCount > maxDescriptors) {
        return GfxError::OutOfMemory;
    }
    result = createHeap(impl_->device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                        static_cast<UINT>(resourceDescriptorCount), true, frame.resources);
    if (SUCCEEDED(result)) {
        result = createHeap(impl_->device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            static_cast<UINT>(samplerDescriptorCount), true, frame.samplers);
    }
    if (SUCCEEDED(result)) {
        result = createHeap(impl_->device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                            static_cast<UINT>(rtvCount), false, frame.rtvs);
    }
    if (SUCCEEDED(result)) {
        result = createHeap(impl_->device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                            static_cast<UINT>(dsvCount), false, frame.dsvs);
    }
    if (FAILED(result)) return toGfxError(result);

    std::vector<std::pair<DxBufferRecord*, D3D12_RESOURCE_STATES>> bufferStates;
    std::vector<std::pair<DxTextureRecord*, std::vector<D3D12_RESOURCE_STATES>>> textureStates;
    std::vector<std::pair<DxSwapchainRecord*, std::vector<D3D12_RESOURCE_STATES>>> swapchainStates;
    impl_->buffers.forEach([&](DxBufferRecord* record) {
        bufferStates.emplace_back(record, record->state);
    });
    impl_->textures.forEach([&](DxTextureRecord* record) {
        textureStates.emplace_back(record, record->states);
    });
    impl_->swapchains.forEach([&](DxSwapchainRecord* record) {
        swapchainStates.emplace_back(record, record->states);
    });
    auto rollbackStates = [&]() {
        for (auto& [record, state] : bufferStates) record->state = state;
        for (auto& [record, states] : textureStates) record->states = std::move(states);
        for (auto& [record, states] : swapchainStates) record->states = std::move(states);
    };

    result = frame.allocator->Reset();
    if (SUCCEEDED(result)) result = impl_->commands->Reset(frame.allocator.get(), nullptr);
    if (FAILED(result)) return toGfxError(result);
    ID3D12GraphicsCommandList* list = impl_->commands.get();
    std::array<ID3D12DescriptorHeap*, 2> shaderHeaps{};
    UINT shaderHeapCount = 0;
    if (frame.resources) shaderHeaps[shaderHeapCount++] = frame.resources.get();
    if (frame.samplers) shaderHeaps[shaderHeapCount++] = frame.samplers.get();
    if (shaderHeapCount != 0) list->SetDescriptorHeaps(shaderHeapCount, shaderHeaps.data());

    const UINT resourceIncrement = impl_->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT samplerIncrement = impl_->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const UINT rtvIncrement = impl_->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT dsvIncrement = impl_->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    UINT resourceOffset = 0;
    UINT samplerOffset = 0;
    UINT rtvOffset = 0;
    UINT dsvOffset = 0;

    bool rendering = false;
    bool indexBound = false;
    bool usedSwapchain = false;
    DxPipelineRecord* boundPipeline = nullptr;
    DxFramebufferRecord* activeFramebuffer = nullptr;
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
    auto bindTables = [&](DxPipelineRecord& pipeline) -> GfxError {
        for (const RootTable& table : pipeline.tables) {
            const UINT tableOffset = table.samplers ? samplerOffset : resourceOffset;
            ID3D12DescriptorHeap* heap = table.samplers
                                              ? frame.samplers.get() : frame.resources.get();
            if (heap == nullptr) return GfxError::BackendUnavailable;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
            const UINT increment = table.samplers ? samplerIncrement : resourceIncrement;
            cpu.ptr += static_cast<SIZE_T>(tableOffset) * increment;
            gpu.ptr += static_cast<UINT64>(tableOffset) * increment;
            for (const BindingSlot& slot : table.slots) {
                const BoundResource* bound = findBound(table.bindPoint, slot);
                if (bound == nullptr) return GfxError::BadDescriptor;
                if (slot.type == BindingType::Sampler) {
                    SamplerHandle handle{bound->handleIndex, bound->handleGeneration};
                    DxSamplerRecord* sampler = impl_->samplers.get(handle);
                    if (sampler == nullptr) return GfxError::InvalidHandle;
                    D3D12_SAMPLER_DESC desc{};
                    desc.Filter = dxFilter({sampler->rec.filter, sampler->rec.mipFilter,
                                            sampler->rec.addressU, sampler->rec.addressV,
                                            sampler->rec.addressW, sampler->rec.maxAnisotropy});
                    desc.AddressU = dxAddress(sampler->rec.addressU);
                    desc.AddressV = dxAddress(sampler->rec.addressV);
                    desc.AddressW = dxAddress(sampler->rec.addressW);
                    desc.MaxAnisotropy = static_cast<UINT>(sampler->rec.maxAnisotropy);
                    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                    desc.MinLOD = 0.0f;
                    desc.MaxLOD = sampler->rec.mipFilter == MipFilter::None
                                      ? 0.0f : D3D12_FLOAT32_MAX;
                    impl_->device->CreateSampler(&desc, cpu);
                } else if (slot.type == BindingType::Texture) {
                    TextureHandle handle{bound->handleIndex, bound->handleGeneration};
                    DxTextureRecord* texture = impl_->textures.get(handle);
                    if (texture == nullptr) return GfxError::InvalidHandle;
                    for (std::uint32_t mip = 0; mip < texture->rec.mipLevels; ++mip) {
                        transitionTextureMip(
                            list, *texture, mip,
                            static_cast<D3D12_RESOURCE_STATES>(
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                    }
                    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                    desc.Format = shaderViewFormat(texture->rec.format);
                    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    desc.Texture2D.MipLevels = texture->rec.mipLevels;
                    impl_->device->CreateShaderResourceView(texture->resource.get(), &desc, cpu);
                } else {
                    BufferHandle handle{bound->handleIndex, bound->handleGeneration};
                    DxBufferRecord* buffer = impl_->buffers.get(handle);
                    if (buffer == nullptr) return GfxError::InvalidHandle;
                    const BufferUsage required = slot.type == BindingType::UniformBuffer
                                                     ? BufferUsage::Uniform
                                                     : BufferUsage::Storage;
                    if (!hasUsage(buffer->rec.usage, required)) return GfxError::BadDescriptor;
                    if (slot.type == BindingType::UniformBuffer) {
                        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
                        desc.BufferLocation = buffer->resource->GetGPUVirtualAddress();
                        desc.SizeInBytes = static_cast<UINT>(buffer->allocationBytes);
                        impl_->device->CreateConstantBufferView(&desc, cpu);
                    } else {
                        if (buffer->rec.sizeBytes < 4 || (buffer->rec.sizeBytes % 4) != 0) {
                            return GfxError::BadDescriptor;
                        }
                        if (!buffer->upload) {
                            transition(list, buffer->resource.get(), buffer->state,
                                       static_cast<D3D12_RESOURCE_STATES>(
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                        }
                        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                        desc.Format = DXGI_FORMAT_R32_TYPELESS;
                        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        desc.Buffer.NumElements = static_cast<UINT>(buffer->rec.sizeBytes / 4);
                        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                        impl_->device->CreateShaderResourceView(buffer->resource.get(), &desc, cpu);
                    }
                }
                cpu.ptr += increment;
            }
            list->SetGraphicsRootDescriptorTable(table.rootIndex, gpu);
            if (table.samplers) samplerOffset += static_cast<UINT>(table.slots.size());
            else resourceOffset += static_cast<UINT>(table.slots.size());
        }
        return GfxError::None;
    };

    for (const Command& command : commandBuffer.commands()) {
        switch (command.kind) {
        case CommandKind::ResourceBarrier:
            if (rendering || command.after.stages == PipelineStageMask::None) {
                error = GfxError::BadDescriptor;
                break;
            }
            if (command.barrierType == BarrierResourceType::Texture) {
                DxTextureRecord* texture = impl_->textures.get(command.texture);
                if (texture == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                const bool depth = isDepthFormat(texture->rec.format);
                if (command.after.layout == TextureLayout::Undefined ||
                    command.after.layout == TextureLayout::Present ||
                    (depth && command.after.layout == TextureLayout::ColorAttachment) ||
                    (!depth && command.after.layout == TextureLayout::DepthStencilAttachment)) {
                    error = GfxError::BadDescriptor;
                    break;
                }
                const D3D12_RESOURCE_STATES target = dxResourceState(command.after, true);
                for (std::uint32_t mip = 0; mip < texture->rec.mipLevels; ++mip) {
                    transitionTextureMip(list, *texture, mip, target);
                }
            } else {
                DxBufferRecord* buffer = impl_->buffers.get(command.buffer);
                if (buffer == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                if (command.before.layout != TextureLayout::Undefined ||
                    command.after.layout != TextureLayout::Undefined) {
                    error = GfxError::BadDescriptor;
                    break;
                }
                if (!buffer->upload) {
                    transition(list, buffer->resource.get(), buffer->state,
                               dxResourceState(command.after, false));
                }
            }
            break;
        case CommandKind::BeginRenderPass: {
            activeFramebuffer = impl_->framebuffers.get(command.framebuffer);
            if (rendering || activeFramebuffer == nullptr) {
                error = activeFramebuffer == nullptr ? GfxError::InvalidHandle
                                                     : GfxError::BadDescriptor;
                break;
            }
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> colorHandles{};
            UINT colorCount = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
            bool hasDepth = false;
            for (const AttachmentDesc& attachment : activeFramebuffer->rec.attachments) {
                if (attachment.type == AttachmentType::Color) {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle =
                        frame.rtvs->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += static_cast<SIZE_T>(rtvOffset++) * rtvIncrement;
                    if (!attachment.texture.isValid()) {
                        DxSwapchainRecord* swapchain = impl_->swapchains.get(
                            activeFramebuffer->swapchain);
                        if (swapchain == nullptr || swapchain != submissionSwapchain) {
                            error = GfxError::InvalidHandle;
                            break;
                        }
                        const UINT imageIndex = swapchain->swapchain->GetCurrentBackBufferIndex();
                        transition(list, swapchain->buffers[imageIndex].get(),
                                   swapchain->states[imageIndex],
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                        impl_->device->CreateRenderTargetView(
                            swapchain->buffers[imageIndex].get(), nullptr, handle);
                        usedSwapchain = true;
                    } else {
                        DxTextureRecord* texture = impl_->textures.get(attachment.texture);
                        if (texture == nullptr || isDepthFormat(texture->rec.format)) {
                            error = texture == nullptr ? GfxError::InvalidHandle
                                                       : GfxError::BadDescriptor;
                            break;
                        }
                        transitionTextureMip(list, *texture, 0,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
                        impl_->device->CreateRenderTargetView(texture->resource.get(), nullptr,
                                                              handle);
                    }
                    colorHandles[colorCount++] = handle;
                    if (attachment.clearOnLoad) {
                        list->ClearRenderTargetView(handle, attachment.clear.color, 0, nullptr);
                    }
                } else {
                    if (hasDepth || !attachment.texture.isValid()) {
                        error = GfxError::BadDescriptor;
                        break;
                    }
                    DxTextureRecord* texture = impl_->textures.get(attachment.texture);
                    if (texture == nullptr || !isDepthFormat(texture->rec.format)) {
                        error = texture == nullptr ? GfxError::InvalidHandle
                                                   : GfxError::BadDescriptor;
                        break;
                    }
                    depthHandle = frame.dsvs->GetCPUDescriptorHandleForHeapStart();
                    depthHandle.ptr += static_cast<SIZE_T>(dsvOffset++) * dsvIncrement;
                    transitionTextureMip(list, *texture, 0,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
                    desc.Format = dxgiFormat(texture->rec.format);
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    impl_->device->CreateDepthStencilView(texture->resource.get(), &desc,
                                                          depthHandle);
                    hasDepth = true;
                    if (attachment.clearOnLoad) {
                        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
                        if (texture->rec.format == TextureFormat::Depth24Stencil8) {
                            flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_STENCIL);
                        }
                        list->ClearDepthStencilView(
                            depthHandle, flags, attachment.clear.depth,
                            static_cast<UINT8>(attachment.clear.stencil), 0, nullptr);
                    }
                }
            }
            if (error != GfxError::None) break;
            list->OMSetRenderTargets(colorCount, colorHandles.data(), FALSE,
                                     hasDepth ? &depthHandle : nullptr);
            const D3D12_VIEWPORT viewport{
                0.0f, 0.0f,
                static_cast<float>(activeFramebuffer->rec.extent.width),
                static_cast<float>(activeFramebuffer->rec.extent.height), 0.0f, 1.0f};
            const D3D12_RECT scissor{
                0, 0, static_cast<LONG>(activeFramebuffer->rec.extent.width),
                static_cast<LONG>(activeFramebuffer->rec.extent.height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            rendering = true;
            indexBound = false;
            boundPipeline = nullptr;
            break;
        }
        case CommandKind::EndRenderPass:
            if (!rendering) {
                error = GfxError::BadDescriptor;
            } else {
                if (activeFramebuffer != nullptr && activeFramebuffer->swapchain.isValid()) {
                    DxSwapchainRecord* swapchain = impl_->swapchains.get(
                        activeFramebuffer->swapchain);
                    if (swapchain == nullptr) {
                        error = GfxError::InvalidHandle;
                        break;
                    }
                    const UINT imageIndex = swapchain->swapchain->GetCurrentBackBufferIndex();
                    transition(list, swapchain->buffers[imageIndex].get(),
                               swapchain->states[imageIndex], D3D12_RESOURCE_STATE_PRESENT);
                }
                rendering = false;
                activeFramebuffer = nullptr;
            }
            break;
        case CommandKind::BindPipeline:
            boundPipeline = impl_->pipelines.get(command.pipeline);
            if (boundPipeline == nullptr) {
                error = GfxError::InvalidHandle;
            } else if (rendering &&
                       (activeFramebuffer == nullptr ||
                        boundPipeline->colorFormats != activeFramebuffer->colorFormats ||
                        (boundPipeline->depthFormat != DXGI_FORMAT_UNKNOWN &&
                         boundPipeline->depthFormat != activeFramebuffer->depthFormat))) {
                error = GfxError::BadDescriptor;
            } else {
                list->SetPipelineState(boundPipeline->pipeline.get());
                list->SetGraphicsRootSignature(boundPipeline->rootSignature.get());
                list->IASetPrimitiveTopology(dxTopology(boundPipeline->rec.desc.topology));
            }
            break;
        case CommandKind::BindVertexBuffer: {
            DxBufferRecord* buffer = impl_->buffers.get(command.buffer);
            if (buffer == nullptr || !hasUsage(buffer->rec.usage, BufferUsage::Vertex) ||
                command.strideBytes == 0 || command.strideBytes > buffer->rec.sizeBytes) {
                error = buffer == nullptr ? GfxError::InvalidHandle : GfxError::BadDescriptor;
            } else {
                D3D12_VERTEX_BUFFER_VIEW view{};
                view.BufferLocation = buffer->resource->GetGPUVirtualAddress();
                view.SizeInBytes = static_cast<UINT>(buffer->rec.sizeBytes);
                view.StrideInBytes = command.strideBytes;
                list->IASetVertexBuffers(command.vertexSlot, 1, &view);
            }
            break;
        }
        case CommandKind::BindIndexBuffer: {
            DxBufferRecord* buffer = impl_->buffers.get(command.buffer);
            if (buffer == nullptr || !hasUsage(buffer->rec.usage, BufferUsage::Index)) {
                error = GfxError::InvalidHandle;
            } else {
                D3D12_INDEX_BUFFER_VIEW view{};
                view.BufferLocation = buffer->resource->GetGPUVirtualAddress();
                view.SizeInBytes = static_cast<UINT>(buffer->rec.sizeBytes);
                view.Format = command.indexFormat == IndexFormat::U16
                                  ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                list->IASetIndexBuffer(&view);
                indexBound = true;
            }
            break;
        }
        case CommandKind::BindResources:
            for (std::uint8_t index = 0; index < command.resourceCount; ++index) {
                const BoundResource& resource = command.resources[index];
                if (boundPipeline != nullptr) {
                    const auto group = std::find_if(
                        boundPipeline->rec.desc.layout.groups.begin(),
                        boundPipeline->rec.desc.layout.groups.end(),
                        [&](const BindGroup& item) { return item.bindPoint == resource.bindPoint; });
                    if (group == boundPipeline->rec.desc.layout.groups.end() ||
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
                    boundResources.begin(), boundResources.end(), [&](const BoundResource& item) {
                        return item.bindPoint == resource.bindPoint && item.slot == resource.slot &&
                               item.type == resource.type;
                    });
                if (existing == boundResources.end()) boundResources.push_back(resource);
                else *existing = resource;
            }
            break;
        case CommandKind::SetViewport: {
            if (command.viewport.width == 0 || command.viewport.height == 0) {
                error = GfxError::BadDescriptor;
                break;
            }
            const D3D12_VIEWPORT viewport{
                0.0f, 0.0f, static_cast<float>(command.viewport.width),
                static_cast<float>(command.viewport.height), 0.0f, 1.0f};
            list->RSSetViewports(1, &viewport);
            break;
        }
        case CommandKind::SetScissor: {
            if (command.scissor.width == 0 || command.scissor.height == 0) {
                error = GfxError::BadDescriptor;
                break;
            }
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(command.scissor.width),
                                     static_cast<LONG>(command.scissor.height)};
            list->RSSetScissorRects(1, &scissor);
            break;
        }
        case CommandKind::Draw:
            if (!rendering || boundPipeline == nullptr) {
                error = GfxError::BadDescriptor;
            } else if ((error = bindTables(*boundPipeline)) == GfxError::None) {
                list->DrawInstanced(command.vertexCount, command.instanceCount, 0, 0);
                ++delta.drawCalls;
                ++delta.batches;
                delta.instances += command.instanceCount;
                delta.vertices += static_cast<std::uint64_t>(command.vertexCount) *
                                  command.instanceCount;
                delta.primitives += countPrimitives(boundPipeline->rec.desc.topology,
                                                     command.vertexCount) *
                                    command.instanceCount;
            }
            break;
        case CommandKind::DrawIndexed:
            if (!rendering || boundPipeline == nullptr || !indexBound) {
                error = GfxError::BadDescriptor;
            } else if ((error = bindTables(*boundPipeline)) == GfxError::None) {
                list->DrawIndexedInstanced(command.indexCount, command.instanceCount,
                                           command.firstIndex, 0, 0);
                ++delta.drawIndexedCalls;
                ++delta.batches;
                delta.instances += command.instanceCount;
                delta.vertices += static_cast<std::uint64_t>(command.indexCount) *
                                  command.instanceCount;
                delta.primitives += countPrimitives(boundPipeline->rec.desc.topology,
                                                     command.indexCount) *
                                    command.instanceCount;
            }
            break;
        }
        if (error != GfxError::None) break;
    }
    if (rendering && error == GfxError::None) error = GfxError::BadDescriptor;
    result = list->Close();
    if (error != GfxError::None) {
        rollbackStates();
        return error;
    }
    if (FAILED(result)) {
        rollbackStates();
        return toGfxError(result);
    }

    ID3D12CommandList* lists[] = {list};
    impl_->queue->ExecuteCommandLists(1, lists);
    HRESULT presentResult = S_OK;
    if (usedSwapchain && submissionSwapchain != nullptr) {
        const UINT syncInterval = submissionSwapchain->rec.presentMode == PresentMode::Immediate
                                      ? 0u : 1u;
        presentResult = submissionSwapchain->swapchain->Present(syncInterval, 0);
    }
    frame.fenceValue = ++impl_->lastSubmittedFence;
    result = impl_->queue->Signal(impl_->fence.get(), frame.fenceValue);
    if (FAILED(result)) return toGfxError(result);
    impl_->frameIndex = (impl_->frameIndex + 1) % kFramesInFlight;
    if (FAILED(presentResult)) return toGfxError(presentResult);
    impl_->stats.drawCalls += delta.drawCalls;
    impl_->stats.drawIndexedCalls += delta.drawIndexedCalls;
    impl_->stats.batches += delta.batches;
    impl_->stats.instances += delta.instances;
    impl_->stats.primitives += delta.primitives;
    impl_->stats.vertices += delta.vertices;
    return GfxError::None;
}

auto Dx12Device::readTexture(TextureHandle handle, std::uint32_t mipLevel,
                             std::span<std::byte> destination) -> GfxError {
    DxTextureRecord* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels) return GfxError::BadDescriptor;
    const std::uint32_t width = std::max(1u, texture->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, texture->rec.height >> mipLevel);
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) *
                                   textureFormatBytes(texture->rec.format);
    if (destination.size() != rowBytes * height) return GfxError::BadDescriptor;

    const D3D12_RESOURCE_DESC textureDesc = texture->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 tightRowBytes = 0;
    UINT64 totalBytes = 0;
    impl_->device->GetCopyableFootprints(
        &textureDesc, mipLevel, 1, 0, &footprint, &rowCount, &tightRowBytes, &totalBytes);
    if (rowCount != height || tightRowBytes != rowBytes) return GfxError::BadDescriptor;
    ComPtr<ID3D12Resource> readback;
    const D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readbackDesc = bufferResourceDesc(totalBytes);
    HRESULT result = impl_->device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &readbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource,
        readback.putVoid());
    if (FAILED(result)) return toGfxError(result);
    result = impl_->executeImmediate([&](ID3D12GraphicsCommandList* list) {
        transitionTextureMip(list, *texture, mipLevel, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture->resource.get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = mipLevel;
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = readback.get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprint;
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    });
    if (FAILED(result)) return toGfxError(result);
    void* mapped = nullptr;
    const D3D12_RANGE readRange{static_cast<SIZE_T>(footprint.Offset),
                                static_cast<SIZE_T>(totalBytes)};
    result = readback->Map(0, &readRange, &mapped);
    if (FAILED(result)) return toGfxError(result);
    const auto* source = static_cast<const std::byte*>(mapped) + footprint.Offset;
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination.data() + static_cast<std::size_t>(row) * rowBytes,
                    source + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                    static_cast<std::size_t>(rowBytes));
    }
    const D3D12_RANGE writtenRange{0, 0};
    readback->Unmap(0, &writtenRange);
    return GfxError::None;
}

auto Dx12Device::waitIdle() -> void {
    if (!ready()) return;
    const std::uint64_t signal = ++impl_->lastSubmittedFence;
    if (SUCCEEDED(impl_->queue->Signal(impl_->fence.get(), signal))) {
        (void)impl_->waitFor(signal);
    }
    collectRetired();
}

auto Dx12Device::stats() const -> DeviceStats { return impl_->stats; }

auto Dx12Device::resetFrameStats() -> void {
    impl_->stats.drawCalls = 0;
    impl_->stats.drawIndexedCalls = 0;
    impl_->stats.batches = 0;
    impl_->stats.instances = 0;
    impl_->stats.primitives = 0;
    impl_->stats.vertices = 0;
}

auto Dx12Device::deviceName() const -> const char* {
    return impl_ != nullptr ? impl_->deviceName : "";
}

auto Dx12Device::featureLevel() const -> std::uint32_t {
    return impl_ != nullptr ? impl_->featureLevel : 0;
}

auto Dx12Device::debugLayerEnabled() const -> bool {
    return impl_ != nullptr && impl_->debugLayer;
}

auto Dx12Device::validationWarningCount() const -> std::uint64_t {
    return impl_ != nullptr ? countMessages(*impl_, D3D12_MESSAGE_SEVERITY_WARNING) : 0;
}

auto Dx12Device::validationErrorCount() const -> std::uint64_t {
    if (impl_ == nullptr) return 0;
    return countMessages(*impl_, D3D12_MESSAGE_SEVERITY_ERROR) +
           countMessages(*impl_, D3D12_MESSAGE_SEVERITY_CORRUPTION);
}

auto Dx12Device::hasWindow() const -> bool {
    return impl_ != nullptr && impl_->window != nullptr;
}

auto Dx12Device::preferredSurfaceFormat() const -> TextureFormat {
    return TextureFormat::RGBA8_UNORM;
}

auto Dx12Device::framesInFlight() const -> std::uint32_t {
    return static_cast<std::uint32_t>(kFramesInFlight);
}

auto createDx12Device(platform::Window* window) -> IDevice* {
    auto* device = new Dx12Device(window);
    if (!device->ready()) {
        delete device;
        return nullptr;
    }
    return device;
}

} // namespace f4::gfx
