#include "doctest/doctest.h"

#include <array>
#include <cmath>
#include <limits>

#include "backends/null/null_device.h"
#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "core/resources/mesh.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/light.h"
#include "engine/rendering/gpu_mesh.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/render_queue.h"
#include "engine/rendering/renderer.h"
#include "engine/rendering/shader_constants.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "random.hpp"

#ifdef F4_GRAPHICS_HAS_OPENGL
#include <epoxy/gl.h>
#include "backends/opengl/opengl_device.h"
#include "platform/gl_context.h"
#include "platform/window.h"
#endif

using namespace f4;

TEST_CASE("M8: constant ring grows past its initial per-frame capacity") {
    gfx::IDevice* device = gfx::createDevice();
    REQUIRE(device != nullptr);
    engine::ConstantRing ring;
    REQUIRE(ring.create(*device, 16, 2) == gfx::GfxError::None);
    const std::byte data[16]{};

    ring.beginFrame();
    engine::ConstantRing::Slot firstFrameSlot;
    for (int i = 0; i < 130; ++i) {
        const auto slot = ring.alloc(*device, data);
        CHECK(slot.valid);
        if (i == 0) firstFrameSlot = slot;
    }
    CHECK(ring.capacity() >= 130);

    ring.beginFrame();
    const auto secondFrameSlot = ring.alloc(*device, data);
    REQUIRE(secondFrameSlot.valid);
    CHECK(secondFrameSlot.handle != firstFrameSlot.handle);
    ring.beginFrame();
    const auto thirdFrameSlot = ring.alloc(*device, data);
    REQUIRE(thirdFrameSlot.valid);
    CHECK(thirdFrameSlot.handle != firstFrameSlot.handle);
    CHECK(thirdFrameSlot.handle != secondFrameSlot.handle);
    ring.beginFrame();
    const auto reusedFrameSlot = ring.alloc(*device, data);
    REQUIRE(reusedFrameSlot.valid);
    CHECK(reusedFrameSlot.handle == firstFrameSlot.handle);
    CHECK(ring.create(*device, 32, 2) == gfx::GfxError::BadDescriptor);
    gfx::NullDevice otherDevice;
    CHECK_FALSE(ring.alloc(otherDevice, data).valid);
    ring.destroy(*device);
    gfx::deleteDevice(device);
}

TEST_CASE("M8: shader float parity uses calibrated absolute and relative tolerance") {
    const std::array<float, 4> reference{{0.0f, 1.0f, 100.0f, -100.0f}};
    const std::array<float, 4> withinTolerance{{5.0e-6f, 1.00005f, 100.005f, -100.005f}};
    const engine::ShaderFloatParityReport passing =
        engine::compareShaderFloats(reference, withinTolerance);
    CHECK(passing.matches());
    CHECK(passing.sampleCount == reference.size());
    CHECK(passing.mismatchCount == 0);
    CHECK(passing.maxAbsoluteError > 0.0f);

    const std::array<float, 4> outsideTolerance{{0.0f, 1.01f, 100.0f, -100.0f}};
    const engine::ShaderFloatParityReport failing =
        engine::compareShaderFloats(reference, outsideTolerance);
    CHECK_FALSE(failing.matches());
    CHECK(failing.status == engine::ShaderParityStatus::OutsideTolerance);
    CHECK(failing.mismatchCount == 1);
    CHECK(failing.worstSample == 1);
}

TEST_CASE("M8: shader parity rejects malformed and non-finite captures") {
    const std::array<float, 2> finite{{1.0f, 2.0f}};
    const std::array<float, 1> shortCapture{{1.0f}};
    CHECK(engine::compareShaderFloats(finite, shortCapture).status ==
          engine::ShaderParityStatus::SizeMismatch);
    CHECK(engine::compareShaderFloats(finite, finite, {-1.0f, 0.0f}).status ==
          engine::ShaderParityStatus::InvalidTolerance);

    const std::array<float, 2> nonFinite{{1.0f, std::numeric_limits<float>::quiet_NaN()}};
    const engine::ShaderFloatParityReport report =
        engine::compareShaderFloats(finite, nonFinite);
    CHECK(report.status == engine::ShaderParityStatus::NonFinite);
    CHECK(report.mismatchCount == 1);
    CHECK(report.worstSample == 1);
}

TEST_CASE("M8: shader RGBA8 parity is exact and reports pixels") {
    const std::array<std::uint8_t, 8> reference{{1, 2, 3, 255, 10, 20, 30, 255}};
    CHECK(engine::compareShaderImagesRgba8(reference, reference).matches());

    const std::array<std::uint8_t, 8> changed{{1, 2, 3, 255, 11, 18, 30, 255}};
    const engine::ShaderImageParityReport report =
        engine::compareShaderImagesRgba8(reference, changed);
    CHECK_FALSE(report.matches());
    CHECK(report.pixelCount == 2);
    CHECK(report.mismatchPixelCount == 1);
    CHECK(report.firstMismatchPixel == 1);
    CHECK(report.maxChannelDelta == 2);
    CHECK_FALSE(engine::compareShaderImagesRgba8(reference, changed, 1).matches());
    const engine::ShaderImageParityReport tolerantReport =
        engine::compareShaderImagesRgba8(reference, changed, 2);
    CHECK(tolerantReport.matches());
    CHECK(tolerantReport.maxChannelDelta == 2);

    const std::array<std::uint8_t, 3> malformed{{1, 2, 3}};
    CHECK(engine::compareShaderImagesRgba8(malformed, malformed).status ==
          engine::ShaderParityStatus::SizeMismatch);
}

TEST_CASE("M8: shader parity fixtures use the canonical deterministic RNG") {
    f4test::Random cpuFixture(0x8A11CEu);
    f4test::Random gpuFixture(0x8A11CEu);
    std::array<float, 256> cpuInputs{};
    std::array<float, 256> gpuInputs{};
    for (std::size_t i = 0; i < cpuInputs.size(); ++i) {
        cpuInputs[i] = cpuFixture.nextSigned();
        gpuInputs[i] = gpuFixture.nextSigned();
    }
    const engine::ShaderFloatParityReport report =
        engine::compareShaderFloats(cpuInputs, gpuInputs);
    CHECK(report.matches());
    CHECK(report.maxAbsoluteError == 0.0f);
}

TEST_CASE("M8: Camera viewProj is invertible and deterministic") {
    engine::Camera cam;
    cam.eye = math::Vec3(0, 0, 5);
    cam.center = math::Vec3(0, 0, 0);
    cam.fovYRadians = 1.04719758f;
    float aspect = 16.f / 9.f;
    auto vp = engine::cameraViewProj(cam, aspect);
    auto inv = vp.inverse();
    auto id = vp.mul(inv);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        float expect = (r == c) ? 1.f : 0.f;
        CHECK(doctest::Approx(id(r,c)).epsilon(1e-3) == expect);
    }
    // orbit deterministic
    auto e1 = engine::orbitEye(2.f, 0.5f, 1.f, math::Vec3(0,0,0));
    auto e2 = engine::orbitEye(2.f, 0.5f, 1.f, math::Vec3(0,0,0));
    CHECK(e1 == e2);
}

TEST_CASE("M8: GpuMesh translator maps mesh to GL layout") {
    resources::Mesh mesh;
    resources::VertexStream s;
    s.strideBytes = 36;
    struct V { float px,py,pz,nx,ny,nz,u,v; uint8_t r,g,b,a; };
    V verts[3] = {{-0.5f,-0.5f,0,0,0,1,0,0,255,0,0,255},{0.5f,-0.5f,0,0,0,1,1,0,0,255,0,255},{0,0.5f,0,0,0,1,0.5f,1,0,0,255,255}};
    s.data.resize(sizeof(verts)); std::memcpy(s.data.data(), verts, sizeof(verts));
    mesh.streams.push(std::move(s));
    mesh.attributes.push({resources::VertexSemantic::Position, resources::VertexFormat::F32_3,0,0});
    mesh.attributes.push({resources::VertexSemantic::Normal, resources::VertexFormat::F32_3,0,12});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0, resources::VertexFormat::F32_2,0,24});
    mesh.attributes.push({resources::VertexSemantic::Color0, resources::VertexFormat::UNORM8_4,0,32});
    mesh.indexFormat=resources::IndexFormat::U16; uint16_t idx[3]={0,1,2}; mesh.indices.resize(sizeof(idx)); std::memcpy(mesh.indices.data(), idx, sizeof(idx)); mesh.submeshes.push({0,3});
    gfx::IDevice* dev = gfx::createDevice();
    auto res = engine::createGpuMesh(*dev, mesh);
    REQUIRE(res.hasValue());
    auto gpu = std::move(res.value());
    CHECK(gpu.vertexBuffers.size() == 1);
    CHECK(gpu.indexBuffer.isValid());
    CHECK(gpu.layout.bindings.size() == 1);
    CHECK(gpu.layout.bindings[0].strideBytes == 36);
    CHECK(gpu.layout.attributes.size() == 4);
    CHECK(gpu.layout.attributes[0].location == 0);
    CHECK(gpu.layout.attributes[1].location == 1);
    CHECK(gpu.layout.attributes[2].location == 2);
    CHECK(gpu.layout.attributes[3].location == 3);
    engine::destroyGpuMesh(*dev, gpu);
    gfx::deleteDevice(dev);
}

TEST_CASE("M8: RenderQueue sorts opaque front-back transparent back-front") {
    engine::RenderQueue q;
    q.push(engine::RenderItem{{}, {}, {}, 5.f, false, 1});
    q.push(engine::RenderItem{{}, {}, {}, 1.f, false, 1});
    q.push(engine::RenderItem{{}, {}, {}, 10.f, true, 1});
    q.push(engine::RenderItem{{}, {}, {}, 20.f, true, 1});
    q.sort();
    auto items = q.items();
    REQUIRE(items.size() == 4);
    CHECK(items[0].depth == doctest::Approx(1.f));
    CHECK(items[1].depth == doctest::Approx(5.f));
    CHECK(items[2].depth == doctest::Approx(20.f));
    CHECK(items[3].depth == doctest::Approx(10.f));
}

TEST_CASE("M8: MaterialCache dedup and pipeline creation (Null backend)") {
    gfx::IDevice* dev = gfx::createDevice();
    engine::MaterialCache cache;
    engine::StandardMaterialDesc d1; d1.tint = math::Vec4(1,0,0,1);
    engine::StandardMaterialDesc d2; d2.tint = math::Vec4(0,1,0,1);
    auto h1 = cache.getOrCreate(d1);
    auto h1b = cache.getOrCreate(d1);
    auto h2 = cache.getOrCreate(d2);
    CHECK(h1 == h1b);
    CHECK(h1 != h2);
    CHECK(cache.liveCount() == 2);
    // Pipeline creation on Null backend should succeed (Null doesn't compile shaders)
    CHECK(cache.ensurePipeline(*dev, h1) == gfx::GfxError::None);
    CHECK(cache.get(h1)->pipeline.isValid());
    gfx::deleteDevice(dev);
}

TEST_CASE("M8: Renderer draws via NullDevice (one triangle)") {
    gfx::IDevice* dev = gfx::createDevice();
    gfx::FramebufferHandle framebuffer{};
    {
        ecs::Registry reg(16);
        engine::Renderer rnd(reg, *dev);
        REQUIRE(rnd.init() == gfx::GfxError::None);
        resources::Mesh mesh; resources::VertexStream s; s.strideBytes=36; struct V{float px,py,pz,nx,ny,nz,u,v; uint8_t r,g,b,a;}; V verts[3]={{-0.5f,-0.5f,0,0,0,1,0,0,255,0,0,255},{0.5f,-0.5f,0,0,0,1,1,0,0,255,0,255},{0,0.5f,0,0,0,1,0.5f,1,0,0,255,255}}; s.data.resize(sizeof(verts)); std::memcpy(s.data.data(),verts,sizeof(verts)); mesh.streams.push(std::move(s));
        mesh.attributes.push({resources::VertexSemantic::Position,resources::VertexFormat::F32_3,0,0});
        mesh.attributes.push({resources::VertexSemantic::Normal,resources::VertexFormat::F32_3,0,12});
        mesh.attributes.push({resources::VertexSemantic::TexCoord0,resources::VertexFormat::F32_2,0,24});
        mesh.attributes.push({resources::VertexSemantic::Color0,resources::VertexFormat::UNORM8_4,0,32});
        mesh.indexFormat=resources::IndexFormat::U16; uint16_t idx[3]={0,1,2}; mesh.indices.resize(sizeof(idx)); std::memcpy(mesh.indices.data(),idx,sizeof(idx)); mesh.submeshes.push({0,3});
        auto mh=rnd.createMesh(mesh); REQUIRE(mh.isValid());
        engine::StandardMaterialDesc md; md.tint=math::Vec4(1,1,1,1); auto mah=rnd.createMaterial(md); REQUIRE(mah.isValid()); REQUIRE(rnd.ensureMaterialPipeline(mah)==gfx::GfxError::None);
        ecs::Entity camE=reg.create(); engine::Camera cam; cam.eye={0,0,3}; reg.emplace<engine::Camera>(camE,cam);
        ecs::Entity e=reg.create(); auto &lt=reg.emplace<ecs::LocalTransform>(e); lt.position={0,0,0}; lt.rotation=math::Quat::identity(); lt.scale={1,1,1}; reg.emplace<engine::MeshRenderer>(e, engine::MeshRenderer{mh,mah,0,false,false}); ecs::updateWorldTransforms(reg);
        gfx::FramebufferDesc fbd; fbd.extent={640,480}; gfx::AttachmentDesc ca; ca.type=gfx::AttachmentType::Color; ca.clearOnLoad=true; fbd.attachments.push(ca); auto fbr=dev->createFramebuffer(fbd); REQUIRE(fbr.hasValue()); framebuffer=fbr.value();
        auto err=rnd.render(framebuffer, camE, 640.f/480.f); CHECK(err==gfx::GfxError::None);
        auto st=dev->stats(); CHECK(st.drawIndexedCalls == 1); CHECK(st.batches == 1);
        REQUIRE(rnd.graph().resourceCount() == 1);
        CHECK(rnd.graph().resource(0).type == gfx::ResourceType::Framebuffer);
        CHECK(rnd.graph().resource(0).importedFramebuffer == framebuffer);
        CHECK(rnd.graph().passCount() == 1);
        CHECK(rnd.graph().pass(0).writes[0] == 0);
    }
    CHECK(dev->stats().framebufferCount == 1);
    CHECK(dev->destroyFramebuffer(framebuffer) == gfx::GfxError::None);
    gfx::deleteDevice(dev);
}

TEST_CASE("M8: Renderer submits more than 64 objects without dropping draws") {
    gfx::NullDevice device;
    ecs::Registry registry(256);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);

    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(float) * 3;
    const std::array<float, 9> vertices{{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    }};
    stream.data.resize(sizeof(vertices));
    std::memcpy(stream.data.data(), vertices.data(), sizeof(vertices));
    mesh.streams.push(std::move(stream));
    mesh.attributes.push({resources::VertexSemantic::Position,
                          resources::VertexFormat::F32_3, 0, 0});
    mesh.indexFormat = resources::IndexFormat::U16;
    const std::array<std::uint16_t, 3> indices{{0, 1, 2}};
    mesh.indices.resize(sizeof(indices));
    std::memcpy(mesh.indices.data(), indices.data(), sizeof(indices));
    mesh.submeshes.push({0, 3});

    const resources::MeshHandle meshHandle = renderer.createMesh(mesh);
    REQUIRE(meshHandle.isValid());
    const resources::MaterialHandle material =
        renderer.createMaterial(engine::StandardMaterialDesc{});
    REQUIRE(material.isValid());
    REQUIRE(renderer.ensureMaterialPipeline(material) == gfx::GfxError::None);

    const ecs::Entity cameraEntity = registry.create();
    engine::Camera camera;
    camera.eye = {0.0f, 0.0f, 3.0f};
    registry.emplace<engine::Camera>(cameraEntity, camera);

    constexpr std::uint32_t objectCount = 65;
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        const ecs::Entity entity = registry.create();
        auto& transform = registry.emplace<ecs::LocalTransform>(entity);
        transform.rotation = math::Quat::identity();
        transform.scale = {1.0f, 1.0f, 1.0f};
        registry.emplace<engine::MeshRenderer>(
            entity, engine::MeshRenderer{meshHandle, material, 0, false, false});
    }
    ecs::updateWorldTransforms(registry);

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(device.stats().drawIndexedCalls == 1);
    CHECK(device.stats().batches == 1);
    CHECK(device.stats().instances == objectCount);
    CHECK(renderer.stats().instances == objectCount);
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}

TEST_CASE("M8: Renderer rejects use before initialization") {
    gfx::IDevice* dev = gfx::createDevice();
    {
        ecs::Registry registry(1);
        engine::Renderer renderer(registry, *dev);

        CHECK(renderer.render(gfx::FramebufferHandle{}, ecs::Entity{}, 1.0f) ==
              gfx::GfxError::BadDescriptor);
    }

    gfx::deleteDevice(dev);
}

TEST_CASE("M8: RenderGraph executes in order and honors resource lifetimes") {
    gfx::NullDevice dev;
    gfx::RenderGraph g;
    gfx::ResourceDesc persistent;
    persistent.name = containers::String("persistent texture");
    persistent.type = gfx::ResourceType::Texture;
    persistent.texture.width = 4;
    persistent.texture.height = 4;
    const auto persistentId = g.declareResource(std::move(persistent));

    gfx::ResourceDesc transient;
    transient.name = containers::String("transient buffer");
    transient.type = gfx::ResourceType::Buffer;
    transient.lifetime = gfx::ResourceLifetime::Transient;
    transient.buffer.sizeBytes = 64;
    transient.buffer.usage = gfx::BufferUsage::Storage;
    const auto transientId = g.declareResource(std::move(transient));

    int order = 0;
    gfx::BufferHandle observedTransient{};
    gfx::PassDesc p0;
    p0.name = containers::String("a");
    p0.reads.push(persistentId);
    p0.reads.push(transientId);
    p0.record = [&g, &order, &observedTransient, transientId](gfx::CommandBuffer&) {
        order = order * 10 + 1;
        observedTransient = g.buffer(transientId);
    };
    gfx::PassDesc p1;
    p1.name = containers::String("b");
    p1.reads.push(persistentId);
    p1.record = [&order](gfx::CommandBuffer&) { order = order * 10 + 2; };
    (void)g.declarePass(std::move(p0));
    (void)g.declarePass(std::move(p1));

    gfx::CommandBuffer cb;
    CHECK(g.execute(dev, cb) == gfx::GfxError::None);
    CHECK_FALSE(cb.isRecording());
    CHECK(order == 12);
    CHECK(observedTransient.isValid());
    CHECK(g.texture(persistentId).isValid());
    CHECK_FALSE(g.buffer(transientId).isValid());
    CHECK(dev.stats().textureCount == 1);
    CHECK(dev.stats().bufferCount == 0);

    const gfx::TextureHandle firstPersistent = g.texture(persistentId);
    const gfx::BufferHandle firstTransient = observedTransient;
    order = 0;
    CHECK(g.execute(dev, cb) == gfx::GfxError::None);
    CHECK(order == 12);
    CHECK(g.texture(persistentId) == firstPersistent);
    CHECK(observedTransient != firstTransient);
    CHECK(dev.stats().bufferCount == 0);

    CHECK(g.release(dev) == gfx::GfxError::None);
    CHECK(dev.stats().textureCount == 0);
    CHECK(g.clear(dev) == gfx::GfxError::None);
    CHECK(g.resourceCount() == 0);
    CHECK(g.passCount() == 0);
}

TEST_CASE("M9: RenderGraph emits and retains explicit access transitions") {
    gfx::NullDevice device;
    gfx::RenderGraph graph;
    gfx::ResourceDesc texture;
    texture.name = containers::String("sampled");
    texture.type = gfx::ResourceType::Texture;
    texture.texture.width = 4;
    texture.texture.height = 4;
    const auto textureId = graph.declareResource(std::move(texture));
    gfx::ResourceDesc buffer;
    buffer.name = containers::String("uniform");
    buffer.type = gfx::ResourceType::Buffer;
    buffer.buffer.sizeBytes = 16;
    buffer.buffer.usage = gfx::BufferUsage::Uniform;
    const auto bufferId = graph.declareResource(std::move(buffer));

    gfx::PassDesc pass;
    pass.name = containers::String("consume");
    pass.accesses.push({textureId,
                        {gfx::PipelineStageMask::FragmentShader,
                         gfx::ResourceAccessMask::ShaderRead,
                         gfx::TextureLayout::ShaderRead}});
    pass.accesses.push({bufferId,
                        {gfx::PipelineStageMask::VertexShader,
                         gfx::ResourceAccessMask::UniformRead,
                         gfx::TextureLayout::Undefined}});
    (void)graph.declarePass(std::move(pass));

    gfx::CommandBuffer commands;
    REQUIRE(graph.execute(device, commands) == gfx::GfxError::None);
    REQUIRE(commands.commandCount() == 2);
    CHECK(commands.commands()[0].kind == gfx::CommandKind::ResourceBarrier);
    CHECK(commands.commands()[0].barrierType == gfx::BarrierResourceType::Texture);
    CHECK(commands.commands()[1].kind == gfx::CommandKind::ResourceBarrier);
    CHECK(commands.commands()[1].barrierType == gfx::BarrierResourceType::Buffer);

    REQUIRE(graph.execute(device, commands) == gfx::GfxError::None);
    CHECK(commands.commandCount() == 0);
    CHECK(graph.release(device) == gfx::GfxError::None);
}

TEST_CASE("M8: RenderGraph validates passes before allocation") {
    gfx::NullDevice dev;
    gfx::RenderGraph g;
    gfx::ResourceDesc texture;
    texture.texture.width = 2;
    texture.texture.height = 2;
    (void)g.declareResource(std::move(texture));
    gfx::PassDesc pass;
    pass.reads.push(99);
    (void)g.declarePass(std::move(pass));

    gfx::CommandBuffer cb;
    CHECK(g.execute(dev, cb) == gfx::GfxError::BadDescriptor);
    CHECK(dev.stats().textureCount == 0);

    gfx::RenderGraph valid;
    gfx::CommandBuffer recording;
    recording.begin();
    CHECK(valid.execute(dev, recording) == gfx::GfxError::BadDescriptor);
    recording.end();
}

TEST_CASE("M8: RenderGraph rolls back a partial realization") {
    gfx::NullDevice dev;
    gfx::RenderGraph g;
    gfx::ResourceDesc texture;
    texture.texture.width = 2;
    texture.texture.height = 2;
    const auto textureId = g.declareResource(std::move(texture));

    gfx::TextureHandle invalidAttachment;
    invalidAttachment.index = 1234;
    invalidAttachment.generation = 7;
    gfx::ResourceDesc framebuffer;
    framebuffer.type = gfx::ResourceType::Framebuffer;
    framebuffer.framebuffer.extent = {2, 2};
    gfx::AttachmentDesc color;
    color.texture = invalidAttachment;
    framebuffer.framebuffer.attachments.push(color);
    (void)g.declareResource(std::move(framebuffer));

    CHECK(g.realize(dev) == gfx::GfxError::InvalidHandle);
    CHECK_FALSE(g.texture(textureId).isValid());
    CHECK(dev.stats().textureCount == 0);
    CHECK(dev.stats().framebufferCount == 0);
    CHECK(g.release(dev) == gfx::GfxError::None);
}

TEST_CASE("M8: RenderGraph rejects cross-device release without losing ownership") {
    gfx::NullDevice owner;
    gfx::NullDevice other;
    gfx::RenderGraph graph;
    gfx::ResourceDesc texture;
    texture.texture.width = 2;
    texture.texture.height = 2;
    (void)graph.declareResource(std::move(texture));
    REQUIRE(graph.realize(owner) == gfx::GfxError::None);

    CHECK(graph.clear(other) == gfx::GfxError::InvalidHandle);
    CHECK(graph.resourceCount() == 1);
    CHECK(owner.stats().textureCount == 1);
    CHECK(graph.release(owner) == gfx::GfxError::None);
    CHECK(owner.stats().textureCount == 0);
}

#ifdef F4_GRAPHICS_HAS_OPENGL
TEST_CASE("M8: Renderer keeps per-draw constants distinct on OpenGL") {
    auto winRes = platform::Window::create({"offscreen", 64, 64, false});
    REQUIRE(winRes.hasValue());
    platform::Window* win = winRes.value();
    REQUIRE(win->makeCurrent());
    auto glRes = platform::initGl(4, 3);
    REQUIRE(glRes.hasValue());

    gfx::IDevice* dev = gfx::createOpenGLDevice();
    REQUIRE(dev != nullptr);
    REQUIRE(dev->backend() == gfx::Backend::OpenGL);

    // Offscreen texture + framebuffer
    gfx::TextureDesc td; td.width = 64; td.height = 64; td.format = gfx::TextureFormat::RGBA8_UNORM; td.mipLevels = 1;
    auto texRes = dev->createTexture(td);
    REQUIRE(texRes.hasValue());
    gfx::TextureHandle tex = texRes.value();
    gfx::FramebufferDesc fbd; fbd.extent = {64, 64};
    gfx::AttachmentDesc col; col.type = gfx::AttachmentType::Color; col.texture = tex; col.clearOnLoad = true;
    col.clear.color[0] = 0.05f; col.clear.color[1] = 0.05f; col.clear.color[2] = 0.07f; col.clear.color[3] = 1.f;
    fbd.attachments.push(col);
    auto fbRes = dev->createFramebuffer(fbd);
    REQUIRE(fbRes.hasValue());
    gfx::FramebufferHandle fb = fbRes.value();

    {
        ecs::Registry reg(16);
        engine::Renderer rnd(reg, *dev);
        REQUIRE(rnd.init() == gfx::GfxError::None);
        resources::Mesh mesh; resources::VertexStream s; s.strideBytes=36;
        struct V{float px,py,pz,nx,ny,nz,u,v; uint8_t r,g,b,a;};
        V verts[3]={{-0.5f,-0.5f,0,0,0,1,0,0,255,0,0,255},{0.5f,-0.5f,0,0,0,1,1,0,0,255,0,255},{0,0.5f,0,0,0,1,0.5f,1,0,0,255,255}};
        s.data.resize(sizeof(verts)); std::memcpy(s.data.data(), verts, sizeof(verts)); mesh.streams.push(std::move(s));
        mesh.attributes.push({resources::VertexSemantic::Position,resources::VertexFormat::F32_3,0,0});
        mesh.attributes.push({resources::VertexSemantic::Normal,resources::VertexFormat::F32_3,0,12});
        mesh.attributes.push({resources::VertexSemantic::TexCoord0,resources::VertexFormat::F32_2,0,24});
        mesh.attributes.push({resources::VertexSemantic::Color0,resources::VertexFormat::UNORM8_4,0,32});
        mesh.indexFormat=resources::IndexFormat::U16; uint16_t idx[3]={0,1,2}; mesh.indices.resize(sizeof(idx)); std::memcpy(mesh.indices.data(), idx, sizeof(idx)); mesh.submeshes.push({0,3});
        auto mh = rnd.createMesh(mesh); REQUIRE(mh.isValid());
        engine::StandardMaterialDesc leftMaterial; leftMaterial.tint = math::Vec4(1,0,0,1);
        auto leftMaterialHandle = rnd.createMaterial(leftMaterial); REQUIRE(leftMaterialHandle.isValid()); REQUIRE(rnd.ensureMaterialPipeline(leftMaterialHandle)==gfx::GfxError::None);
        engine::StandardMaterialDesc rightMaterial; rightMaterial.tint = math::Vec4(0,1,0,1);
        auto rightMaterialHandle = rnd.createMaterial(rightMaterial); REQUIRE(rightMaterialHandle.isValid()); REQUIRE(rnd.ensureMaterialPipeline(rightMaterialHandle)==gfx::GfxError::None);
        ecs::Entity camE = reg.create(); engine::Camera cam; cam.eye={0,0,2}; reg.emplace<engine::Camera>(camE, cam);
        ecs::Entity left = reg.create(); auto& leftTransform = reg.emplace<ecs::LocalTransform>(left); leftTransform.position={-0.55f,0,0}; leftTransform.rotation=math::Quat::identity(); leftTransform.scale={0.75f,0.75f,0.75f};
        reg.emplace<engine::MeshRenderer>(left, engine::MeshRenderer{mh,leftMaterialHandle,0,false,false});
        ecs::Entity right = reg.create(); auto& rightTransform = reg.emplace<ecs::LocalTransform>(right); rightTransform.position={0.55f,0,0}; rightTransform.rotation=math::Quat::identity(); rightTransform.scale={0.75f,0.75f,0.75f};
        reg.emplace<engine::MeshRenderer>(right, engine::MeshRenderer{mh,rightMaterialHandle,0,false,false});
        ecs::updateWorldTransforms(reg);

        auto err = rnd.render(fb, camE, 1.f);
        REQUIRE(err == gfx::GfxError::None);
        dev->waitIdle();

        std::vector<std::uint8_t> pixels(64*64*4);
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const auto leftPixel = (32 * 64 + 17) * 4;
        const auto rightPixel = (32 * 64 + 47) * 4;
        const auto gapPixel = (32 * 64 + 32) * 4;
        INFO("left pixel ", (int)pixels[leftPixel], " ", (int)pixels[leftPixel+1], " ", (int)pixels[leftPixel+2]);
        INFO("right pixel ", (int)pixels[rightPixel], " ", (int)pixels[rightPixel+1], " ", (int)pixels[rightPixel+2]);
        CHECK(pixels[leftPixel] > 30);
        CHECK(pixels[leftPixel+1] < 25);
        CHECK(pixels[rightPixel] < 25);
        CHECK(pixels[rightPixel+1] > 30);
        CHECK(pixels[gapPixel] < 30);
        CHECK(pixels[gapPixel+1] < 30);

        const std::vector<std::uint8_t> reference = pixels;
        REQUIRE(rnd.render(fb, camE, 1.f) == gfx::GfxError::None);
        dev->waitIdle();
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const engine::ShaderImageParityReport parity =
            engine::compareShaderImagesRgba8(reference, pixels);
        INFO("image mismatch pixels=" << parity.mismatchPixelCount
                                      << " max channel delta=" << (int)parity.maxChannelDelta);
        CHECK(parity.matches());
    }

    CHECK(dev->destroyFramebuffer(fb) == gfx::GfxError::None);
    CHECK(dev->destroyTexture(tex) == gfx::GfxError::None);
    gfx::deleteDevice(dev);
    delete win;
}

TEST_CASE("M8: OpenGL float probe matches the seeded CPU reference") {
    auto windowResult = platform::Window::create({"shader parity", 32, 32, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());

    constexpr std::size_t sampleCount = 256;
    std::array<float, sampleCount> inputs{};
    std::array<float, sampleCount> reference{};
    std::array<float, sampleCount> observed{};
    f4test::Random random(0x8A11CEu);
    for (std::size_t i = 0; i < sampleCount; ++i) {
        inputs[i] = random.nextSigned();
        const float a = inputs[i] * 0.75f;
        const float b = inputs[i] * inputs[i] + 0.125f;
        reference[i] = (a + b) / (std::fabs(inputs[i]) + 1.0f);
    }

    constexpr const char* source = R"glsl(
#version 430 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Inputs { float values[]; } inputData;
layout(std430, binding = 1) writeonly buffer Outputs { float values[]; } outputData;
void main() {
    uint i = gl_GlobalInvocationID.x;
    precise float a = inputData.values[i] * 0.75;
    precise float b = inputData.values[i] * inputData.values[i] + 0.125;
    outputData.values[i] = (a + b) / (abs(inputData.values[i]) + 1.0);
}
)glsl";
    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    std::array<char, 1024> shaderLog{};
    glGetShaderInfoLog(shader, static_cast<GLsizei>(shaderLog.size()), nullptr, shaderLog.data());
    INFO("compute shader log: " << shaderLog.data());
    REQUIRE(compiled == GL_TRUE);

    const GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    std::array<char, 1024> programLog{};
    glGetProgramInfoLog(program, static_cast<GLsizei>(programLog.size()), nullptr,
                        programLog.data());
    INFO("compute program log: " << programLog.data());
    REQUIRE(linked == GL_TRUE);

    GLuint buffers[2]{};
    glGenBuffers(2, buffers);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(inputs)), inputs.data(),
                 GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(observed)), nullptr,
                 GL_STREAM_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[1]);

    glUseProgram(program);
    glDispatchCompute(static_cast<GLuint>(sampleCount / 64), 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(observed)),
                       observed.data());
    REQUIRE(glGetError() == GL_NO_ERROR);

    const engine::ShaderFloatParityReport parity =
        engine::compareShaderFloats(reference, observed);
    INFO("OpenGL max abs parity error=" << parity.maxAbsoluteError
                                         << " max rel=" << parity.maxRelativeError);
    CHECK(parity.matches());
    CHECK(parity.maxAbsoluteError * 4.0f <= math::kParityTolerance.abs);

    glDeleteBuffers(2, buffers);
    glDeleteProgram(program);
    glDeleteShader(shader);
    delete window;
}
#endif
