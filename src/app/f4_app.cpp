// f4_app — windowed OpenGL demo (M7). Manual smoke test: opens an SDL2 window,
// creates a GL 4.3 context + OpenGLDevice, draws a rotating colored cube, and
// presents. Grown in later M7 steps to a hierarchy of cubes.
//
// NOT part of the headless test gate (a window/display is required).

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include "backends/opengl/opengl_device.h"
#include "core/assert.h"
#include "core/math/mat4.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
#include "platform/gl_context.h"
#include "platform/window.h"

using namespace f4;
using namespace f4::gfx;
using namespace f4::math;

namespace {

constexpr const char* kVertexShader = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

layout(std140, binding = 0) uniform UBO {
    mat4 mvp;
};

out vec3 vColor;

void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vColor = aColor;
}
)glsl";

constexpr const char* kFragmentShader = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;

void main() {
    fragColor = vec4(vColor, 1.0);
}
)glsl";

void fillBlob(Array<std::byte>& blob, const std::string& text) {
    blob.resize(text.size());
    std::memcpy(blob.data(), text.data(), text.size());
}

struct Vertex {
    float x, y, z;
    float r, g, b;
};

} // namespace

int main() {
    auto w = platform::Window::create({"f4", 800, 600, true});
    if (!w.hasValue()) {
        return 1;
    }
    platform::Window* window = w.value();
    window->makeCurrent();

    auto glInfo = platform::initGl(4, 3);
    if (!glInfo.hasValue()) {
        return 1;
    }

    IDevice* dev = createOpenGLDevice();
    if (dev == nullptr) {
        return 1;
    }

    // Cube: 24 vertices (4 per face, unique colors), 36 indices.
    //       Face colors: R G B C M Y
    constexpr float S = 0.5f;
    // clang-format off
    constexpr std::array<Vertex, 24> kCube = {{
        // Front (red)
        {-S, -S,  S, 1, 0, 0}, { S, -S,  S, 1, 0, 0},
        { S,  S,  S, 1, 0, 0}, {-S,  S,  S, 1, 0, 0},
        // Back (green)
        { S, -S, -S, 0, 1, 0}, {-S, -S, -S, 0, 1, 0},
        {-S,  S, -S, 0, 1, 0}, { S,  S, -S, 0, 1, 0},
        // Right (blue)
        { S, -S,  S, 0, 0, 1}, { S, -S, -S, 0, 0, 1},
        { S,  S, -S, 0, 0, 1}, { S,  S,  S, 0, 0, 1},
        // Left (cyan)
        {-S, -S, -S, 0, 1, 1}, {-S, -S,  S, 0, 1, 1},
        {-S,  S,  S, 0, 1, 1}, {-S,  S, -S, 0, 1, 1},
        // Top (magenta)
        {-S,  S,  S, 1, 0, 1}, { S,  S,  S, 1, 0, 1},
        { S,  S, -S, 1, 0, 1}, {-S,  S, -S, 1, 0, 1},
        // Bottom (yellow)
        {-S, -S, -S, 1, 1, 0}, { S, -S, -S, 1, 1, 0},
        { S, -S,  S, 1, 1, 0}, {-S, -S,  S, 1, 1, 0},
    }};
    // clang-format on
    constexpr std::array<std::uint16_t, 36> kIndices = {{
        0,  1,  2,  0,  2,  3,   // front
        4,  5,  6,  4,  6,  7,   // back
        8,  9,  10, 8,  10, 11,  // right
        12, 13, 14, 12, 14, 15,  // left
        16, 17, 18, 16, 18, 19,  // top
        20, 21, 22, 20, 22, 23,  // bottom
    }};

    BufferHandle vb{};
    {
        auto* bytes = reinterpret_cast<const std::byte*>(kCube.data());
        BufferDesc vbd;
        vbd.sizeBytes = kCube.size() * sizeof(Vertex);
        vbd.usage = BufferUsage::Vertex;
        vb = dev->createBuffer(vbd, std::span(bytes, vbd.sizeBytes)).value();
    }

    BufferHandle ib{};
    {
        auto* bytes = reinterpret_cast<const std::byte*>(kIndices.data());
        BufferDesc ibd;
        ibd.sizeBytes = kIndices.size() * sizeof(std::uint16_t);
        ibd.usage = BufferUsage::Index;
        ib = dev->createBuffer(ibd, std::span(bytes, ibd.sizeBytes)).value();
    }

    // Uniform buffer for the MVP matrix (16 floats = 64 bytes, std140 aligned).
    BufferHandle ubo{};
    {
        BufferDesc ubd;
        ubd.sizeBytes = sizeof(Mat4);
        ubd.usage = BufferUsage::Uniform;
        ubo = dev->createBuffer(ubd, {}).value();
    }

    // Shaders.
    ShaderDesc vsd;
    vsd.stage = ShaderStage::Vertex;
    fillBlob(vsd.blob, kVertexShader);
    auto vs = dev->createShader(vsd);
    if (!vs.hasValue()) {
        return 1;
    }
    ShaderDesc fsd;
    fsd.stage = ShaderStage::Fragment;
    fillBlob(fsd.blob, kFragmentShader);
    auto fs = dev->createShader(fsd);
    if (!fs.hasValue()) {
        return 1;
    }

    // Pipeline: two bindings (position + color), depth enabled.
    PipelineStateDesc pd;
    pd.vertexShader = vs.value();
    pd.fragmentShader = fs.value();
    pd.topology = PrimitiveTopology::TriangleList;
    pd.raster.cullMode = CullMode::Back;
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = true;
    pd.depthStencil.depthCompare = CompareOp::Less;

    VertexInputLayout::VertexBinding binding;
    binding.binding = 0;
    binding.strideBytes = sizeof(Vertex);
    pd.vertexInput.bindings.push(binding);

    VertexInputLayout::VertexAttribute posAttr;
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VertexFormat::Float32x3;
    posAttr.offsetBytes = 0;
    pd.vertexInput.attributes.push(posAttr);

    VertexInputLayout::VertexAttribute colorAttr;
    colorAttr.binding = 0;
    colorAttr.location = 1;
    colorAttr.format = VertexFormat::Float32x3;
    colorAttr.offsetBytes = 3 * sizeof(float);
    pd.vertexInput.attributes.push(colorAttr);

    auto pipeline = dev->createPipeline(pd);
    if (!pipeline.hasValue()) {
        return 1;
    }

    // Framebuffer: color + depth.
    FramebufferDesc fbDesc;
    fbDesc.extent = {800, 600};
    AttachmentDesc colorAtt;
    colorAtt.type = AttachmentType::Color;
    colorAtt.clearOnLoad = true;
    colorAtt.clear.color[0] = 0.05f;
    colorAtt.clear.color[1] = 0.05f;
    colorAtt.clear.color[2] = 0.1f;
    fbDesc.attachments.push(colorAtt);
    AttachmentDesc depthAtt;
    depthAtt.type = AttachmentType::Depth;
    depthAtt.clearOnLoad = true;
    depthAtt.clear.depth = 1.0f;
    fbDesc.attachments.push(depthAtt);
    auto fb = dev->createFramebuffer(fbDesc);
    if (!fb.hasValue()) {
        return 1;
    }

    // Main loop.
    bool running = true;
    const Uint32 startTime = SDL_GetTicks();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        const float t = static_cast<float>(SDL_GetTicks() - startTime) * 0.001f;

        // MVP: perspective projection, camera at (2,2,2) looking at origin.
        const Mat4 proj =
            Mat4::perspectiveRH_ZO(1.0472f, 800.0f / 600.0f, 0.1f, 100.0f); // 60 deg
        const Mat4 view =
            Mat4::lookAtRH(Vec3(2.0f, 2.0f, 2.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f));
        const Mat4 model =
            Mat4::rotationY(t * 0.7f).mul(Mat4::rotationX(t * 0.3f));
        const Mat4 mvp = proj.mul(view.mul(model));

        dev->writeBuffer(ubo, 0,
                         std::span(reinterpret_cast<const std::byte*>(&mvp), sizeof(mvp)));

        CommandBuffer cb;
        cb.begin();
        cb.beginRenderPass(fb.value());
        cb.bindPipeline(pipeline.value());
        cb.bindVertexBuffer(0, vb, sizeof(Vertex));
        cb.bindIndexBuffer(ib, IndexFormat::U16);
        cb.bindResource(0, ubo.index, ubo.generation);
        cb.drawIndexed(36);
        cb.endRenderPass();
        cb.end();

        dev->submit(cb);
        window->swap();
    }

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
    return 0;
}
