#include "engine/rendering/shader_constants.h"

#include <algorithm>

#include "graphics/buffer/buffer_desc.h"

namespace f4::engine {

auto ConstantRing::create(gfx::IDevice& device, std::uint32_t strideBytes,
                          std::uint32_t capacity) -> gfx::GfxError {
    if (valid()) {
        return device_ == &device && strideBytes_ == strideBytes && this->capacity() >= capacity
                   ? gfx::GfxError::None
                   : gfx::GfxError::BadDescriptor;
    }
    if (capacity == 0 || strideBytes == 0) return gfx::GfxError::BadDescriptor;
    device_ = &device;
    strideBytes_ = strideBytes;
    const gfx::GfxError error = grow(device, capacity);
    if (error != gfx::GfxError::None) {
        destroy(device);
        return error;
    }
    cursor_ = 0;
    return gfx::GfxError::None;
}

void ConstantRing::destroy(gfx::IDevice& device) {
    for (auto& frameHandles : handles_) {
        for (const gfx::BufferHandle handle : frameHandles) {
            if (handle.isValid()) device.destroyBuffer(handle);
        }
        frameHandles.clear();
    }
    strideBytes_ = 0;
    currentFrame_ = kBufferedFrames - 1;
    cursor_ = 0;
    allocationsThisFrame_ = 0;
    device_ = nullptr;
}

auto ConstantRing::alloc(gfx::IDevice& device, std::span<const std::byte> data) -> Slot {
    if (!valid() || device_ != &device || data.size() > strideBytes_) return {};
    if (allocationsThisFrame_ >= handles_[currentFrame_].size()) {
        if (grow(device, allocationsThisFrame_ + 1) != gfx::GfxError::None) return {};
    }
    const std::size_t i = cursor_;
    if (device.writeBuffer(handles_[currentFrame_][i], 0, data) != gfx::GfxError::None) return {};
    ++cursor_;
    ++allocationsThisFrame_;
    return Slot{handles_[currentFrame_][i], true};
}

auto ConstantRing::grow(gfx::IDevice& device, std::size_t minimumCapacity) -> gfx::GfxError {
    const std::size_t oldSize = handles_[0].size();
    const std::size_t targetCapacity = std::max(minimumCapacity, std::max<std::size_t>(1, oldSize * 2));
    for (auto& frameHandles : handles_) frameHandles.reserve(targetCapacity);

    gfx::BufferDesc desc;
    desc.sizeBytes = strideBytes_;
    desc.usage = gfx::BufferUsage::Uniform;
    for (auto& frameHandles : handles_) {
        while (frameHandles.size() < targetCapacity) {
            auto result = device.createBuffer(desc, {});
            if (!result.hasValue()) {
                for (auto& rollbackHandles : handles_) {
                    while (rollbackHandles.size() > oldSize) {
                        (void)device.destroyBuffer(rollbackHandles.back());
                        rollbackHandles.pop();
                    }
                }
                return result.error();
            }
            frameHandles.push(result.value());
        }
    }
    return gfx::GfxError::None;
}

void ConstantRing::bind(gfx::CommandBuffer& cb, const Slot& slot) {
    if (!slot.valid) return;
    cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
}

auto ConstantRing::writeAndBind(gfx::IDevice& device, gfx::CommandBuffer& cb,
                                std::span<const std::byte> data) -> Slot {
    Slot slot = alloc(device, data);
    bind(cb, slot);
    return slot;
}

} // namespace f4::engine
