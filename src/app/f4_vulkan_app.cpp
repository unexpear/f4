#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "backends/vulkan/vulkan_device.h"
#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/framebuffer/swapchain_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/texture/texture_desc.h"
#include "platform/window.h"

using namespace f4;

namespace {

struct RenderTargets {
    gfx::SwapchainHandle swapchain;
    gfx::FramebufferHandle presentFramebuffer;
    gfx::TextureHandle captureTexture;
    gfx::FramebufferHandle captureFramebuffer;
    gfx::Extent extent;
    gfx::TextureFormat format = gfx::TextureFormat::RGBA8_UNORM;
};

auto loadShader(const char* path, gfx::ShaderStage stage) -> gfx::ShaderDesc {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    gfx::ShaderDesc desc;
    desc.stage = stage;
    if (!stream.good()) return desc;
    const std::streamsize size = stream.tellg();
    if (size <= 0) return desc;
    stream.seekg(0);
    desc.blob.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(desc.blob.data()), size);
    if (!stream.good()) desc.blob.clear();
    return desc;
}

auto destroyTargets(gfx::VulkanDevice& device, RenderTargets& targets) -> void {
    device.waitIdle();
    if (targets.captureFramebuffer.isValid()) {
        device.destroyFramebuffer(targets.captureFramebuffer);
    }
    if (targets.captureTexture.isValid()) device.destroyTexture(targets.captureTexture);
    if (targets.presentFramebuffer.isValid()) {
        device.destroyFramebuffer(targets.presentFramebuffer);
    }
    if (targets.swapchain.isValid()) device.destroySwapchain(targets.swapchain);
    targets = {};
}

auto createTargets(gfx::VulkanDevice& device, gfx::Extent extent, RenderTargets& targets)
    -> bool {
    gfx::SwapchainDesc swapchainDesc;
    swapchainDesc.surfaceFormat = device.preferredSurfaceFormat();
    swapchainDesc.extent = extent;
    auto swapchain = device.createSwapchain(swapchainDesc);
    if (!swapchain.hasValue()) return false;

    gfx::FramebufferDesc presentDesc;
    presentDesc.extent = extent;
    gfx::AttachmentDesc presentColor;
    presentColor.clearOnLoad = true;
    presentColor.clear.color[0] = 0.018f;
    presentColor.clear.color[1] = 0.028f;
    presentColor.clear.color[2] = 0.055f;
    presentColor.clear.color[3] = 1.0f;
    presentDesc.attachments.push(presentColor);
    auto presentFramebuffer = device.createFramebuffer(presentDesc);
    if (!presentFramebuffer.hasValue()) {
        device.destroySwapchain(swapchain.value());
        return false;
    }

    gfx::TextureDesc captureTextureDesc;
    captureTextureDesc.format = swapchainDesc.surfaceFormat;
    captureTextureDesc.width = extent.width;
    captureTextureDesc.height = extent.height;
    auto captureTexture = device.createTexture(captureTextureDesc);
    if (!captureTexture.hasValue()) {
        device.destroyFramebuffer(presentFramebuffer.value());
        device.destroySwapchain(swapchain.value());
        return false;
    }

    gfx::FramebufferDesc captureDesc = presentDesc;
    captureDesc.attachments[0].texture = captureTexture.value();
    auto captureFramebuffer = device.createFramebuffer(captureDesc);
    if (!captureFramebuffer.hasValue()) {
        device.destroyTexture(captureTexture.value());
        device.destroyFramebuffer(presentFramebuffer.value());
        device.destroySwapchain(swapchain.value());
        return false;
    }

    targets = {swapchain.value(), presentFramebuffer.value(), captureTexture.value(),
               captureFramebuffer.value(), extent, swapchainDesc.surfaceFormat};
    return true;
}

auto recordFrame(const RenderTargets& targets, gfx::PipelineHandle pipeline,
                 gfx::CommandBuffer& commands) -> bool {
    commands.begin();
    commands.beginRenderPass(targets.presentFramebuffer);
    commands.bindPipeline(pipeline);
    commands.draw(3);
    commands.endRenderPass();
    commands.beginRenderPass(targets.captureFramebuffer);
    commands.bindPipeline(pipeline);
    commands.draw(3);
    commands.endRenderPass();
    commands.end();
    return true;
}

auto writeCapture(gfx::VulkanDevice& device, const RenderTargets& targets,
                  const std::string& path) -> bool {
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(targets.extent.width) * targets.extent.height * 4u);
    if (device.readTexture(targets.captureTexture, 0, pixels) != gfx::GfxError::None) {
        return false;
    }
    if (targets.format == gfx::TextureFormat::BGRA8_UNORM) {
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
            const std::byte red = pixels[offset];
            pixels[offset] = pixels[offset + 2];
            pixels[offset + 2] = red;
        }
    }
    return stbi_write_png(path.c_str(), static_cast<int>(targets.extent.width),
                          static_cast<int>(targets.extent.height), 4, pixels.data(),
                          static_cast<int>(targets.extent.width * 4u)) != 0;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t smokeFrames = 0;
    std::string capturePath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smokeFrames = 3;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const int count = std::atoi(argv[++i]);
                if (count <= 0) return 2;
                smokeFrames = static_cast<std::size_t>(count);
            }
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: f4_vulkan_app [--smoke frames] [--capture path]\n");
            return 2;
        }
    }

    auto windowResult = platform::Window::createVulkan(
        {"f4 Vulkan M9", 960, 640, true});
    if (!windowResult.hasValue()) return 1;
    platform::Window* window = windowResult.value();
    gfx::IDevice* base = gfx::createVulkanDevice(window);
    if (base == nullptr) {
        delete window;
        return 1;
    }
    auto* device = static_cast<gfx::VulkanDevice*>(base);

    auto vertex = device->createShader(
        loadShader(F4_VK_APP_VERTEX_SPV, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_VK_APP_FRAGMENT_SPV, gfx::ShaderStage::Fragment));
    if (!vertex.hasValue() || !fragment.hasValue()) {
        gfx::deleteDevice(device);
        delete window;
        return 1;
    }
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::ColorAttachmentState colorState;
    colorState.format = device->preferredSurfaceFormat();
    pipelineDesc.colorAttachments.push(colorState);
    auto pipeline = device->createPipeline(pipelineDesc);
    if (!pipeline.hasValue()) {
        gfx::deleteDevice(device);
        delete window;
        return 1;
    }

    RenderTargets targets;
    if (!createTargets(*device, {window->width(), window->height()}, targets)) {
        gfx::deleteDevice(device);
        delete window;
        return 1;
    }

    bool running = true;
    bool rebuild = false;
    bool captured = false;
    std::size_t frame = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                rebuild = true;
            }
        }
        if (!running) break;
        if (rebuild) {
            const gfx::Extent extent{window->width(), window->height()};
            destroyTargets(*device, targets);
            if (!createTargets(*device, extent, targets)) break;
            rebuild = false;
        }

        gfx::CommandBuffer commands;
        if (!recordFrame(targets, pipeline.value(), commands)) break;
        const gfx::GfxError submitError = device->submit(commands);
        if (submitError == gfx::GfxError::BackendUnavailable) {
            rebuild = true;
            continue;
        }
        if (submitError != gfx::GfxError::None) break;
        ++frame;

        if (!capturePath.empty() && !captured) {
            device->waitIdle();
            captured = writeCapture(*device, targets, capturePath);
            if (!captured) break;
        }
        if (smokeFrames != 0 && frame >= smokeFrames) running = false;
    }

    device->waitIdle();
    const bool valid = device->validationErrorCount() == 0;
    destroyTargets(*device, targets);
    device->destroyPipeline(pipeline.value());
    device->destroyShader(fragment.value());
    device->destroyShader(vertex.value());
    gfx::deleteDevice(device);
    delete window;
    return valid && (capturePath.empty() || captured) ? 0 : 1;
}
