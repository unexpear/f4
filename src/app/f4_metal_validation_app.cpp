#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "backends/metal/metal_device.h"
#include "demos/demo_cornell_box.h"
#include "demos/demo_cube.h"
#include "demos/demo_render_validation.h"
#include "demos/demo_terrain.h"
#include "graphics/command/command_buffer.h"

using namespace f4;

namespace {

constexpr gfx::Extent kCaptureExtent{640, 360};
constexpr std::size_t kCaptureCount = 3;

struct Targets {
    gfx::TextureHandle color;
    gfx::TextureHandle depth;
    gfx::FramebufferHandle framebuffer;
};

auto createTargets(gfx::IDevice& device) -> Targets {
    Targets targets;
    gfx::TextureDesc colorDesc;
    colorDesc.width = kCaptureExtent.width;
    colorDesc.height = kCaptureExtent.height;
    const auto color = device.createTexture(colorDesc);
    if (!color) return targets;
    targets.color = color.value();

    gfx::TextureDesc depthDesc;
    depthDesc.width = kCaptureExtent.width;
    depthDesc.height = kCaptureExtent.height;
    depthDesc.format = gfx::TextureFormat::Depth32F;
    const auto depth = device.createTexture(depthDesc);
    if (!depth) return targets;
    targets.depth = depth.value();

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = kCaptureExtent;
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.texture = targets.color;
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.025f;
    colorAttachment.clear.color[1] = 0.055f;
    colorAttachment.clear.color[2] = 0.045f;
    colorAttachment.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = targets.depth;
    depthAttachment.clearOnLoad = true;
    depthAttachment.clear.depth = 1.0f;
    framebufferDesc.attachments.push(depthAttachment);
    const auto framebuffer = device.createFramebuffer(framebufferDesc);
    if (framebuffer) targets.framebuffer = framebuffer.value();
    return targets;
}

void destroyTargets(gfx::IDevice& device, Targets& targets) {
    if (targets.framebuffer.isValid()) (void)device.destroyFramebuffer(targets.framebuffer);
    if (targets.depth.isValid()) (void)device.destroyTexture(targets.depth);
    if (targets.color.isValid()) (void)device.destroyTexture(targets.color);
    targets = {};
}

auto safeName(const char* name) -> std::string {
    std::string result = name != nullptr ? name : "scene";
    for (char& character : result) {
        if (!std::isalnum(static_cast<unsigned char>(character))) character = '_';
    }
    return result;
}

auto saveCapture(const std::filesystem::path& path, std::vector<std::byte>& pixels) -> bool {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels.data(), static_cast<int>(kCaptureExtent.width),
        static_cast<int>(kCaptureExtent.height), 32,
        static_cast<int>(kCaptureExtent.width * 4), SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) return false;
    const int result = SDL_SaveBMP(surface, path.string().c_str());
    SDL_FreeSurface(surface);
    return result == 0;
}

auto digest(std::span<const std::byte> bytes) -> std::uint32_t {
    std::uint32_t value = 2166136261u;
    for (const std::byte byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 16777619u;
    }
    return value;
}

auto changedPixelCount(std::span<const std::byte> pixels) -> std::size_t {
    constexpr std::array<int, 3> clear{6, 14, 11};
    std::size_t changed = 0;
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        bool differs = false;
        for (std::size_t channel = 0; channel < clear.size(); ++channel) {
            const int value = std::to_integer<std::uint8_t>(pixels[offset + channel]);
            differs = differs || std::abs(value - clear[channel]) > 3;
        }
        if (differs) ++changed;
    }
    return changed;
}

auto renderScene(gfx::MetalDevice& device, f4::demo::IDemoScene& scene,
                 std::size_t sceneIndex, const std::filesystem::path& outputDirectory)
    -> bool {
    if (!scene.init(&device, nullptr)) {
        std::fprintf(stderr, "Metal scene initialization failed: %s\n", scene.name());
        scene.shutdown(&device);
        return false;
    }
    Targets targets = createTargets(device);
    if (!targets.framebuffer.isValid()) {
        std::fprintf(stderr, "Metal capture target creation failed: %s\n", scene.name());
        scene.shutdown(&device);
        destroyTargets(device, targets);
        return false;
    }

    f4::demo::FreeCamera camera;
    std::vector<std::byte> firstCapture;
    const std::size_t byteCount = static_cast<std::size_t>(kCaptureExtent.width) *
                                  kCaptureExtent.height * 4;
    bool valid = true;
    for (std::size_t frame = 0; frame < kCaptureCount && valid; ++frame) {
        scene.onResize(kCaptureExtent.width, kCaptureExtent.height);
        scene.update(0.0f, nullptr, nullptr);
        device.resetFrameStats();
        gfx::CommandBuffer commands;
        commands.begin();
        if (scene.recordsOwnPasses()) {
            scene.renderPasses(commands, camera, kCaptureExtent.width,
                               kCaptureExtent.height, targets.framebuffer);
        } else {
            commands.beginRenderPass(targets.framebuffer);
            commands.setViewport(kCaptureExtent);
            commands.setScissor(kCaptureExtent);
            scene.render(commands, camera, kCaptureExtent.width, kCaptureExtent.height);
            commands.endRenderPass();
        }
        commands.end();
        if (device.submit(commands) != gfx::GfxError::None) {
            std::fprintf(stderr, "Metal scene submission failed: %s\n", scene.name());
            valid = false;
            break;
        }
        const gfx::DeviceStats stats = device.stats();
        if (stats.drawCalls + stats.drawIndexedCalls == 0) {
            std::fprintf(stderr, "Metal scene recorded no draws: %s\n", scene.name());
            valid = false;
            break;
        }

        std::vector<std::byte> pixels(byteCount);
        if (device.readTexture(targets.color, 0, pixels) != gfx::GfxError::None ||
            changedPixelCount(pixels) < 100) {
            std::fprintf(stderr, "Metal scene produced an empty capture: %s\n", scene.name());
            valid = false;
            break;
        }
        if (frame == 0) firstCapture = pixels;
        else if (pixels != firstCapture) {
            std::fprintf(stderr, "Metal scene capture changed between frames: %s\n", scene.name());
            valid = false;
            break;
        }

        char fileName[320]{};
        std::snprintf(fileName, sizeof(fileName), "%02zu_%s_%03zu.bmp", sceneIndex,
                      safeName(scene.name()).c_str(), frame + 1);
        if (!saveCapture(outputDirectory / fileName, pixels)) {
            std::fprintf(stderr, "Metal capture write failed: %s\n", SDL_GetError());
            valid = false;
            break;
        }
        std::printf("Metal capture %s frame %zu: pixels=%zu hash=%08x\n",
                    scene.name(), frame + 1, changedPixelCount(pixels), digest(pixels));
    }

    scene.shutdown(&device);
    destroyTargets(device, targets);
    device.waitIdle();
    return valid && device.validationErrorCount() == 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* captureDirectory = "m17-metal-captures";
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--capture-dir") != 0 || index + 1 >= argc) {
            std::fprintf(stderr, "Usage: f4_metal_validation_app [--capture-dir path]\n");
            return 2;
        }
        captureDirectory = argv[++index];
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(captureDirectory, filesystemError);
    if (filesystemError) {
        std::fprintf(stderr, "Cannot create Metal capture directory: %s\n",
                     filesystemError.message().c_str());
        return 1;
    }
    SDL_SetMainReady();
    if (SDL_Init(0) != 0) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }

    gfx::IDevice* base = gfx::createMetalDevice();
    if (base == nullptr) {
        std::fprintf(stderr, "No native Metal device is available\n");
        SDL_Quit();
        return 1;
    }
    auto* device = static_cast<gfx::MetalDevice*>(base);

    f4::demo::PbrSphereGridScene pbr;
    f4::demo::MipmappingScene mipmapping;
    f4::demo::ZFightingScene zFighting;
    f4::demo::AlphaOrderScene alphaOrder;
    f4::demo::NormalMapOrientationScene normalMapping;
    f4::demo::HybridRenderingScene hybrid;
    f4::demo::CornellBoxScene cornell;
    f4::demo::CubeScene cube;
    f4::demo::TerrainScene terrain;
    const std::array<f4::demo::IDemoScene*, 9> scenes{
        &cube, &cornell, &pbr, &mipmapping, &zFighting,
        &alphaOrder, &normalMapping, &hybrid, &terrain};

    bool valid = true;
    for (std::size_t index = 0; index < scenes.size() && valid; ++index) {
        valid = renderScene(*device, *scenes[index], index,
                            std::filesystem::path(captureDirectory));
    }
    if (device->validationWarningCount() != 0 || device->validationErrorCount() != 0) {
        std::fprintf(stderr, "Metal diagnostics: warnings=%llu errors=%llu\n",
                     static_cast<unsigned long long>(device->validationWarningCount()),
                     static_cast<unsigned long long>(device->validationErrorCount()));
        valid = false;
    }
    gfx::deleteDevice(device);
    SDL_Quit();
    return valid ? 0 : 1;
}
