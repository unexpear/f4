#include "backends/vulkan/vulkan_device.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "core/logging/logger.h"
#include "graphics/gpu_selection.h"
#include "graphics/device/object_store.h"
#if defined(F4_VULKAN_HAS_PLATFORM)
#include "platform/window.h"
#endif

namespace f4::gfx {

namespace {

constexpr auto kInvalidQueueFamily = std::numeric_limits<std::uint32_t>::max();
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::size_t kFramesInFlight = 2;

struct ValidationCounts {
    std::atomic_uint64_t warnings{0};
    std::atomic_uint64_t errors{0};
};

template <typename Property>
[[nodiscard]] auto hasNamedProperty(const std::vector<Property>& properties, const char* name)
    -> bool {
    return std::any_of(properties.begin(), properties.end(), [name](const Property& property) {
        if constexpr (requires { property.layerName; }) {
            return std::strcmp(property.layerName, name) == 0;
        } else {
            return std::strcmp(property.extensionName, name) == 0;
        }
    });
}

[[nodiscard]] auto enumerateInstanceLayers() -> std::vector<VkLayerProperties> {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkLayerProperties> properties(count);
    if (count > 0 && vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return {};
    }
    properties.resize(count);
    return properties;
}

[[nodiscard]] auto enumerateInstanceExtensions(const char* layerName = nullptr)
    -> std::vector<VkExtensionProperties> {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0 &&
        vkEnumerateInstanceExtensionProperties(layerName, &count, properties.data()) != VK_SUCCESS) {
        return {};
    }
    properties.resize(count);
    return properties;
}

[[nodiscard]] auto hasDeviceExtension(VkPhysicalDevice device, const char* name) -> bool {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0 && vkEnumerateDeviceExtensionProperties(
                         device, nullptr, &count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    properties.resize(count);
    return hasNamedProperty(properties, name);
}

VKAPI_ATTR auto VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData) -> VkBool32;

[[nodiscard]] auto debugMessengerCreateInfo(void* userData)
    -> VkDebugUtilsMessengerCreateInfoEXT {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = validationCallback;
    info.pUserData = userData;
    return info;
}

struct QueueSelection {
    std::uint32_t graphics = kInvalidQueueFamily;
    std::uint32_t present = kInvalidQueueFamily;

    [[nodiscard]] auto complete(bool needsPresent) const -> bool {
        return graphics != kInvalidQueueFamily &&
               (!needsPresent || present != kInvalidQueueFamily);
    }
};

[[nodiscard]] auto selectQueues(VkPhysicalDevice device, VkSurfaceKHR surface) -> QueueSelection {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    if (count > 0) vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

    QueueSelection result;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (properties[i].queueCount == 0) continue;
        if (result.graphics == kInvalidQueueFamily &&
            (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            result.graphics = i;
        }
        if (surface != VK_NULL_HANDLE) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported) == VK_SUCCESS &&
                supported == VK_TRUE) {
                result.present = i;
                if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    result.graphics = i;
                    break;
                }
            }
        }
    }
    if (surface == VK_NULL_HANDLE) result.present = result.graphics;
    return result;
}

[[nodiscard]] auto gpuClassOf(VkPhysicalDeviceType type) -> GpuClass {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return GpuClass::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return GpuClass::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return GpuClass::Virtual;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return GpuClass::Software;
    default: return GpuClass::Other;
    }
}

[[nodiscard]] auto deviceLocalBytes(VkPhysicalDevice device) -> std::uint64_t {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    std::uint64_t bytes = 0;
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            bytes += memory.memoryHeaps[heap].size;
        }
    }
    return bytes;
}

[[nodiscard]] auto toGfxError(VkResult result) -> GfxError {
    switch (result) {
    case VK_SUCCESS: return GfxError::None;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return GfxError::OutOfMemory;
    case VK_ERROR_DEVICE_LOST: return GfxError::DeviceLost;
    default: return GfxError::BackendUnavailable;
    }
}

[[nodiscard]] auto vkBufferUsage(BufferUsage usage) -> VkBufferUsageFlags {
    VkBufferUsageFlags flags = 0;
    if (hasUsage(usage, BufferUsage::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (hasUsage(usage, BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

[[nodiscard]] auto vkTextureFormat(TextureFormat format) -> VkFormat {
    switch (format) {
    case TextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case TextureFormat::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] constexpr auto isDepthFormat(TextureFormat format) -> bool {
    return format == TextureFormat::Depth32F || format == TextureFormat::Depth24Stencil8;
}

[[nodiscard]] auto imageAspect(TextureFormat format) -> VkImageAspectFlags {
    if (format == TextureFormat::Depth24Stencil8) {
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return format == TextureFormat::Depth32F ? VK_IMAGE_ASPECT_DEPTH_BIT
                                             : VK_IMAGE_ASPECT_COLOR_BIT;
}

[[nodiscard]] auto vkAddressMode(AddressMode mode) -> VkSamplerAddressMode {
    switch (mode) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

[[nodiscard]] auto minFilter(TextureFilter filter) -> VkFilter {
    return filter == TextureFilter::MinMagNearest ||
                   filter == TextureFilter::MinNearestMagLinear
               ? VK_FILTER_NEAREST
               : VK_FILTER_LINEAR;
}

[[nodiscard]] auto magFilter(TextureFilter filter) -> VkFilter {
    return filter == TextureFilter::MinMagNearest ||
                   filter == TextureFilter::MinLinearMagNearest
               ? VK_FILTER_NEAREST
               : VK_FILTER_LINEAR;
}

[[nodiscard]] auto mipFilter(MipFilter filter) -> VkSamplerMipmapMode {
    return filter == MipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

[[nodiscard]] auto vkShaderStage(ShaderStage stage) -> VkShaderStageFlagBits {
    switch (stage) {
    case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::TessEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    return VK_SHADER_STAGE_ALL;
}

[[nodiscard]] auto vkShaderStages(std::uint32_t stageMask,
                                  VkShaderStageFlags defaultStages) -> VkShaderStageFlags {
    if (stageMask == 0) return defaultStages;
    VkShaderStageFlags result = 0;
    if ((stageMask & shaderStageBit(ShaderStage::Vertex)) != 0) {
        result |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((stageMask & shaderStageBit(ShaderStage::Fragment)) != 0) {
        result |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if ((stageMask & shaderStageBit(ShaderStage::Compute)) != 0) {
        result |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return result;
}

[[nodiscard]] auto vkDescriptorType(BindingType type) -> VkDescriptorType {
    switch (type) {
    case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::Texture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

[[nodiscard]] auto vkTopology(PrimitiveTopology topology) -> VkPrimitiveTopology {
    switch (topology) {
    case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

[[nodiscard]] auto vkVertexFormat(VertexFormat format) -> VkFormat {
    switch (format) {
    case VertexFormat::Float32x2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float32x3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::UNorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] auto vkCompare(CompareOp compare) -> VkCompareOp {
    switch (compare) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

[[nodiscard]] auto vkCull(CullMode cull) -> VkCullModeFlags {
    switch (cull) {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

[[nodiscard]] auto vkBlendFactor(BlendFactor factor) -> VkBlendFactor {
    switch (factor) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

[[nodiscard]] auto vkBlendOp(BlendOp op) -> VkBlendOp {
    switch (op) {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Sub: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::RevSub: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

[[nodiscard]] auto vkPresentMode(PresentMode mode) -> VkPresentModeKHR {
    switch (mode) {
    case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] auto vkColorMask(std::uint32_t mask) -> VkColorComponentFlags {
    VkColorComponentFlags result = 0;
    if ((mask & ColorWriteMask::R) != 0) result |= VK_COLOR_COMPONENT_R_BIT;
    if ((mask & ColorWriteMask::G) != 0) result |= VK_COLOR_COMPONENT_G_BIT;
    if ((mask & ColorWriteMask::B) != 0) result |= VK_COLOR_COMPONENT_B_BIT;
    if ((mask & ColorWriteMask::A) != 0) result |= VK_COLOR_COMPONENT_A_BIT;
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

struct VkBufferRecord {
    GpuBuffer rec;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct VkTextureRecord {
    GpuTexture rec;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::vector<VkImageLayout> layouts;
};

struct VkSamplerRecord {
    GpuSampler rec;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VkShaderRecord {
    GpuShader rec;
    VkShaderModule module = VK_NULL_HANDLE;
};

struct VkPipelineRecord {
    GpuPipeline rec;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> setLayouts;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool compute = false;
};

struct VkFramebufferRecord {
    GpuFramebuffer rec;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    SwapchainHandle swapchain{};
};

struct VkSwapchainRecord {
    GpuSwapchain rec;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkImageLayout> layouts;
    std::vector<VkSemaphore> renderComplete;
    std::uint32_t imageIndex = 0;
};

struct VkFrameSlot {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::uint64_t serial = 0;
    bool pending = false;
};

template <typename Record>
struct RetiredRecord {
    Record record;
    std::uint64_t afterSerial = 0;
};

} // namespace

struct VulkanDevice::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkFrameSlot, kFramesInFlight> frames;
    std::size_t frameIndex = 0;
    std::uint64_t lastSubmittedSerial = 0;
    std::uint64_t completedSerial = 0;
    std::uint32_t graphicsQueueFamily = kInvalidQueueFamily;
    std::uint32_t presentQueueFamily = kInvalidQueueFamily;
    std::uint32_t apiVersion = 0;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};
    bool validationEnabled = false;
    bool synchronizationValidationEnabled = false;
    bool samplerAnisotropy = false;
    bool depthClamp = false;
    bool independentBlend = false;
    float maxSamplerAnisotropy = 1.0f;
    bool initialized = false;
    ValidationCounts validation;
    DeviceStats stats;
    ObjectStore<VkBufferRecord, GpuBufferTag> buffers;
    ObjectStore<VkTextureRecord, GpuTextureTag> textures;
    ObjectStore<VkSamplerRecord, GpuSamplerTag> samplers;
    ObjectStore<VkShaderRecord, GpuShaderTag> shaders;
    ObjectStore<VkPipelineRecord, GpuPipelineTag> pipelines;
    ObjectStore<VkFramebufferRecord, GpuFramebufferTag> framebuffers;
    ObjectStore<VkSwapchainRecord, GpuSwapchainTag> swapchains;
    SwapchainHandle activeSwapchain{};
    std::vector<RetiredRecord<VkBufferRecord>> retiredBuffers;
    std::vector<RetiredRecord<VkTextureRecord>> retiredTextures;
    std::vector<RetiredRecord<VkSamplerRecord>> retiredSamplers;
    std::vector<RetiredRecord<VkPipelineRecord>> retiredPipelines;
};

namespace {

VKAPI_ATTR auto VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData) -> VkBool32 {
    auto* counts = static_cast<ValidationCounts*>(userData);
    const char* message = callbackData != nullptr && callbackData->pMessage != nullptr
                              ? callbackData->pMessage
                              : "Vulkan validation message without text";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        if (counts != nullptr) ++counts->errors;
        F4_LOG_ERROR() << "Vulkan validation: " << message;
    } else {
        if (counts != nullptr) ++counts->warnings;
        F4_LOG_WARNING() << "Vulkan validation: " << message;
    }
    return VK_FALSE;
}

void destroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    if (instance == VK_NULL_HANDLE || messenger == VK_NULL_HANDLE) return;
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy != nullptr) destroy(instance, messenger, nullptr);
}

[[nodiscard]] auto findMemoryType(VkPhysicalDevice physicalDevice, std::uint32_t typeBits,
                                  VkMemoryPropertyFlags required,
                                  std::uint32_t* memoryTypeIndex) -> bool {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            *memoryTypeIndex = i;
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto createBufferAllocation(VkPhysicalDevice physicalDevice, VkDevice device,
                                          VkDeviceSize size, VkBufferUsageFlags usage,
                                          VkMemoryPropertyFlags memoryProperties,
                                          VkBuffer* buffer, VkDeviceMemory* memory) -> GfxError {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, buffer);
    if (result != VK_SUCCESS) return toGfxError(result);

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    std::uint32_t memoryTypeIndex = 0;
    if (!findMemoryType(physicalDevice, requirements.memoryTypeBits, memoryProperties,
                        &memoryTypeIndex)) {
        vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return GfxError::BackendUnavailable;
    }

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryTypeIndex;
    result = vkAllocateMemory(device, &allocationInfo, nullptr, memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return toGfxError(result);
    }
    result = vkBindBufferMemory(device, *buffer, *memory, 0);
    if (result != VK_SUCCESS) {
        vkFreeMemory(device, *memory, nullptr);
        vkDestroyBuffer(device, *buffer, nullptr);
        *memory = VK_NULL_HANDLE;
        *buffer = VK_NULL_HANDLE;
        return toGfxError(result);
    }
    return GfxError::None;
}

void destroyBufferAllocation(VkDevice device, VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
}

[[nodiscard]] auto beginOneTimeCommands(VkDevice device, VkCommandPool pool,
                                        VkCommandBuffer* commandBuffer) -> GfxError {
    VkCommandBufferAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocationInfo.commandPool = pool;
    allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocationInfo.commandBufferCount = 1;
    VkResult result = vkAllocateCommandBuffers(device, &allocationInfo, commandBuffer);
    if (result != VK_SUCCESS) return toGfxError(result);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(*commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        vkFreeCommandBuffers(device, pool, 1, commandBuffer);
        *commandBuffer = VK_NULL_HANDLE;
        return toGfxError(result);
    }
    return GfxError::None;
}

[[nodiscard]] auto endOneTimeCommands(VkDevice device, VkCommandPool pool, VkQueue queue,
                                      VkCommandBuffer commandBuffer) -> GfxError {
    VkResult result = vkEndCommandBuffer(commandBuffer);
    if (result == VK_SUCCESS) {
        VkCommandBufferSubmitInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandInfo.commandBuffer = commandBuffer;
        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        result = vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE);
    }
    if (result == VK_SUCCESS) result = vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
    return toGfxError(result);
}

struct LayoutAccess {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

[[nodiscard]] auto sourceAccess(VkImageLayout layout) -> LayoutAccess {
    switch (layout) {
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    default: return {};
    }
}

[[nodiscard]] auto vkPipelineStages(std::uint32_t stages) -> VkPipelineStageFlags2 {
    VkPipelineStageFlags2 result = 0;
    if ((stages & PipelineStageMask::VertexInput) != 0) {
        result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    }
    if ((stages & PipelineStageMask::VertexShader) != 0) {
        result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if ((stages & PipelineStageMask::FragmentShader) != 0) {
        result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if ((stages & PipelineStageMask::ComputeShader) != 0) {
        result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if ((stages & PipelineStageMask::ColorAttachment) != 0) {
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if ((stages & PipelineStageMask::DepthStencil) != 0) {
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if ((stages & PipelineStageMask::Transfer) != 0) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if ((stages & PipelineStageMask::Host) != 0) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if ((stages & PipelineStageMask::AllGraphics) != 0) {
        result |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    }
    if ((stages & PipelineStageMask::AllCommands) != 0) {
        result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return result;
}

[[nodiscard]] auto vkAccess(std::uint32_t access) -> VkAccessFlags2 {
    VkAccessFlags2 result = 0;
    if ((access & ResourceAccessMask::VertexRead) != 0) {
        result |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if ((access & ResourceAccessMask::IndexRead) != 0) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if ((access & ResourceAccessMask::UniformRead) != 0) {
        result |= VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    if ((access & ResourceAccessMask::ShaderRead) != 0) {
        result |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    if ((access & ResourceAccessMask::ShaderWrite) != 0) {
        result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if ((access & ResourceAccessMask::ColorRead) != 0) {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    }
    if ((access & ResourceAccessMask::ColorWrite) != 0) {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((access & ResourceAccessMask::DepthRead) != 0) {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if ((access & ResourceAccessMask::DepthWrite) != 0) {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if ((access & ResourceAccessMask::TransferRead) != 0) {
        result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if ((access & ResourceAccessMask::TransferWrite) != 0) {
        result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if ((access & ResourceAccessMask::HostRead) != 0) result |= VK_ACCESS_2_HOST_READ_BIT;
    if ((access & ResourceAccessMask::HostWrite) != 0) result |= VK_ACCESS_2_HOST_WRITE_BIT;
    return result;
}

[[nodiscard]] auto vkTextureLayout(TextureLayout layout) -> VkImageLayout {
    switch (layout) {
    case TextureLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case TextureLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
    case TextureLayout::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case TextureLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case TextureLayout::DepthStencilAttachment:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case TextureLayout::TransferSource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case TextureLayout::TransferDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case TextureLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

void transitionTexture(VkCommandBuffer commandBuffer, VkTextureRecord& texture,
                       std::uint32_t mipLevel, VkImageLayout newLayout,
                       VkPipelineStageFlags2 destinationStage,
                       VkAccessFlags2 destinationAccess) {
    if (texture.layouts[mipLevel] == newLayout) return;
    const LayoutAccess source = sourceAccess(texture.layouts[mipLevel]);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stage;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = texture.layouts[mipLevel];
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {imageAspect(texture.rec.format), mipLevel, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    texture.layouts[mipLevel] = newLayout;
}

void transitionSwapchainImage(VkCommandBuffer commandBuffer, VkImage image,
                              VkImageLayout* oldLayout, VkImageLayout newLayout,
                              VkPipelineStageFlags2 destinationStage,
                              VkAccessFlags2 destinationAccess) {
    if (*oldLayout == newLayout) return;
    LayoutAccess source = sourceAccess(*oldLayout);
    if (*oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        *oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        // The acquire semaphore is waited at color-output; include that stage
        // in the transition's source scope so the acquire read happens-before
        // the layout-transition write under synchronization validation.
        source.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stage;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = *oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    *oldLayout = newLayout;
}

} // namespace

void VulkanDevice::collectRetired() {
    auto eraseCompleted = [&](auto& records, auto&& destroy) {
        records.erase(std::remove_if(records.begin(), records.end(), [&](auto& retired) {
                          if (retired.afterSerial > impl_->completedSerial) return false;
                          destroy(retired.record);
                          return true;
                      }),
                      records.end());
    };
    eraseCompleted(impl_->retiredPipelines, [&](VkPipelineRecord& pipeline) {
        vkDestroyPipeline(impl_->device, pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(impl_->device, pipeline.layout, nullptr);
        for (VkDescriptorSetLayout setLayout : pipeline.setLayouts) {
            vkDestroyDescriptorSetLayout(impl_->device, setLayout, nullptr);
        }
    });
    eraseCompleted(impl_->retiredSamplers, [&](VkSamplerRecord& sampler) {
        vkDestroySampler(impl_->device, sampler.sampler, nullptr);
    });
    eraseCompleted(impl_->retiredTextures, [&](VkTextureRecord& texture) {
        vkDestroyImageView(impl_->device, texture.view, nullptr);
        vkDestroyImage(impl_->device, texture.image, nullptr);
        vkFreeMemory(impl_->device, texture.memory, nullptr);
    });
    eraseCompleted(impl_->retiredBuffers, [&](VkBufferRecord& buffer) {
        destroyBufferAllocation(impl_->device, buffer.buffer, buffer.memory);
    });
}

VulkanDevice::VulkanDevice(platform::Window* window) : impl_(new Impl()) {
    if (impl_ != nullptr) impl_->initialized = initialize(window);
}

VulkanDevice::~VulkanDevice() {
    if (impl_ == nullptr) return;
    if (impl_->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(impl_->device);
        impl_->completedSerial = impl_->lastSubmittedSerial;
        collectRetired();
        impl_->swapchains.forEach([&](VkSwapchainRecord* swapchain) {
            for (VkSemaphore semaphore : swapchain->renderComplete) {
                vkDestroySemaphore(impl_->device, semaphore, nullptr);
            }
            for (VkImageView view : swapchain->views) {
                vkDestroyImageView(impl_->device, view, nullptr);
            }
            if (swapchain->swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(impl_->device, swapchain->swapchain, nullptr);
            }
        });
        for (VkFrameSlot& frame : impl_->frames) {
            if (frame.descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(impl_->device, frame.descriptorPool, nullptr);
            }
        }
        impl_->pipelines.forEach([&](VkPipelineRecord* pipeline) {
            if (pipeline->pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(impl_->device, pipeline->pipeline, nullptr);
            }
            if (pipeline->layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(impl_->device, pipeline->layout, nullptr);
            }
            for (VkDescriptorSetLayout setLayout : pipeline->setLayouts) {
                vkDestroyDescriptorSetLayout(impl_->device, setLayout, nullptr);
            }
        });
        impl_->shaders.forEach([&](VkShaderRecord* shader) {
            if (shader->module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(impl_->device, shader->module, nullptr);
            }
        });
        impl_->samplers.forEach([&](VkSamplerRecord* sampler) {
            if (sampler->sampler != VK_NULL_HANDLE) {
                vkDestroySampler(impl_->device, sampler->sampler, nullptr);
            }
        });
        impl_->textures.forEach([&](VkTextureRecord* texture) {
            if (texture->view != VK_NULL_HANDLE) vkDestroyImageView(impl_->device, texture->view, nullptr);
            if (texture->image != VK_NULL_HANDLE) vkDestroyImage(impl_->device, texture->image, nullptr);
            if (texture->memory != VK_NULL_HANDLE) vkFreeMemory(impl_->device, texture->memory, nullptr);
        });
        impl_->buffers.forEach([&](VkBufferRecord* buffer) {
            destroyBufferAllocation(impl_->device, buffer->buffer, buffer->memory);
        });
        if (impl_->commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl_->device, impl_->commandPool, nullptr);
        }
        for (VkFrameSlot& frame : impl_->frames) {
            if (frame.fence != VK_NULL_HANDLE) {
                vkDestroyFence(impl_->device, frame.fence, nullptr);
            }
            if (frame.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(impl_->device, frame.imageAvailable, nullptr);
            }
        }
        vkDestroyDevice(impl_->device, nullptr);
    }
    if (impl_->surface != VK_NULL_HANDLE && impl_->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    }
    destroyDebugMessenger(impl_->instance, impl_->debugMessenger);
    if (impl_->instance != VK_NULL_HANDLE) vkDestroyInstance(impl_->instance, nullptr);
    delete impl_;
}

auto VulkanDevice::initialize(platform::Window* window) -> bool {
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS) {
        return false;
    }
    if (loaderVersion < VK_API_VERSION_1_3) {
        F4_LOG_ERROR() << "Vulkan 1.3 loader required";
        return false;
    }

    std::vector<const char*> extensions;
    if (window != nullptr) {
#if defined(F4_VULKAN_HAS_PLATFORM)
        std::uint32_t count = 0;
        if (!window->vulkanInstanceExtensions(&count, nullptr) || count == 0) return false;
        extensions.resize(count);
        if (!window->vulkanInstanceExtensions(&count, extensions.data())) return false;
        extensions.resize(count);
#else
        F4_LOG_ERROR() << "Vulkan window requested without SDL platform support";
        return false;
#endif
    }

    const std::vector<VkLayerProperties> layers = enumerateInstanceLayers();
    const std::vector<VkExtensionProperties> availableExtensions = enumerateInstanceExtensions();
    const std::vector<VkExtensionProperties> validationExtensions =
        hasNamedProperty(layers, kValidationLayer)
            ? enumerateInstanceExtensions(kValidationLayer)
            : std::vector<VkExtensionProperties>{};
#if defined(F4_VULKAN_VALIDATION)
    impl_->validationEnabled = hasNamedProperty(layers, kValidationLayer) &&
                               hasNamedProperty(availableExtensions,
                                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (!impl_->validationEnabled) {
        F4_LOG_WARNING() << "Vulkan validation requested but layer/debug-utils is unavailable";
    }
#endif
    if (impl_->validationEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const bool hasLayerSettings =
            hasNamedProperty(validationExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        const bool hasValidationFeatures =
            hasNamedProperty(validationExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        impl_->synchronizationValidationEnabled = hasLayerSettings || hasValidationFeatures;
        if (hasLayerSettings) {
            extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        } else if (hasValidationFeatures) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        } else {
            F4_LOG_WARNING() << "Vulkan synchronization validation is unavailable";
        }
    }

    for (const char* extension : extensions) {
        if (!hasNamedProperty(availableExtensions, extension) &&
            !hasNamedProperty(validationExtensions, extension)) {
            F4_LOG_ERROR() << "Required Vulkan instance extension unavailable: " << extension;
            return false;
        }
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "f4";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.pEngineName = "f4";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerCreateInfo(&impl_->validation);
    const VkBool32 enableSynchronizationValidation = VK_TRUE;
    const VkLayerSettingEXT synchronizationSetting = {
        kValidationLayer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
        &enableSynchronizationValidation};
    VkLayerSettingsCreateInfoEXT layerSettings{};
    layerSettings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layerSettings.pNext = &debugInfo;
    layerSettings.settingCount = 1;
    layerSettings.pSettings = &synchronizationSetting;
    const VkValidationFeatureEnableEXT synchronizationFeature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.pNext = &debugInfo;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &synchronizationFeature;
    const bool useLayerSettings = impl_->synchronizationValidationEnabled &&
                                  hasNamedProperty(validationExtensions,
                                                   VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = useLayerSettings
                             ? static_cast<const void*>(&layerSettings)
                         : impl_->synchronizationValidationEnabled
                             ? static_cast<const void*>(&validationFeatures)
                         : impl_->validationEnabled
                             ? static_cast<const void*>(&debugInfo)
                             : nullptr;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = impl_->validationEnabled ? 1u : 0u;
    instanceInfo.ppEnabledLayerNames = impl_->validationEnabled ? &kValidationLayer : nullptr;
    if (vkCreateInstance(&instanceInfo, nullptr, &impl_->instance) != VK_SUCCESS) return false;

    if (impl_->validationEnabled) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(impl_->instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create == nullptr ||
            create(impl_->instance, &debugInfo, nullptr, &impl_->debugMessenger) != VK_SUCCESS) {
            return false;
        }
    }

    if (window != nullptr) {
#if defined(F4_VULKAN_HAS_PLATFORM)
        if (!window->createVulkanSurface(reinterpret_cast<void*>(impl_->instance),
                                         &impl_->surface)) {
            return false;
        }
#endif
    }

    std::uint32_t physicalCount = 0;
    if (vkEnumeratePhysicalDevices(impl_->instance, &physicalCount, nullptr) != VK_SUCCESS ||
        physicalCount == 0) {
        return false;
    }
    std::vector<VkPhysicalDevice> devices(physicalCount);
    if (vkEnumeratePhysicalDevices(impl_->instance, &physicalCount, devices.data()) != VK_SUCCESS) {
        return false;
    }

    // Rank every eligible device (see graphics/gpu_selection.h).
    struct EligibleDevice {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        QueueSelection queues{};
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceFeatures features{};
    };
    std::vector<EligibleDevice> eligible;
    std::vector<GpuCandidate> candidates;
    VkPhysicalDeviceFeatures selectedFeatures{};
    for (std::uint32_t deviceIndex = 0; deviceIndex < physicalCount; ++deviceIndex) {
        VkPhysicalDevice device = devices[deviceIndex];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        if (impl_->surface != VK_NULL_HANDLE && !hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(device, &features2);
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }

        const QueueSelection queues = selectQueues(device, impl_->surface);
        if (!queues.complete(impl_->surface != VK_NULL_HANDLE)) continue;
        GpuCandidate candidate;
        candidate.name = properties.deviceName;
        candidate.gpuClass = gpuClassOf(properties.deviceType);
        candidate.dedicatedMemoryBytes = deviceLocalBytes(device);
        candidate.apiIndex = deviceIndex;
        candidates.push_back(std::move(candidate));
        eligible.push_back({device, queues, properties, features2.features});
    }
    if (!eligible.empty()) {
        const GpuPreference preference = gpuPreferenceFromEnvironment();
        const GpuSelection selection = selectGpu(candidates, preference);
        logGpuSelection("Vulkan", candidates, selection, preference);
        const EligibleDevice& chosen = eligible[selection.chosen];
        impl_->physicalDevice = chosen.device;
        impl_->graphicsQueueFamily = chosen.queues.graphics;
        impl_->presentQueueFamily = chosen.queues.present;
        impl_->apiVersion = chosen.properties.apiVersion;
        std::snprintf(impl_->deviceName, sizeof(impl_->deviceName), "%s",
                      chosen.properties.deviceName);
        selectedFeatures = chosen.features;
    }
    if (impl_->physicalDevice == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceProperties selectedProperties{};
    vkGetPhysicalDeviceProperties(impl_->physicalDevice, &selectedProperties);
    impl_->samplerAnisotropy = selectedFeatures.samplerAnisotropy == VK_TRUE;
    impl_->depthClamp = selectedFeatures.depthClamp == VK_TRUE;
    impl_->independentBlend = selectedFeatures.independentBlend == VK_TRUE;
    impl_->maxSamplerAnisotropy = selectedProperties.limits.maxSamplerAnisotropy;

    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo{};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = impl_->graphicsQueueFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(graphicsQueueInfo);
    if (impl_->presentQueueFamily != impl_->graphicsQueueFamily) {
        VkDeviceQueueCreateInfo presentQueueInfo = graphicsQueueInfo;
        presentQueueInfo.queueFamilyIndex = impl_->presentQueueFamily;
        queueInfos.push_back(presentQueueInfo);
    }

    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.depthClamp = selectedFeatures.depthClamp;
    enabledFeatures.independentBlend = selectedFeatures.independentBlend;
    enabledFeatures.samplerAnisotropy = selectedFeatures.samplerAnisotropy;
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.dynamicRendering = VK_TRUE;
    enabled13.synchronization2 = VK_TRUE;
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabled13;
    deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = impl_->surface != VK_NULL_HANDLE ? 1u : 0u;
    deviceInfo.ppEnabledExtensionNames = impl_->surface != VK_NULL_HANDLE ? &swapchainExtension : nullptr;
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    if (vkCreateDevice(impl_->physicalDevice, &deviceInfo, nullptr, &impl_->device) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(impl_->device, impl_->graphicsQueueFamily, 0, &impl_->graphicsQueue);
    vkGetDeviceQueue(impl_->device, impl_->presentQueueFamily, 0, &impl_->presentQueue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = impl_->graphicsQueueFamily;
    if (vkCreateCommandPool(impl_->device, &poolInfo, nullptr, &impl_->commandPool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = impl_->commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = static_cast<std::uint32_t>(kFramesInFlight);
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
    if (vkAllocateCommandBuffers(impl_->device, &commandInfo, commandBuffers.data()) != VK_SUCCESS) {
        return false;
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::size_t frameIndex = 0; frameIndex < kFramesInFlight; ++frameIndex) {
        VkFrameSlot& frame = impl_->frames[frameIndex];
        frame.commandBuffer = commandBuffers[frameIndex];
        if (vkCreateFence(impl_->device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) {
            return false;
        }
        if (impl_->surface != VK_NULL_HANDLE &&
            vkCreateSemaphore(impl_->device, &semaphoreInfo, nullptr,
                              &frame.imageAvailable) != VK_SUCCESS) {
            return false;
        }
    }

    F4_LOG_INFO() << "Vulkan device: " << impl_->deviceName << " (API "
                  << VK_API_VERSION_MAJOR(impl_->apiVersion) << '.'
                  << VK_API_VERSION_MINOR(impl_->apiVersion) << ')';
    return true;
}

auto VulkanDevice::ready() const -> bool {
    return impl_ != nullptr && impl_->initialized;
}

auto VulkanDevice::backend() const -> Backend { return Backend::Vulkan; }

auto VulkanDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    const VkBufferUsageFlags usage = vkBufferUsage(desc.usage);
    if (!ready() || desc.sizeBytes == 0 || usage == 0 || initialData.size() > desc.sizeBytes) {
        return GfxError::BadDescriptor;
    }
    VkBufferRecord record;
    record.rec.sizeBytes = desc.sizeBytes;
    record.rec.usage = desc.usage;
    const GfxError allocationError = createBufferAllocation(
        impl_->physicalDevice, impl_->device, desc.sizeBytes, usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &record.buffer, &record.memory);
    if (allocationError != GfxError::None) return allocationError;
    if (!initialData.empty()) {
        void* mapped = nullptr;
        const VkResult mapResult = vkMapMemory(
            impl_->device, record.memory, 0, initialData.size(), 0, &mapped);
        if (mapResult != VK_SUCCESS) {
            destroyBufferAllocation(impl_->device, record.buffer, record.memory);
            return toGfxError(mapResult);
        }
        std::memcpy(mapped, initialData.data(), initialData.size());
        vkUnmapMemory(impl_->device, record.memory);
    }
    return impl_->buffers.create(std::move(record));
}

auto VulkanDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (!ready() || desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height)) {
        return GfxError::BadDescriptor;
    }
    const VkFormat format = vkTextureFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED) return GfxError::BadDescriptor;

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice, format, &formatProperties);
    VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    requiredFeatures |= isDepthFormat(desc.format)
                            ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if ((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
        return GfxError::BackendUnavailable;
    }

    VkTextureRecord record;
    record.rec = GpuTexture{desc.format, desc.width, desc.height, desc.mipLevels};
    record.layouts.resize(desc.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      (isDepthFormat(desc.format)
                           ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(impl_->device, &imageInfo, nullptr, &record.image);
    if (result != VK_SUCCESS) return toGfxError(result);

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(impl_->device, record.image, &requirements);
    std::uint32_t memoryTypeIndex = 0;
    if (!findMemoryType(impl_->physicalDevice, requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex)) {
        vkDestroyImage(impl_->device, record.image, nullptr);
        return GfxError::BackendUnavailable;
    }
    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryTypeIndex;
    result = vkAllocateMemory(impl_->device, &allocationInfo, nullptr, &record.memory);
    if (result == VK_SUCCESS) result = vkBindImageMemory(impl_->device, record.image, record.memory, 0);
    if (result != VK_SUCCESS) {
        if (record.memory != VK_NULL_HANDLE) vkFreeMemory(impl_->device, record.memory, nullptr);
        vkDestroyImage(impl_->device, record.image, nullptr);
        return toGfxError(result);
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = record.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = imageAspect(desc.format);
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(impl_->device, &viewInfo, nullptr, &record.view);
    if (result != VK_SUCCESS) {
        vkFreeMemory(impl_->device, record.memory, nullptr);
        vkDestroyImage(impl_->device, record.image, nullptr);
        return toGfxError(result);
    }
    return impl_->textures.create(std::move(record));
}

auto VulkanDevice::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    if (!ready() || !std::isfinite(desc.maxAnisotropy) || desc.maxAnisotropy < 1.0f) {
        return GfxError::BadDescriptor;
    }
    VkSamplerRecord record;
    record.rec = GpuSampler{desc.filter, desc.mipFilter, desc.addressU, desc.addressV,
                            desc.addressW, desc.maxAnisotropy};
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = magFilter(desc.filter);
    info.minFilter = minFilter(desc.filter);
    info.mipmapMode = mipFilter(desc.mipFilter);
    info.addressModeU = vkAddressMode(desc.addressU);
    info.addressModeV = vkAddressMode(desc.addressV);
    info.addressModeW = vkAddressMode(desc.addressW);
    info.anisotropyEnable = desc.maxAnisotropy > 1.0f && impl_->samplerAnisotropy
                                ? VK_TRUE
                                : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable == VK_TRUE
                             ? std::min(desc.maxAnisotropy, impl_->maxSamplerAnisotropy)
                             : 1.0f;
    info.minLod = 0.0f;
    info.maxLod = desc.mipFilter == MipFilter::None ? 0.0f : VK_LOD_CLAMP_NONE;
    const VkResult result = vkCreateSampler(impl_->device, &info, nullptr, &record.sampler);
    if (result != VK_SUCCESS) return toGfxError(result);
    return impl_->samplers.create(record);
}

auto VulkanDevice::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    constexpr std::uint32_t spirvMagic = 0x07230203u;
    if (!ready() || desc.blob.size() < sizeof(std::uint32_t) ||
        desc.blob.size() % sizeof(std::uint32_t) != 0) {
        ++impl_->stats.shaderFailed;
        return GfxError::BadDescriptor;
    }
    std::vector<std::uint32_t> code(desc.blob.size() / sizeof(std::uint32_t));
    std::memcpy(code.data(), desc.blob.data(), desc.blob.size());
    if (code[0] != spirvMagic) {
        ++impl_->stats.shaderFailed;
        return GfxError::BadDescriptor;
    }
    VkShaderRecord record;
    record.rec.stage = desc.stage;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = desc.blob.size();
    info.pCode = code.data();
    const VkResult result = vkCreateShaderModule(impl_->device, &info, nullptr, &record.module);
    if (result != VK_SUCCESS) {
        ++impl_->stats.shaderFailed;
        return toGfxError(result);
    }
    ++impl_->stats.shaderCompiled;
    return impl_->shaders.create(record);
}
auto VulkanDevice::createPipeline(const PipelineStateDesc& desc)
    -> Result<PipelineHandle, GfxError> {
    const bool compute = desc.computeShader.isValid();
    const bool hasVertex = desc.vertexShader.isValid();
    const bool hasFragment = desc.fragmentShader.isValid();
    if ((compute && (hasVertex || hasFragment)) ||
        (!compute && (!hasVertex || !hasFragment))) {
        return GfxError::BadDescriptor;
    }
    VkShaderRecord* vertex = compute ? nullptr : impl_->shaders.get(desc.vertexShader);
    VkShaderRecord* fragment = compute ? nullptr : impl_->shaders.get(desc.fragmentShader);
    VkShaderRecord* computeShader = compute ? impl_->shaders.get(desc.computeShader) : nullptr;
    if ((compute && computeShader == nullptr) ||
        (!compute && (vertex == nullptr || fragment == nullptr))) {
        return GfxError::InvalidHandle;
    }
    if ((compute && computeShader->rec.stage != ShaderStage::Compute) ||
        (!compute && (vertex->rec.stage != ShaderStage::Vertex ||
                      fragment->rec.stage != ShaderStage::Fragment))) {
        return GfxError::BadDescriptor;
    }
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(impl_->physicalDevice, &deviceProperties);
    if (desc.layout.pushConstantSizeBytes != 0) {
        return GfxError::BadDescriptor;
    }
    if (!compute && desc.raster.depthClampEnable && !impl_->depthClamp) {
        return GfxError::BackendUnavailable;
    }
    if (compute && (!desc.vertexInput.bindings.isEmpty() ||
                    !desc.vertexInput.attributes.isEmpty() ||
                    !desc.colorAttachments.isEmpty() ||
                    desc.depthStencil.usesAttachment())) {
        return GfxError::BadDescriptor;
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(desc.vertexInput.bindings.size());
    for (const VertexInputLayout::VertexBinding& binding : desc.vertexInput.bindings) {
        if (binding.strideBytes == 0 || binding.divisor > 1) return GfxError::BadDescriptor;
        if (std::any_of(bindings.begin(), bindings.end(), [&](const auto& existing) {
                return existing.binding == binding.binding;
            })) {
            return GfxError::BadDescriptor;
        }
        bindings.push_back({binding.binding, binding.strideBytes,
                            binding.divisor == 0 ? VK_VERTEX_INPUT_RATE_VERTEX
                                                 : VK_VERTEX_INPUT_RATE_INSTANCE});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertexInput.attributes.size());
    for (const VertexInputLayout::VertexAttribute& attribute : desc.vertexInput.attributes) {
        const auto binding = std::find_if(
            desc.vertexInput.bindings.begin(), desc.vertexInput.bindings.end(),
            [&](const auto& candidate) { return candidate.binding == attribute.binding; });
        if (binding == desc.vertexInput.bindings.end() ||
            attribute.offsetBytes + vertexFormatSize(attribute.format) > binding->strideBytes ||
            std::any_of(attributes.begin(), attributes.end(), [&](const auto& existing) {
                return existing.location == attribute.location;
            })) {
            return GfxError::BadDescriptor;
        }
        attributes.push_back(
            {attribute.location, attribute.binding, vkVertexFormat(attribute.format),
             attribute.offsetBytes});
    }

    const std::size_t colorCount = compute
                                       ? 0
                                       : (desc.colorAttachments.isEmpty()
                                              ? 1
                                              : desc.colorAttachments.size());
    if (colorCount > deviceProperties.limits.maxColorAttachments) {
        return GfxError::BadDescriptor;
    }
    if (colorCount > 1 && !impl_->independentBlend) return GfxError::BackendUnavailable;
    std::vector<VkPipelineColorBlendAttachmentState> colorStates(colorCount);
    for (std::size_t i = 0; i < colorCount; ++i) {
        const ColorAttachmentState state = desc.colorAttachments.isEmpty()
                                               ? ColorAttachmentState{}
                                               : desc.colorAttachments[i];
        if (isDepthFormat(state.format) || vkTextureFormat(state.format) == VK_FORMAT_UNDEFINED) {
            return GfxError::BadDescriptor;
        }
        colorStates[i].blendEnable = state.blendEnable ? VK_TRUE : VK_FALSE;
        colorStates[i].srcColorBlendFactor = vkBlendFactor(state.srcColor);
        colorStates[i].dstColorBlendFactor = vkBlendFactor(state.dstColor);
        colorStates[i].colorBlendOp = vkBlendOp(state.colorOp);
        colorStates[i].srcAlphaBlendFactor = vkBlendFactor(state.srcAlpha);
        colorStates[i].dstAlphaBlendFactor = vkBlendFactor(state.dstAlpha);
        colorStates[i].alphaBlendOp = vkBlendOp(state.alphaOp);
        colorStates[i].colorWriteMask = vkColorMask(state.writeMask);
    }

    VkPipelineRecord record;
    record.rec.desc = desc;
    record.compute = compute;
    std::vector<const BindGroup*> groups;
    if (!desc.layout.groups.isEmpty()) {
        std::uint32_t maxBindPoint = 0;
        for (const BindGroup& group : desc.layout.groups) {
            maxBindPoint = std::max(maxBindPoint, group.bindPoint);
        }
        if (maxBindPoint >= deviceProperties.limits.maxBoundDescriptorSets) {
            return GfxError::BadDescriptor;
        }
        groups.resize(static_cast<std::size_t>(maxBindPoint) + 1, nullptr);
        for (const BindGroup& group : desc.layout.groups) {
            if (groups[group.bindPoint] != nullptr) return GfxError::BadDescriptor;
            groups[group.bindPoint] = &group;
        }
    }

    auto destroySetLayouts = [&]() {
        for (VkDescriptorSetLayout setLayout : record.setLayouts) {
            vkDestroyDescriptorSetLayout(impl_->device, setLayout, nullptr);
        }
        record.setLayouts.clear();
    };
    const std::uint32_t allowedStageMask = compute
        ? shaderStageBit(ShaderStage::Compute)
        : shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment);
    const VkShaderStageFlags defaultShaderStages = compute
        ? VK_SHADER_STAGE_COMPUTE_BIT
        : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (const BindGroup* group : groups) {
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        if (group != nullptr) {
            layoutBindings.reserve(group->slots.size());
            for (const BindingSlot& slot : group->slots) {
                if (slot.count != 1 || slot.slot > (std::numeric_limits<std::uint32_t>::max() - 3u) / 4u ||
                    (slot.stageMask & ~allowedStageMask) != 0) {
                    destroySetLayouts();
                    return GfxError::BadDescriptor;
                }
                const std::uint32_t binding = bindingIndex(slot.type, slot.slot);
                if (std::any_of(layoutBindings.begin(), layoutBindings.end(),
                                [&](const auto& existing) {
                                    return existing.binding == binding;
                                })) {
                    destroySetLayouts();
                    return GfxError::BadDescriptor;
                }
                layoutBindings.push_back({binding, vkDescriptorType(slot.type), slot.count,
                                          vkShaderStages(slot.stageMask, defaultShaderStages),
                                          nullptr});
            }
        }
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = static_cast<std::uint32_t>(layoutBindings.size());
        setInfo.pBindings = layoutBindings.data();
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        const VkResult setResult =
            vkCreateDescriptorSetLayout(impl_->device, &setInfo, nullptr, &setLayout);
        if (setResult != VK_SUCCESS) {
            destroySetLayouts();
            return toGfxError(setResult);
        }
        record.setLayouts.push_back(setLayout);
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.setLayouts.size());
    layoutInfo.pSetLayouts = record.setLayouts.data();
    VkResult result = vkCreatePipelineLayout(impl_->device, &layoutInfo, nullptr, &record.layout);
    if (result != VK_SUCCESS) {
        destroySetLayouts();
        return toGfxError(result);
    }

    if (compute) {
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                              VK_SHADER_STAGE_COMPUTE_BIT, computeShader->module, "main", nullptr};
        pipelineInfo.layout = record.layout;
        result = vkCreateComputePipelines(
            impl_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &record.pipeline);
        if (result != VK_SUCCESS) {
            vkDestroyPipelineLayout(impl_->device, record.layout, nullptr);
            destroySetLayouts();
            return toGfxError(result);
        }
        return impl_->pipelines.create(std::move(record));
    }

    const VkPipelineShaderStageCreateInfo shaderStages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         vkShaderStage(vertex->rec.stage), vertex->module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         vkShaderStage(fragment->rec.stage), fragment->module, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = vkTopology(desc.topology);

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = desc.raster.depthClampEnable ? VK_TRUE : VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = vkCull(desc.raster.cullMode);
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depthStencil.depthTestEnable ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthStencil.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = vkCompare(desc.depthStencil.depthCompare);
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<std::uint32_t>(colorStates.size());
    blend.pAttachments = colorStates.data();
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    std::vector<VkFormat> colorFormats;
    colorFormats.reserve(colorCount);
    if (desc.colorAttachments.isEmpty()) {
        colorFormats.push_back(VK_FORMAT_R8G8B8A8_UNORM);
    } else {
        for (const ColorAttachmentState& state : desc.colorAttachments) {
            colorFormats.push_back(vkTextureFormat(state.format));
        }
    }
    record.colorFormats = colorFormats;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size());
    rendering.pColorAttachmentFormats = colorFormats.data();
    if (desc.depthStencil.usesAttachment()) {
        if (!isDepthFormat(desc.depthStencil.format)) {
            vkDestroyPipelineLayout(impl_->device, record.layout, nullptr);
            destroySetLayouts();
            return GfxError::BadDescriptor;
        }
        rendering.depthAttachmentFormat = vkTextureFormat(desc.depthStencil.format);
        if (desc.depthStencil.format == TextureFormat::Depth24Stencil8) {
            rendering.stencilAttachmentFormat = rendering.depthAttachmentFormat;
        }
        record.depthFormat = rendering.depthAttachmentFormat;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = record.layout;
    result = vkCreateGraphicsPipelines(
        impl_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &record.pipeline);
    if (result != VK_SUCCESS) {
        vkDestroyPipelineLayout(impl_->device, record.layout, nullptr);
        destroySetLayouts();
        return toGfxError(result);
    }
    return impl_->pipelines.create(std::move(record));
}
auto VulkanDevice::createSwapchain(const SwapchainDesc& desc) -> Result<SwapchainHandle, GfxError> {
    if (!ready() || impl_->surface == VK_NULL_HANDLE || desc.extent.width == 0 ||
        desc.extent.height == 0 || impl_->activeSwapchain.isValid()) {
        return GfxError::BadDescriptor;
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        impl_->physicalDevice, impl_->surface, &capabilities);
    if (result != VK_SUCCESS) return toGfxError(result);
    std::uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        impl_->physicalDevice, impl_->surface, &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount == 0) return GfxError::BackendUnavailable;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        impl_->physicalDevice, impl_->surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) return toGfxError(result);
    formats.resize(formatCount);

    const VkFormat requestedFormat = vkTextureFormat(desc.surfaceFormat);
    const auto surfaceFormat = std::find_if(formats.begin(), formats.end(), [&](const auto& format) {
        return format.format == requestedFormat &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (surfaceFormat == formats.end()) return GfxError::BackendUnavailable;

    std::uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        impl_->physicalDevice, impl_->surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS || presentModeCount == 0) return GfxError::BackendUnavailable;
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        impl_->physicalDevice, impl_->surface, &presentModeCount, presentModes.data());
    if (result != VK_SUCCESS) return toGfxError(result);
    presentModes.resize(presentModeCount);
    VkPresentModeKHR selectedPresentMode = vkPresentMode(desc.presentMode);
    if (std::find(presentModes.begin(), presentModes.end(), selectedPresentMode) ==
        presentModes.end()) {
        selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(desc.extent.width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(desc.extent.height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) imageCount = std::min(imageCount, capabilities.maxImageCount);
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
        constexpr VkCompositeAlphaFlagBitsKHR candidates[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
            if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
                compositeAlpha = candidate;
                break;
            }
        }
    }

    VkSwapchainRecord record;
    record.rec.surfaceFormat = desc.surfaceFormat;
    record.rec.extent = {extent.width, extent.height};
    record.rec.presentMode = selectedPresentMode == VK_PRESENT_MODE_FIFO_KHR
                                 ? PresentMode::Fifo
                                 : desc.presentMode;
    record.format = surfaceFormat->format;
    const std::uint32_t queueFamilies[] = {
        impl_->graphicsQueueFamily, impl_->presentQueueFamily};
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = impl_->surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat->format;
    info.imageColorSpace = surfaceFormat->colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (impl_->graphicsQueueFamily != impl_->presentQueueFamily) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = queueFamilies;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = compositeAlpha;
    info.presentMode = selectedPresentMode;
    info.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(impl_->device, &info, nullptr, &record.swapchain);
    if (result != VK_SUCCESS) return toGfxError(result);

    result = vkGetSwapchainImagesKHR(impl_->device, record.swapchain, &imageCount, nullptr);
    if (result == VK_SUCCESS) {
        record.images.resize(imageCount);
        result = vkGetSwapchainImagesKHR(
            impl_->device, record.swapchain, &imageCount, record.images.data());
    }
    if (result != VK_SUCCESS) {
        vkDestroySwapchainKHR(impl_->device, record.swapchain, nullptr);
        return toGfxError(result);
    }
    record.images.resize(imageCount);
    record.layouts.resize(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
    record.views.resize(imageCount, VK_NULL_HANDLE);
    record.renderComplete.resize(imageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = record.images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = record.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        result = vkCreateImageView(impl_->device, &viewInfo, nullptr, &record.views[i]);
        if (result == VK_SUCCESS) {
            result = vkCreateSemaphore(
                impl_->device, &semaphoreInfo, nullptr, &record.renderComplete[i]);
        }
        if (result != VK_SUCCESS) {
            for (VkSemaphore semaphore : record.renderComplete) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(impl_->device, semaphore, nullptr);
                }
            }
            for (VkImageView view : record.views) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(impl_->device, view, nullptr);
            }
            vkDestroySwapchainKHR(impl_->device, record.swapchain, nullptr);
            return toGfxError(result);
        }
    }
    const SwapchainHandle handle = impl_->swapchains.create(std::move(record));
    impl_->activeSwapchain = handle;
    return handle;
}
auto VulkanDevice::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (!ready() || desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.attachments.isEmpty()) {
        return GfxError::BadDescriptor;
    }
    std::size_t colorCount = 0;
    bool hasDepth = false;
    VkFramebufferRecord record;
    for (const AttachmentDesc& attachment : desc.attachments) {
        if (!attachment.texture.isValid()) {
            if (attachment.type != AttachmentType::Color || record.swapchain.isValid()) {
                return GfxError::BadDescriptor;
            }
            VkSwapchainRecord* swapchain = impl_->swapchains.get(impl_->activeSwapchain);
            if (swapchain == nullptr || swapchain->rec.extent.width != desc.extent.width ||
                swapchain->rec.extent.height != desc.extent.height) {
                return GfxError::BadDescriptor;
            }
            record.swapchain = impl_->activeSwapchain;
            record.colorFormats.push_back(swapchain->format);
            ++colorCount;
            continue;
        }
        VkTextureRecord* texture = impl_->textures.get(attachment.texture);
        if (texture == nullptr) return GfxError::InvalidHandle;
        if (texture->rec.width != desc.extent.width || texture->rec.height != desc.extent.height) {
            return GfxError::BadDescriptor;
        }
        if (attachment.type == AttachmentType::Depth) {
            if (hasDepth || !isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
            hasDepth = true;
        } else {
            if (isDepthFormat(texture->rec.format)) return GfxError::BadDescriptor;
            ++colorCount;
            record.colorFormats.push_back(vkTextureFormat(texture->rec.format));
        }
    }
    if (colorCount == 0) return GfxError::BadDescriptor;
    record.rec.extent = desc.extent;
    record.rec.attachments = desc.attachments;
    if (hasDepth) {
        for (const AttachmentDesc& attachment : desc.attachments) {
            if (attachment.type == AttachmentType::Depth) {
                record.depthFormat = vkTextureFormat(
                    impl_->textures.get(attachment.texture)->rec.format);
                break;
            }
        }
    }
    return impl_->framebuffers.create(std::move(record));
}

auto VulkanDevice::destroyBuffer(BufferHandle handle) -> GfxError {
    VkBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    VkBufferRecord retired = std::move(*record);
    if (!impl_->buffers.destroy(handle)) return GfxError::StaleHandle;
    if (impl_->lastSubmittedSerial > impl_->completedSerial) {
        impl_->retiredBuffers.push_back({std::move(retired), impl_->lastSubmittedSerial});
    } else {
        destroyBufferAllocation(impl_->device, retired.buffer, retired.memory);
    }
    return GfxError::None;
}
auto VulkanDevice::destroyTexture(TextureHandle handle) -> GfxError {
    VkTextureRecord* record = impl_->textures.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    VkTextureRecord retired = std::move(*record);
    if (!impl_->textures.destroy(handle)) return GfxError::StaleHandle;
    if (impl_->lastSubmittedSerial > impl_->completedSerial) {
        impl_->retiredTextures.push_back({std::move(retired), impl_->lastSubmittedSerial});
    } else {
        vkDestroyImageView(impl_->device, retired.view, nullptr);
        vkDestroyImage(impl_->device, retired.image, nullptr);
        vkFreeMemory(impl_->device, retired.memory, nullptr);
    }
    return GfxError::None;
}
auto VulkanDevice::destroySampler(SamplerHandle handle) -> GfxError {
    VkSamplerRecord* record = impl_->samplers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    VkSamplerRecord retired = std::move(*record);
    if (!impl_->samplers.destroy(handle)) return GfxError::StaleHandle;
    if (impl_->lastSubmittedSerial > impl_->completedSerial) {
        impl_->retiredSamplers.push_back({std::move(retired), impl_->lastSubmittedSerial});
    } else {
        vkDestroySampler(impl_->device, retired.sampler, nullptr);
    }
    return GfxError::None;
}
auto VulkanDevice::destroyShader(ShaderHandle handle) -> GfxError {
    VkShaderRecord* record = impl_->shaders.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    vkDestroyShaderModule(impl_->device, record->module, nullptr);
    return impl_->shaders.destroy(handle) ? GfxError::None : GfxError::StaleHandle;
}
auto VulkanDevice::destroyPipeline(PipelineHandle handle) -> GfxError {
    VkPipelineRecord* record = impl_->pipelines.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    VkPipelineRecord retired = std::move(*record);
    if (!impl_->pipelines.destroy(handle)) return GfxError::StaleHandle;
    if (impl_->lastSubmittedSerial > impl_->completedSerial) {
        impl_->retiredPipelines.push_back({std::move(retired), impl_->lastSubmittedSerial});
    } else {
        vkDestroyPipeline(impl_->device, retired.pipeline, nullptr);
        vkDestroyPipelineLayout(impl_->device, retired.layout, nullptr);
        for (VkDescriptorSetLayout setLayout : retired.setLayouts) {
            vkDestroyDescriptorSetLayout(impl_->device, setLayout, nullptr);
        }
    }
    return GfxError::None;
}
auto VulkanDevice::destroySwapchain(SwapchainHandle handle) -> GfxError {
    VkSwapchainRecord* record = impl_->swapchains.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    bool referenced = false;
    impl_->framebuffers.forEach([&](const VkFramebufferRecord* framebuffer) {
        if (framebuffer->swapchain == handle) referenced = true;
    });
    if (referenced) return GfxError::BadDescriptor;
    waitIdle();
    for (VkSemaphore semaphore : record->renderComplete) {
        vkDestroySemaphore(impl_->device, semaphore, nullptr);
    }
    for (VkImageView view : record->views) {
        vkDestroyImageView(impl_->device, view, nullptr);
    }
    vkDestroySwapchainKHR(impl_->device, record->swapchain, nullptr);
    if (impl_->activeSwapchain == handle) impl_->activeSwapchain = {};
    return impl_->swapchains.destroy(handle) ? GfxError::None : GfxError::StaleHandle;
}
auto VulkanDevice::destroyFramebuffer(FramebufferHandle handle) -> GfxError {
    if (impl_->framebuffers.get(handle) == nullptr) return GfxError::StaleHandle;
    return impl_->framebuffers.destroy(handle) ? GfxError::None : GfxError::StaleHandle;
}
auto VulkanDevice::writeBuffer(BufferHandle handle, std::uint64_t offset,
                               std::span<const std::byte> data) -> GfxError {
    VkBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (offset > record->rec.sizeBytes || data.size() > record->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    if (data.empty()) return GfxError::None;
    void* mapped = nullptr;
    const VkResult result = vkMapMemory(
        impl_->device, record->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) return toGfxError(result);
    std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());
    vkUnmapMemory(impl_->device, record->memory);
    return GfxError::None;
}

auto VulkanDevice::readBuffer(BufferHandle handle, std::uint64_t offset,
                              std::span<std::byte> destination) -> GfxError {
    VkBufferRecord* record = impl_->buffers.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (offset > record->rec.sizeBytes ||
        destination.size() > record->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    if (destination.empty()) return GfxError::None;
    waitIdle();
    void* mapped = nullptr;
    const VkResult result = vkMapMemory(
        impl_->device, record->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) return toGfxError(result);
    std::memcpy(destination.data(), static_cast<const std::byte*>(mapped) + offset,
                destination.size());
    vkUnmapMemory(impl_->device, record->memory);
    return GfxError::None;
}

auto VulkanDevice::writeTexture(TextureHandle handle, std::uint32_t mipLevel,
                                std::span<const std::byte> data) -> GfxError {
    VkTextureRecord* record = impl_->textures.get(handle);
    if (record == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= record->rec.mipLevels || isDepthFormat(record->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(1u, record->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, record->rec.height >> mipLevel);
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height *
                                   textureFormatBytes(record->rec.format);
    if (data.size() != expected) return GfxError::BadDescriptor;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    GfxError error = createBufferAllocation(
        impl_->physicalDevice, impl_->device, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &stagingBuffer, &stagingMemory);
    if (error != GfxError::None) return error;
    void* mapped = nullptr;
    VkResult result = vkMapMemory(impl_->device, stagingMemory, 0, data.size(), 0, &mapped);
    if (result != VK_SUCCESS) {
        destroyBufferAllocation(impl_->device, stagingBuffer, stagingMemory);
        return toGfxError(result);
    }
    std::memcpy(mapped, data.data(), data.size());
    vkUnmapMemory(impl_->device, stagingMemory);

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    error = beginOneTimeCommands(impl_->device, impl_->commandPool, &commandBuffer);
    if (error != GfxError::None) {
        destroyBufferAllocation(impl_->device, stagingBuffer, stagingMemory);
        return error;
    }
    VkImageMemoryBarrier2 toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = record->layouts[mipLevel] == VK_IMAGE_LAYOUT_UNDEFINED
                                  ? VK_PIPELINE_STAGE_2_NONE
                                  : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    toTransfer.srcAccessMask = record->layouts[mipLevel] == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? VK_ACCESS_2_NONE
                                   : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = record->layouts[mipLevel];
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = record->image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, record->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier2 toShaderRead = toTransfer;
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dependency.pImageMemoryBarriers = &toShaderRead;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    error = endOneTimeCommands(impl_->device, impl_->commandPool, impl_->graphicsQueue,
                               commandBuffer);
    destroyBufferAllocation(impl_->device, stagingBuffer, stagingMemory);
    if (error == GfxError::None) {
        record->layouts[mipLevel] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return error;
}
auto VulkanDevice::submit(CommandBuffer& commands) -> GfxError {
    if (!ready() || commands.isRecording()) return GfxError::BadDescriptor;
    VkFrameSlot& frame = impl_->frames[impl_->frameIndex];
    if (frame.pending) {
        const VkResult waitResult = vkWaitForFences(
            impl_->device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) return toGfxError(waitResult);
        frame.pending = false;
        impl_->completedSerial = std::max(impl_->completedSerial, frame.serial);
    }
    if (frame.descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(impl_->device, frame.descriptorPool, nullptr);
        frame.descriptorPool = VK_NULL_HANDLE;
    }
    collectRetired();

    std::array<std::uint64_t, 4> descriptorCapacity{};
    std::uint64_t descriptorSetCapacity = 0;
    VkPipelineRecord* scanPipeline = nullptr;
    auto accountDescriptorSets = [&](const VkPipelineRecord* pipeline) {
        if (pipeline == nullptr) return;
        descriptorSetCapacity += pipeline->setLayouts.size();
        for (const BindGroup& group : pipeline->rec.desc.layout.groups) {
            for (const BindingSlot& slot : group.slots) {
                descriptorCapacity[static_cast<std::size_t>(slot.type)] += slot.count;
            }
        }
    };
    for (const Command& command : commands.commands()) {
        if (command.kind == CommandKind::BindPipeline) {
            scanPipeline = impl_->pipelines.get(command.pipeline);
            if (scanPipeline == nullptr) return GfxError::InvalidHandle;
            accountDescriptorSets(scanPipeline);
        } else if (command.kind == CommandKind::BindResources && scanPipeline != nullptr) {
            accountDescriptorSets(scanPipeline);
        }
    }
    if (descriptorSetCapacity > std::numeric_limits<std::uint32_t>::max()) {
        return GfxError::OutOfMemory;
    }
    if (descriptorSetCapacity != 0) {
        std::vector<VkDescriptorPoolSize> poolSizes;
        for (std::size_t typeIndex = 0; typeIndex < descriptorCapacity.size(); ++typeIndex) {
            if (descriptorCapacity[typeIndex] == 0) continue;
            if (descriptorCapacity[typeIndex] > std::numeric_limits<std::uint32_t>::max()) {
                return GfxError::OutOfMemory;
            }
            poolSizes.push_back(
                {vkDescriptorType(static_cast<BindingType>(typeIndex)),
                 static_cast<std::uint32_t>(descriptorCapacity[typeIndex])});
        }
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<std::uint32_t>(descriptorSetCapacity);
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        const VkResult poolResult =
            vkCreateDescriptorPool(impl_->device, &poolInfo, nullptr, &frame.descriptorPool);
        if (poolResult != VK_SUCCESS) return toGfxError(poolResult);
    }

    VkSwapchainRecord* submissionSwapchain = nullptr;
    for (const Command& command : commands.commands()) {
        if (command.kind != CommandKind::BeginRenderPass) continue;
        VkFramebufferRecord* framebuffer = impl_->framebuffers.get(command.framebuffer);
        if (framebuffer == nullptr) return GfxError::InvalidHandle;
        if (!framebuffer->swapchain.isValid()) continue;
        VkSwapchainRecord* swapchain = impl_->swapchains.get(framebuffer->swapchain);
        if (swapchain == nullptr) return GfxError::InvalidHandle;
        if (submissionSwapchain != nullptr && submissionSwapchain != swapchain) {
            return GfxError::BadDescriptor;
        }
        submissionSwapchain = swapchain;
    }
    if (submissionSwapchain != nullptr) {
        const VkResult acquireResult = vkAcquireNextImageKHR(
            impl_->device, submissionSwapchain->swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &submissionSwapchain->imageIndex);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            return toGfxError(acquireResult);
        }
    }

    const VkCommandBuffer vkCommands = frame.commandBuffer;
    VkResult result = vkResetCommandBuffer(vkCommands, 0);
    if (result != VK_SUCCESS) return toGfxError(result);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(vkCommands, &beginInfo);
    if (result != VK_SUCCESS) return toGfxError(result);

    GfxError error = GfxError::None;
    for (const Command& command : commands.commands()) {
        if (command.kind != CommandKind::BindResources) continue;
        for (std::uint8_t resourceIndex = 0; resourceIndex < command.resourceCount;
             ++resourceIndex) {
            const BoundResource& resource = command.resources[resourceIndex];
            if (resource.type != BindingType::Texture) continue;
            TextureHandle handle;
            handle.index = resource.handleIndex;
            handle.generation = resource.handleGeneration;
            VkTextureRecord* texture = impl_->textures.get(handle);
            if (texture == nullptr) {
                error = GfxError::InvalidHandle;
                break;
            }
            transitionTexture(vkCommands, *texture, 0,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        if (error != GfxError::None) break;
    }

    bool rendering = false;
    bool indexBound = false;
    VkPipelineRecord* boundPipeline = nullptr;
    VkFramebufferRecord* activeFramebuffer = nullptr;
    std::vector<BoundResource> boundResources;
    auto findGroup = [](const VkPipelineRecord& pipeline,
                        std::uint32_t bindPoint) -> const BindGroup* {
        const auto& groups = pipeline.rec.desc.layout.groups;
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const BindGroup& group) {
            return group.bindPoint == bindPoint;
        });
        return found == groups.end() ? nullptr : &*found;
    };
    auto bindDescriptorSets = [&](VkPipelineRecord& pipeline) -> GfxError {
        if (pipeline.setLayouts.empty()) return GfxError::None;
        if (frame.descriptorPool == VK_NULL_HANDLE) return GfxError::BackendUnavailable;
        std::vector<VkDescriptorSet> sets(pipeline.setLayouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo allocationInfo{};
        allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocationInfo.descriptorPool = frame.descriptorPool;
        allocationInfo.descriptorSetCount = static_cast<std::uint32_t>(pipeline.setLayouts.size());
        allocationInfo.pSetLayouts = pipeline.setLayouts.data();
        const VkResult allocationResult =
            vkAllocateDescriptorSets(impl_->device, &allocationInfo, sets.data());
        if (allocationResult != VK_SUCCESS) return toGfxError(allocationResult);

        std::size_t bindingCount = 0;
        for (const BindGroup& group : pipeline.rec.desc.layout.groups) {
            bindingCount += group.slots.size();
        }
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;
        bufferInfos.reserve(bindingCount);
        imageInfos.reserve(bindingCount);
        writes.reserve(bindingCount);
        for (const BindGroup& group : pipeline.rec.desc.layout.groups) {
            for (const BindingSlot& slot : group.slots) {
                const auto bound = std::find_if(
                    boundResources.begin(), boundResources.end(), [&](const BoundResource& resource) {
                        return resource.bindPoint == group.bindPoint && resource.slot == slot.slot &&
                               resource.type == slot.type;
                    });
                if (bound == boundResources.end()) continue;
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = sets[group.bindPoint];
                write.dstBinding = bindingIndex(slot.type, slot.slot);
                write.descriptorCount = 1;
                write.descriptorType = vkDescriptorType(slot.type);
                if (slot.type == BindingType::Texture) {
                    TextureHandle handle;
                    handle.index = bound->handleIndex;
                    handle.generation = bound->handleGeneration;
                    VkTextureRecord* texture = impl_->textures.get(handle);
                    if (texture == nullptr) return GfxError::InvalidHandle;
                    imageInfos.push_back(
                        {VK_NULL_HANDLE, texture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                    write.pImageInfo = &imageInfos.back();
                } else if (slot.type == BindingType::Sampler) {
                    SamplerHandle handle;
                    handle.index = bound->handleIndex;
                    handle.generation = bound->handleGeneration;
                    VkSamplerRecord* sampler = impl_->samplers.get(handle);
                    if (sampler == nullptr) return GfxError::InvalidHandle;
                    imageInfos.push_back({sampler->sampler, VK_NULL_HANDLE,
                                          VK_IMAGE_LAYOUT_UNDEFINED});
                    write.pImageInfo = &imageInfos.back();
                } else {
                    BufferHandle handle;
                    handle.index = bound->handleIndex;
                    handle.generation = bound->handleGeneration;
                    VkBufferRecord* buffer = impl_->buffers.get(handle);
                    if (buffer == nullptr) return GfxError::InvalidHandle;
                    const BufferUsage required = slot.type == BindingType::UniformBuffer
                                                     ? BufferUsage::Uniform
                                                     : BufferUsage::Storage;
                    if (!hasUsage(buffer->rec.usage, required)) return GfxError::BadDescriptor;
                    bufferInfos.push_back({buffer->buffer, 0, buffer->rec.sizeBytes});
                    write.pBufferInfo = &bufferInfos.back();
                }
                writes.push_back(write);
            }
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(impl_->device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
        vkCmdBindDescriptorSets(vkCommands,
                                pipeline.compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                 : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout, 0, static_cast<std::uint32_t>(sets.size()),
                                sets.data(), 0, nullptr);
        return GfxError::None;
    };
    DeviceStats delta;
    if (error == GfxError::None) for (const Command& command : commands.commands()) {
        switch (command.kind) {
        case CommandKind::ResourceBarrier:
            if (rendering || command.after.stages == PipelineStageMask::None) {
                error = GfxError::BadDescriptor;
                break;
            }
            if (command.barrierType == BarrierResourceType::Texture) {
                VkTextureRecord* texture = impl_->textures.get(command.texture);
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
                const VkImageLayout newLayout = vkTextureLayout(command.after.layout);
                std::vector<VkImageMemoryBarrier2> barriers(texture->layouts.size());
                for (std::size_t mipLevel = 0; mipLevel < texture->layouts.size(); ++mipLevel) {
                    VkImageMemoryBarrier2& barrier = barriers[mipLevel];
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    barrier.srcStageMask = vkPipelineStages(command.before.stages);
                    barrier.srcAccessMask = vkAccess(command.before.access);
                    barrier.dstStageMask = vkPipelineStages(command.after.stages);
                    barrier.dstAccessMask = vkAccess(command.after.access);
                    barrier.oldLayout = texture->layouts[mipLevel];
                    barrier.newLayout = newLayout;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = texture->image;
                    barrier.subresourceRange = {
                        imageAspect(texture->rec.format), static_cast<std::uint32_t>(mipLevel),
                        1, 0, 1};
                }
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
                dependency.pImageMemoryBarriers = barriers.data();
                vkCmdPipelineBarrier2(vkCommands, &dependency);
                std::fill(texture->layouts.begin(), texture->layouts.end(), newLayout);
            } else {
                VkBufferRecord* buffer = impl_->buffers.get(command.buffer);
                if (buffer == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                if (command.before.layout != TextureLayout::Undefined ||
                    command.after.layout != TextureLayout::Undefined) {
                    error = GfxError::BadDescriptor;
                    break;
                }
                VkBufferMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = vkPipelineStages(command.before.stages);
                barrier.srcAccessMask = vkAccess(command.before.access);
                barrier.dstStageMask = vkPipelineStages(command.after.stages);
                barrier.dstAccessMask = vkAccess(command.after.access);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = buffer->buffer;
                barrier.offset = 0;
                barrier.size = VK_WHOLE_SIZE;
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(vkCommands, &dependency);
            }
            break;
        case CommandKind::BeginRenderPass: {
            activeFramebuffer = impl_->framebuffers.get(command.framebuffer);
            if (rendering || activeFramebuffer == nullptr) {
                error = GfxError::InvalidHandle;
                break;
            }
            std::vector<VkRenderingAttachmentInfo> colors;
            colors.reserve(activeFramebuffer->colorFormats.size());
            VkRenderingAttachmentInfo depth{};
            bool hasDepth = false;
            for (const AttachmentDesc& attachment : activeFramebuffer->rec.attachments) {
                if (!attachment.texture.isValid()) {
                    if (submissionSwapchain == nullptr ||
                        activeFramebuffer->swapchain != impl_->activeSwapchain) {
                        error = GfxError::InvalidHandle;
                        break;
                    }
                    const std::uint32_t imageIndex = submissionSwapchain->imageIndex;
                    transitionSwapchainImage(
                        vkCommands, submissionSwapchain->images[imageIndex],
                        &submissionSwapchain->layouts[imageIndex],
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    VkRenderingAttachmentInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    info.imageView = submissionSwapchain->views[imageIndex];
                    info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    info.loadOp = attachment.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                         : VK_ATTACHMENT_LOAD_OP_LOAD;
                    info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    std::memcpy(info.clearValue.color.float32, attachment.clear.color,
                                sizeof(attachment.clear.color));
                    colors.push_back(info);
                    continue;
                }
                VkTextureRecord* texture = impl_->textures.get(attachment.texture);
                if (texture == nullptr) {
                    error = GfxError::InvalidHandle;
                    break;
                }
                VkRenderingAttachmentInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                info.imageView = texture->view;
                info.loadOp = attachment.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (attachment.type == AttachmentType::Color) {
                    transitionTexture(vkCommands, *texture, 0,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    std::memcpy(info.clearValue.color.float32, attachment.clear.color,
                                sizeof(attachment.clear.color));
                    colors.push_back(info);
                } else {
                    transitionTexture(vkCommands, *texture, 0,
                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
                    info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    info.clearValue.depthStencil = {attachment.clear.depth,
                                                    attachment.clear.stencil};
                    depth = info;
                    hasDepth = true;
                }
            }
            if (error != GfxError::None) break;
            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea.extent = {activeFramebuffer->rec.extent.width,
                                               activeFramebuffer->rec.extent.height};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            renderingInfo.pColorAttachments = colors.data();
            renderingInfo.pDepthAttachment = hasDepth ? &depth : nullptr;
            renderingInfo.pStencilAttachment =
                hasDepth && activeFramebuffer->depthFormat == VK_FORMAT_D24_UNORM_S8_UINT
                    ? &depth
                    : nullptr;
            vkCmdBeginRendering(vkCommands, &renderingInfo);
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = static_cast<float>(activeFramebuffer->rec.extent.height);
            viewport.width = static_cast<float>(activeFramebuffer->rec.extent.width);
            viewport.height = -static_cast<float>(activeFramebuffer->rec.extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(vkCommands, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, {activeFramebuffer->rec.extent.width,
                                      activeFramebuffer->rec.extent.height}};
            vkCmdSetScissor(vkCommands, 0, 1, &scissor);
            rendering = true;
            indexBound = false;
            boundPipeline = nullptr;
            break;
        }
        case CommandKind::EndRenderPass:
            if (!rendering) {
                error = GfxError::BadDescriptor;
            } else {
                vkCmdEndRendering(vkCommands);
                if (activeFramebuffer != nullptr && activeFramebuffer->swapchain.isValid()) {
                    const std::uint32_t imageIndex = submissionSwapchain->imageIndex;
                    transitionSwapchainImage(
                        vkCommands, submissionSwapchain->images[imageIndex],
                        &submissionSwapchain->layouts[imageIndex],
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT);
                }
                rendering = false;
                activeFramebuffer = nullptr;
            }
            break;
        case CommandKind::BindPipeline:
            boundPipeline = impl_->pipelines.get(command.pipeline);
            if (boundPipeline == nullptr) {
                error = GfxError::InvalidHandle;
            } else if (rendering && boundPipeline->compute) {
                error = GfxError::BadDescriptor;
            } else if (rendering &&
                       (activeFramebuffer == nullptr ||
                        boundPipeline->colorFormats != activeFramebuffer->colorFormats ||
                        (boundPipeline->depthFormat != VK_FORMAT_UNDEFINED &&
                         boundPipeline->depthFormat != activeFramebuffer->depthFormat))) {
                error = GfxError::BadDescriptor;
            } else {
                vkCmdBindPipeline(vkCommands,
                                  boundPipeline->compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                         : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  boundPipeline->pipeline);
                error = bindDescriptorSets(*boundPipeline);
            }
            break;
        case CommandKind::BindVertexBuffer: {
            VkBufferRecord* buffer = impl_->buffers.get(command.buffer);
            if (buffer == nullptr || !hasUsage(buffer->rec.usage, BufferUsage::Vertex)) {
                error = GfxError::InvalidHandle;
            } else {
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(vkCommands, command.vertexSlot, 1,
                                       &buffer->buffer, &offset);
            }
            break;
        }
        case CommandKind::BindIndexBuffer: {
            VkBufferRecord* buffer = impl_->buffers.get(command.buffer);
            if (buffer == nullptr || !hasUsage(buffer->rec.usage, BufferUsage::Index)) {
                error = GfxError::InvalidHandle;
            } else {
                vkCmdBindIndexBuffer(vkCommands, buffer->buffer, 0,
                                     command.indexFormat == IndexFormat::U16
                                         ? VK_INDEX_TYPE_UINT16
                                         : VK_INDEX_TYPE_UINT32);
                indexBound = true;
            }
            break;
        }
        case CommandKind::BindResources:
            for (std::uint8_t resourceIndex = 0; resourceIndex < command.resourceCount;
                 ++resourceIndex) {
                const BoundResource& resource = command.resources[resourceIndex];
                if (boundPipeline != nullptr) {
                    const BindGroup* group = findGroup(*boundPipeline, resource.bindPoint);
                    if (group == nullptr ||
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
                    boundResources.begin(), boundResources.end(), [&](const BoundResource& bound) {
                        return bound.bindPoint == resource.bindPoint &&
                               bound.slot == resource.slot && bound.type == resource.type;
                    });
                if (existing == boundResources.end()) {
                    boundResources.push_back(resource);
                } else {
                    *existing = resource;
                }
            }
            if (error == GfxError::None && boundPipeline != nullptr) {
                error = bindDescriptorSets(*boundPipeline);
            }
            break;
        case CommandKind::SetViewport: {
            if (command.viewport.width == 0 || command.viewport.height == 0) {
                error = GfxError::BadDescriptor;
                break;
            }
            VkViewport viewport{};
            // Negative height keeps NDC +Y at the rectangle's first row.
            viewport.x = static_cast<float>(command.viewportX);
            viewport.y = static_cast<float>(command.viewportY) +
                         static_cast<float>(command.viewport.height);
            viewport.width = static_cast<float>(command.viewport.width);
            viewport.height = -static_cast<float>(command.viewport.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(vkCommands, 0, 1, &viewport);
            break;
        }
        case CommandKind::SetScissor: {
            if (command.scissor.width == 0 || command.scissor.height == 0) {
                error = GfxError::BadDescriptor;
                break;
            }
            VkRect2D scissor{{static_cast<std::int32_t>(command.scissorX),
                              static_cast<std::int32_t>(command.scissorY)},
                             {command.scissor.width, command.scissor.height}};
            vkCmdSetScissor(vkCommands, 0, 1, &scissor);
            break;
        }
        case CommandKind::Draw:
            if (!rendering || boundPipeline == nullptr || boundPipeline->compute) {
                error = GfxError::BadDescriptor;
            } else {
                vkCmdDraw(vkCommands, command.vertexCount, command.instanceCount, 0, 0);
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
            if (!rendering || boundPipeline == nullptr || boundPipeline->compute ||
                !indexBound) {
                error = GfxError::BadDescriptor;
            } else {
                vkCmdDrawIndexed(vkCommands, command.indexCount, command.instanceCount,
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
        case CommandKind::Dispatch:
            if (rendering || boundPipeline == nullptr || !boundPipeline->compute ||
                command.groupCountX == 0 || command.groupCountY == 0 ||
                command.groupCountZ == 0) {
                error = GfxError::BadDescriptor;
            } else {
                vkCmdDispatch(vkCommands, command.groupCountX, command.groupCountY,
                              command.groupCountZ);
                ++delta.dispatchCalls;
            }
            break;
        }
        if (error != GfxError::None) break;
    }
    if (rendering) {
        vkCmdEndRendering(vkCommands);
        if (error == GfxError::None) error = GfxError::BadDescriptor;
    }
    result = vkEndCommandBuffer(vkCommands);
    if (error != GfxError::None) return error;
    if (result != VK_SUCCESS) return toGfxError(result);

    result = vkResetFences(impl_->device, 1, &frame.fence);
    if (result != VK_SUCCESS) return toGfxError(result);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkCommands;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore renderCompleteSignal = VK_NULL_HANDLE;
    if (submissionSwapchain != nullptr) {
        renderCompleteSignal =
            submissionSwapchain->renderComplete[submissionSwapchain->imageIndex];
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderCompleteSignal;
    }
    result = vkQueueSubmit(impl_->graphicsQueue, 1, &submitInfo, frame.fence);
    if (result != VK_SUCCESS) return toGfxError(result);
    frame.pending = true;
    frame.serial = ++impl_->lastSubmittedSerial;
    impl_->frameIndex = (impl_->frameIndex + 1) % kFramesInFlight;
    if (submissionSwapchain != nullptr) {
        const VkSemaphore renderComplete =
            submissionSwapchain->renderComplete[submissionSwapchain->imageIndex];
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderComplete;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &submissionSwapchain->swapchain;
        presentInfo.pImageIndices = &submissionSwapchain->imageIndex;
        result = vkQueuePresentKHR(impl_->presentQueue, &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return toGfxError(result);
    }
    impl_->stats.drawCalls += delta.drawCalls;
    impl_->stats.drawIndexedCalls += delta.drawIndexedCalls;
    impl_->stats.dispatchCalls += delta.dispatchCalls;
    impl_->stats.batches += delta.batches;
    impl_->stats.instances += delta.instances;
    impl_->stats.primitives += delta.primitives;
    impl_->stats.vertices += delta.vertices;
    return GfxError::None;
}

auto VulkanDevice::waitIdle() -> void {
    if (impl_ != nullptr && impl_->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(impl_->device);
        for (VkFrameSlot& frame : impl_->frames) {
            frame.pending = false;
            if (frame.descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(impl_->device, frame.descriptorPool, nullptr);
                frame.descriptorPool = VK_NULL_HANDLE;
            }
        }
        impl_->completedSerial = impl_->lastSubmittedSerial;
        collectRetired();
    }
}

auto VulkanDevice::readTexture(TextureHandle handle, std::uint32_t mipLevel,
                               std::span<std::byte> destination) -> GfxError {
    VkTextureRecord* texture = impl_->textures.get(handle);
    if (texture == nullptr) return GfxError::StaleHandle;
    if (mipLevel >= texture->rec.mipLevels || isDepthFormat(texture->rec.format)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t width = std::max(1u, texture->rec.width >> mipLevel);
    const std::uint32_t height = std::max(1u, texture->rec.height >> mipLevel);
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height *
                                   textureFormatBytes(texture->rec.format);
    if (destination.size() != expected) return GfxError::BadDescriptor;
    waitIdle();

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    GfxError error = createBufferAllocation(
        impl_->physicalDevice, impl_->device, destination.size(),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &stagingBuffer, &stagingMemory);
    if (error != GfxError::None) return error;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    error = beginOneTimeCommands(impl_->device, impl_->commandPool, &commandBuffer);
    if (error != GfxError::None) {
        destroyBufferAllocation(impl_->device, stagingBuffer, stagingMemory);
        return error;
    }
    transitionTexture(commandBuffer, *texture, mipLevel, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &copy);
    transitionTexture(commandBuffer, *texture, mipLevel,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    error = endOneTimeCommands(impl_->device, impl_->commandPool, impl_->graphicsQueue,
                               commandBuffer);
    if (error == GfxError::None) {
        void* mapped = nullptr;
        const VkResult mapResult = vkMapMemory(
            impl_->device, stagingMemory, 0, destination.size(), 0, &mapped);
        if (mapResult == VK_SUCCESS) {
            std::memcpy(destination.data(), mapped, destination.size());
            vkUnmapMemory(impl_->device, stagingMemory);
        } else {
            error = toGfxError(mapResult);
        }
    }
    destroyBufferAllocation(impl_->device, stagingBuffer, stagingMemory);
    return error;
}

auto VulkanDevice::stats() const -> DeviceStats {
    if (impl_ == nullptr) return {};
    DeviceStats stats = impl_->stats;
    stats.bufferCount = impl_->buffers.liveCount();
    impl_->buffers.forEach([&](const VkBufferRecord* buffer) {
        stats.bufferBytes += buffer->rec.sizeBytes;
    });
    stats.textureCount = impl_->textures.liveCount();
    impl_->textures.forEach([&](const VkTextureRecord* texture) {
        stats.textureBytes += textureBytes(texture->rec);
    });
    stats.samplerCount = impl_->samplers.liveCount();
    stats.shaderCount = impl_->shaders.liveCount();
    stats.pipelineCount = impl_->pipelines.liveCount();
    stats.framebufferCount = impl_->framebuffers.liveCount();
    stats.swapchainCount = impl_->swapchains.liveCount();
    return stats;
}
auto VulkanDevice::resetFrameStats() -> void {
    if (impl_ != nullptr) impl_->stats = DeviceStats{};
}
auto VulkanDevice::deviceName() const -> const char* { return impl_ != nullptr ? impl_->deviceName : ""; }
auto VulkanDevice::apiVersion() const -> std::uint32_t { return impl_ != nullptr ? impl_->apiVersion : 0; }
auto VulkanDevice::graphicsQueueFamily() const -> std::uint32_t {
    return impl_ != nullptr ? impl_->graphicsQueueFamily : kInvalidQueueFamily;
}
auto VulkanDevice::presentQueueFamily() const -> std::uint32_t {
    return impl_ != nullptr ? impl_->presentQueueFamily : kInvalidQueueFamily;
}
auto VulkanDevice::validationEnabled() const -> bool {
    return impl_ != nullptr && impl_->validationEnabled;
}
auto VulkanDevice::synchronizationValidationEnabled() const -> bool {
    return impl_ != nullptr && impl_->synchronizationValidationEnabled;
}
auto VulkanDevice::validationWarningCount() const -> std::uint64_t {
    return impl_ != nullptr ? impl_->validation.warnings.load() : 0;
}
auto VulkanDevice::validationErrorCount() const -> std::uint64_t {
    return impl_ != nullptr ? impl_->validation.errors.load() : 0;
}
auto VulkanDevice::hasSurface() const -> bool {
    return impl_ != nullptr && impl_->surface != VK_NULL_HANDLE;
}
auto VulkanDevice::preferredSurfaceFormat() const -> TextureFormat {
    if (!ready() || impl_->surface == VK_NULL_HANDLE) return TextureFormat::RGBA8_UNORM;
    std::uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            impl_->physicalDevice, impl_->surface, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return TextureFormat::RGBA8_UNORM;
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            impl_->physicalDevice, impl_->surface, &count, formats.data()) != VK_SUCCESS) {
        return TextureFormat::RGBA8_UNORM;
    }
    formats.resize(count);
    constexpr TextureFormat candidates[] = {
        TextureFormat::RGBA8_UNORM, TextureFormat::BGRA8_UNORM};
    for (TextureFormat candidate : candidates) {
        const VkFormat candidateFormat = vkTextureFormat(candidate);
        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) return candidate;
        if (std::find_if(formats.begin(), formats.end(), [&](const auto& available) {
                return available.format == candidateFormat &&
                       available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            }) != formats.end()) {
            return candidate;
        }
    }
    return TextureFormat::RGBA8_UNORM;
}
auto VulkanDevice::framesInFlight() const -> std::uint32_t {
    return static_cast<std::uint32_t>(kFramesInFlight);
}

auto createVulkanDevice(platform::Window* window) -> IDevice* {
    auto* device = new VulkanDevice(window);
    if (!device->ready()) {
        delete device;
        return nullptr;
    }
    return device;
}

} // namespace f4::gfx
