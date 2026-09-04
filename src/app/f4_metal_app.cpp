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
#include <utility>
#include <vector>

#include "backends/metal/metal_device.h"
#include "graphics/command/command_buffer.h"
#include "platform/window.h"

using namespace f4;

namespace {

struct RenderTargets {
    gfx::SwapchainHandle swapchain;
    gfx::FramebufferHandle presentFramebuffer;
    gfx::TextureHandle captureTexture;
    gfx::FramebufferHandle captureFramebuffer;
    gfx::Extent extent;
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

void destroyTargets(gfx::MetalDevice& device, RenderTargets& targets) {
    device.waitIdle();
    if (targets.captureFramebuffer.isValid()) {
        (void)device.destroyFramebuffer(targets.captureFramebuffer);
    }
    if (targets.captureTexture.isValid()) (void)device.destroyTexture(targets.captureTexture);
    if (targets.presentFramebuffer.isValid()) {
        (void)device.destroyFramebuffer(targets.presentFramebuffer);
    }
    if (targets.swapchain.isValid()) (void)device.destroySwapchain(targets.swapchain);
    targets = {};
}

auto createTargets(gfx::MetalDevice& device, gfx::Extent extent, RenderTargets& targets)
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
        (void)device.destroySwapchain(swapchain.value());
        return false;
    }
    gfx::TextureDesc captureDesc;
    captureDesc.format = gfx::TextureFormat::BGRA8_UNORM;
    captureDesc.width = extent.width;
    captureDesc.height = extent.height;
    auto captureTexture = device.createTexture(captureDesc);
    if (!captureTexture.hasValue()) {
        (void)device.destroyFramebuffer(presentFramebuffer.value());
        (void)device.destroySwapchain(swapchain.value());
        return false;
    }
    gfx::FramebufferDesc captureFramebufferDesc = presentDesc;
    captureFramebufferDesc.attachments[0].texture = captureTexture.value();
    auto captureFramebuffer = device.createFramebuffer(captureFramebufferDesc);
    if (!captureFramebuffer.hasValue()) {
        (void)device.destroyTexture(captureTexture.value());
        (void)device.destroyFramebuffer(presentFramebuffer.value());
        (void)device.destroySwapchain(swapchain.value());
        return false;
    }
    targets = {swapchain.value(), presentFramebuffer.value(), captureTexture.value(),
               captureFramebuffer.value(), extent};
    return true;
}

auto writeCapture(gfx::MetalDevice& device, const RenderTargets& targets,
                  const std::string& path) -> bool {
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(targets.extent.width) * targets.extent.height * 4u);
    if (device.readTexture(targets.captureTexture, 0, pixels) != gfx::GfxError::None) {
        return false;
    }
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        std::swap(pixels[offset], pixels[offset + 2]);
    }
    return stbi_write_png(path.c_str(), static_cast<int>(targets.extent.width),
                          static_cast<int>(targets.extent.height), 4, pixels.data(),
                          static_cast<int>(targets.extent.width * 4u)) != 0;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t smokeFrames = 0;
    std::string capturePath;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--smoke") == 0) {
            smokeFrames = 3;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                const int count = std::atoi(argv[++index]);
                if (count <= 0) return 2;
                smokeFrames = static_cast<std::size_t>(count);
            }
        } else if (std::strcmp(argv[index], "--capture") == 0 && index + 1 < argc) {
            capturePath = argv[++index];
        } else {
            std::fprintf(stderr, "Usage: f4_metal_app [--smoke frames] [--capture path]\n");
            return 2;
        }
    }

#if defined(F4_GRAPHICS_BACKEND_metal)
    gfx::IDevice* selected = gfx::createDevice();
    if (selected == nullptr || selected->backend() != gfx::Backend::Metal) {
        gfx::deleteDevice(selected);
        return 1;
    }
    gfx::deleteDevice(selected);
#endif

    auto windowResult = platform::Window::createMetal({"f4 native Metal M17", 960, 640, true});
    if (!windowResult.hasValue()) return 1;
    platform::Window* window = windowResult.value();
    gfx::IDevice* base = gfx::createMetalDevice(window);
    if (base == nullptr) {
        delete window;
        return 1;
    }
    auto* device = static_cast<gfx::MetalDevice*>(base);
    auto vertex = device->createShader(
        loadShader(F4_METAL_APP_SHADER, gfx::ShaderStage::Vertex));
    auto fragment = device->createShader(
        loadShader(F4_METAL_APP_SHADER, gfx::ShaderStage::Fragment));
    if (!vertex.hasValue() || !fragment.hasValue()) {
        gfx::deleteDevice(device);
        delete window;
        return 1;
    }
    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex.value();
    pipelineDesc.fragmentShader = fragment.value();
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    gfx::ColorAttachmentState color;
    color.format = gfx::TextureFormat::BGRA8_UNORM;
    pipelineDesc.colorAttachments.push(color);
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
        commands.begin();
        commands.beginRenderPass(targets.presentFramebuffer);
        commands.bindPipeline(pipeline.value());
        commands.draw(3);
        commands.endRenderPass();
        commands.beginRenderPass(targets.captureFramebuffer);
        commands.bindPipeline(pipeline.value());
        commands.draw(3);
        commands.endRenderPass();
        commands.end();
        if (device->submit(commands) != gfx::GfxError::None) break;
        ++frame;
        if (!capturePath.empty() && !captured) {
            captured = writeCapture(*device, targets, capturePath);
            if (!captured) break;
        }
        if (smokeFrames != 0 && frame >= smokeFrames) running = false;
    }

    device->waitIdle();
    const bool valid = device->validationErrorCount() == 0;
    destroyTargets(*device, targets);
    (void)device->destroyPipeline(pipeline.value());
    (void)device->destroyShader(fragment.value());
    (void)device->destroyShader(vertex.value());
    gfx::deleteDevice(device);
    delete window;
    return valid && (capturePath.empty() || captured) ? 0 : 1;
}
