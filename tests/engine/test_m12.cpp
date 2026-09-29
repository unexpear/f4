#include "doctest/doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "backends/null/null_device.h"
#include "core/ecs/transform.h"
#include "core/resources/mesh.h"
#include "engine/cameras/camera.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/render_queue.h"
#include "engine/rendering/renderer.h"
#include "engine/rendering/shader_constants.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/render_graph/render_graph.h"

using namespace f4;

namespace {

[[nodiscard]] auto makeTriangle() -> resources::Mesh {
    constexpr std::array<float, 9> vertices{{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    }};
    constexpr std::array<std::uint16_t, 3> indices{{0, 1, 2}};
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(float) * 3;
    stream.data.resize(sizeof(vertices));
    std::memcpy(stream.data.data(), vertices.data(), sizeof(vertices));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position,
                          resources::VertexFormat::F32_3, 0, 0});
    mesh.indices.resize(sizeof(indices));
    std::memcpy(mesh.indices.data(), indices.data(), sizeof(indices));
    mesh.submeshes.push({0, indices.size()});
    return mesh;
}

void addObject(ecs::Registry& registry, resources::MeshHandle mesh,
               resources::MaterialHandle material, math::Vec3 position) {
    const ecs::Entity entity = registry.create();
    auto& transform = registry.emplace<ecs::LocalTransform>(entity);
    transform.position = position;
    transform.rotation = math::Quat::identity();
    transform.scale = {1.0f, 1.0f, 1.0f};
    registry.emplace<engine::MeshRenderer>(
        entity, engine::MeshRenderer{mesh, material, 0, false, false});
}

} // namespace

TEST_CASE("M12: render graph retains same-state write barriers across passes and frames") {
    gfx::NullDevice device;
    gfx::RenderGraph graph;
    gfx::ResourceDesc buffer;
    buffer.type = gfx::ResourceType::Buffer;
    buffer.buffer.sizeBytes = 16;
    buffer.buffer.usage = gfx::BufferUsage::Storage;
    const auto bufferId = graph.declareResource(std::move(buffer));
    gfx::ResourceDesc texture;
    texture.texture.width = 4;
    texture.texture.height = 4;
    const auto textureId = graph.declareResource(std::move(texture));
    const gfx::ResourceState bufferState{
        gfx::PipelineStageMask::ComputeShader,
        gfx::ResourceAccessMask::ShaderRead | gfx::ResourceAccessMask::ShaderWrite,
        gfx::TextureLayout::Undefined};
    const gfx::ResourceState textureState{
        gfx::PipelineStageMask::ColorAttachment,
        gfx::ResourceAccessMask::ColorWrite, gfx::TextureLayout::ColorAttachment};
    for (int i = 0; i < 2; ++i) {
        gfx::PassDesc pass;
        pass.accesses.push({bufferId, bufferState});
        pass.accesses.push({textureId, textureState});
        (void)graph.declarePass(std::move(pass));
    }
    gfx::CommandBuffer commands;
    for (int frame = 0; frame < 2; ++frame) {
        REQUIRE(graph.execute(device, commands) == gfx::GfxError::None);
        CHECK(commands.commandCount() == 4);
        if (commands.commandCount() == 4) {
            CHECK(commands.commands()[2].before == bufferState);
            CHECK(commands.commands()[2].after == bufferState);
            CHECK(commands.commands()[3].before == textureState);
            CHECK(commands.commands()[3].after == textureState);
        }
    }
    CHECK(graph.release(device) == gfx::GfxError::None);
}

TEST_CASE("M12: render graph schedules explicit dependencies deterministically without a pass ceiling") {
    gfx::NullDevice device;
    gfx::RenderGraph graph;
    containers::Array<std::uint32_t> recorded;
    constexpr std::uint32_t count = 130;
    for (std::uint32_t i = 0; i < count; ++i) {
        gfx::PassDesc pass;
        if (i == 0) {
            pass.dependsOn.push(count - 1);
            pass.dependsOn.push(count - 1); // Duplicate edges must balance.
        }
        pass.record = [i, &recorded](gfx::CommandBuffer&) { recorded.push(i); };
        CHECK(graph.declarePass(std::move(pass)) == i);
    }
    REQUIRE(graph.compile() == gfx::GfxError::None);
    REQUIRE(graph.executionOrder().size() == count);
    for (std::uint32_t i = 0; i < count - 1; ++i) CHECK(graph.executionOrder()[i] == i + 1);
    CHECK(graph.executionOrder()[count - 1] == 0);
    gfx::CommandBuffer commands;
    for (int frame = 0; frame < 2; ++frame) {
        recorded.clear();
        REQUIRE(graph.execute(device, commands) == gfx::GfxError::None);
        REQUIRE(recorded.size() == count);
        for (std::uint32_t i = 0; i < count; ++i) CHECK(recorded[i] == graph.executionOrder()[i]);
    }
    CHECK(graph.declarePass({}) == count);
    CHECK(graph.executionOrder().isEmpty());
    REQUIRE(graph.compile() == gfx::GfxError::None);
    REQUIRE(graph.executionOrder().size() == count + 1);
    CHECK(graph.executionOrder()[count] == count);
    gfx::ResourceDesc resource;
    resource.type = gfx::ResourceType::Buffer;
    resource.buffer.sizeBytes = 16;
    (void)graph.declareResource(std::move(resource));
    CHECK(graph.executionOrder().isEmpty());
    REQUIRE(graph.compile() == gfx::GfxError::None);
    CHECK(device.stats().bufferCount == 0); // Compilation never materializes resources.
    CHECK(graph.clear(device) == gfx::GfxError::None);
    CHECK(graph.executionOrder().isEmpty());
    CHECK(graph.compile() == gfx::GfxError::None);
    CHECK(graph.execute(device, commands) == gfx::GfxError::None);
    CHECK(recorded.size() == count);
    CHECK(graph.release(device) == gfx::GfxError::None);
}

TEST_CASE("M12: render graph rejects invalid dependencies before recording or allocation") {
    for (int mode = 0; mode < 3; ++mode) {
        gfx::NullDevice device;
        gfx::RenderGraph graph;
        gfx::ResourceDesc resource;
        resource.type = gfx::ResourceType::Buffer;
        resource.buffer.sizeBytes = 16;
        (void)graph.declareResource(std::move(resource));
        bool recorded = false;
        gfx::PassDesc first;
        first.dependsOn.push(mode == 0 ? 99u : mode == 1 ? 0u : 1u);
        first.record = [&recorded](gfx::CommandBuffer&) { recorded = true; };
        (void)graph.declarePass(std::move(first));
        gfx::PassDesc second;
        second.dependsOn.push(0);
        (void)graph.declarePass(std::move(second));
        gfx::CommandBuffer commands;
        CHECK(graph.compile() == gfx::GfxError::BadDescriptor);
        CHECK(graph.executionOrder().isEmpty());
        CHECK(graph.execute(device, commands) == gfx::GfxError::BadDescriptor);
        CHECK_FALSE(recorded);
        CHECK_FALSE(commands.isRecording());
        CHECK(commands.commandCount() == 0);
        CHECK(device.stats().bufferCount == 0);
    }
}

TEST_CASE("M12: render graph preserves RAW WAR WAW hazards from declarations and access masks") {
    for (bool explicitAccess : {false, true}) {
        for (int hazard = 0; hazard < 3; ++hazard) {
            gfx::RenderGraph graph;
            gfx::ResourceDesc resource;
            resource.type = gfx::ResourceType::Buffer;
            resource.buffer.sizeBytes = 16;
            const auto resourceId = graph.declareResource(std::move(resource));
            for (std::uint32_t i = 0; i < 2; ++i) {
                const bool write = hazard == 2 || (hazard == 0 ? i == 0 : i == 1);
                gfx::PassDesc pass;
                if (i == 0) pass.dependsOn.push(1); // Contradicts the resource hazard.
                if (explicitAccess) {
                    pass.accesses.push({resourceId,
                        {gfx::PipelineStageMask::ComputeShader,
                         write ? gfx::ResourceAccessMask::ShaderWrite : gfx::ResourceAccessMask::ShaderRead,
                         gfx::TextureLayout::Undefined}});
                } else if (write) {
                    pass.writes.push(resourceId);
                } else {
                    pass.reads.push(resourceId);
                }
                (void)graph.declarePass(std::move(pass));
            }
            CHECK(graph.compile() == gfx::GfxError::BadDescriptor);
            CHECK(graph.executionOrder().isEmpty());
        }
    }
}

TEST_CASE("M12: render graph allows independent readers but orders the following writer") {
    gfx::RenderGraph graph;
    gfx::ResourceDesc resource;
    resource.type = gfx::ResourceType::Buffer;
    resource.buffer.sizeBytes = 16;
    const auto id = graph.declareResource(std::move(resource));
    gfx::PassDesc first;
    first.reads.push(id);
    first.reads.push(id);
    first.dependsOn.push(1);
    (void)graph.declarePass(std::move(first));
    gfx::PassDesc second;
    second.accesses.push({id, {gfx::PipelineStageMask::ComputeShader,
        gfx::ResourceAccessMask::ShaderRead, gfx::TextureLayout::Undefined}});
    (void)graph.declarePass(std::move(second));
    gfx::PassDesc writer;
    writer.writes.push(id);
    writer.reads.push(id); // One read/write use, not a self-dependency.
    writer.writes.push(id);
    (void)graph.declarePass(std::move(writer));
    REQUIRE(graph.compile() == gfx::GfxError::None);
    REQUIRE(graph.executionOrder().size() == 3);
    CHECK(graph.executionOrder()[0] == 1);
    CHECK(graph.executionOrder()[1] == 0);
    CHECK(graph.executionOrder()[2] == 2);
}

TEST_CASE("M12: render graph preserves texture layout transitions between readers") {
    gfx::RenderGraph graph;
    gfx::ResourceDesc resource;
    resource.texture.width = 4;
    resource.texture.height = 4;
    const auto id = graph.declareResource(std::move(resource));
    gfx::PassDesc sample;
    sample.dependsOn.push(1);
    sample.accesses.push({id, {gfx::PipelineStageMask::FragmentShader,
        gfx::ResourceAccessMask::ShaderRead, gfx::TextureLayout::ShaderRead}});
    (void)graph.declarePass(std::move(sample));
    gfx::PassDesc copy;
    copy.accesses.push({id, {gfx::PipelineStageMask::Transfer,
        gfx::ResourceAccessMask::TransferRead, gfx::TextureLayout::TransferSource}});
    (void)graph.declarePass(std::move(copy));
    CHECK(graph.compile() == gfx::GfxError::BadDescriptor);
}

TEST_CASE("M12: render queue batches opaque state and isolates transparent draws") {
    const resources::MeshHandle meshA{1, 2};
    const resources::MeshHandle meshB{2, 2};
    const resources::MaterialHandle materialA{3, 4};
    const resources::MaterialHandle materialB{4, 4};

    engine::RenderQueue queue;
    queue.push({ecs::Entity{5, 0}, engine::MeshRenderer{meshA, materialA},
                materialA, 3.0f, false, 7});
    queue.push({ecs::Entity{4, 0}, engine::MeshRenderer{meshA, materialA},
                materialA, 1.0f, false, 7});
    queue.push({ecs::Entity{3, 0}, engine::MeshRenderer{meshB, materialA},
                materialA, 2.0f, false, 7});
    queue.push({ecs::Entity{2, 0}, engine::MeshRenderer{meshA, materialB},
                materialB, 20.0f, true, 8});
    queue.push({ecs::Entity{1, 0}, engine::MeshRenderer{meshA, materialB},
                materialB, 10.0f, true, 8});
    queue.sort();

    REQUIRE(queue.count() == 5);
    CHECK(queue.items()[0].depth == doctest::Approx(1.0f));
    CHECK(queue.items()[1].depth == doctest::Approx(3.0f));
    CHECK(queue.items()[3].depth == doctest::Approx(20.0f));
    CHECK(queue.items()[4].depth == doctest::Approx(10.0f));

    containers::Array<engine::RenderBatch> batches;
    queue.buildBatches(batches);
    REQUIRE(batches.size() == 4);
    CHECK(batches[0].firstItem == 0);
    CHECK(batches[0].itemCount == 2);
    CHECK(batches[1].itemCount == 1);
    CHECK(batches[2].itemCount == 1);
    CHECK(batches[3].itemCount == 1);

    queue.buildBatches(batches, false);
    REQUIRE(batches.size() == queue.count());
    for (const engine::RenderBatch batch : batches) CHECK(batch.itemCount == 1);
}

TEST_CASE("M12: render queue orders non-finite depths deterministically") {
    engine::RenderQueue queue;

    engine::RenderItem finite;
    finite.entity = {2, 1};
    finite.depth = 3.0f;
    queue.push(finite);

    engine::RenderItem nonFinite = finite;
    nonFinite.entity = {1, 1};
    nonFinite.depth = std::numeric_limits<float>::quiet_NaN();
    queue.push(nonFinite);

    engine::RenderItem infinite = finite;
    infinite.entity = {3, 1};
    infinite.depth = std::numeric_limits<float>::infinity();
    queue.push(infinite);

    queue.sort();
    REQUIRE(queue.count() == 3);
    CHECK(queue.items()[0].entity == finite.entity);
    CHECK(queue.items()[1].entity == nonFinite.entity);
    CHECK(queue.items()[2].entity == infinite.entity);

    queue.clear();
    finite.transparent = true;
    nonFinite.transparent = true;
    infinite.transparent = true;
    queue.push(finite);
    queue.push(nonFinite);
    queue.push(infinite);
    queue.sort();
    REQUIRE(queue.count() == 3);
    CHECK(queue.items()[0].entity == nonFinite.entity);
    CHECK(queue.items()[1].entity == infinite.entity);
    CHECK(queue.items()[2].entity == finite.entity);
}

TEST_CASE("M12: mirrored and ordinary instances never share a raster batch") {
    engine::RenderQueue queue;
    engine::RenderItem item;
    queue.push(item);
    item.mirrored = true;
    queue.push(item);
    queue.push(item);
    queue.sort();
    containers::Array<engine::RenderBatch> batches;
    queue.buildBatches(batches);
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].itemCount == 1);
    CHECK(batches[1].itemCount == 2);
}

TEST_CASE("M12: dynamic upload ring grows transactionally and rotates frame banks") {
    gfx::NullDevice device;
    engine::DynamicBufferRing ring;
    REQUIRE(ring.create(device, gfx::BufferUsage::Vertex) == gfx::GfxError::None);
    CHECK_FALSE(ring.alloc(device, {}).valid);

    std::array<std::byte, 112> small{};
    std::array<std::byte, 700> large{};
    ring.beginFrame();
    const auto first = ring.alloc(device, small);
    REQUIRE(first.valid);
    CHECK(first.sizeBytes == small.size());
    CHECK(first.capacityBytes >= small.size());
    REQUIRE(ring.alloc(device, small).valid);
    CHECK(ring.bufferCount() == 2);

    ring.beginFrame();
    REQUIRE(ring.alloc(device, small).valid);
    ring.beginFrame();
    REQUIRE(ring.alloc(device, small).valid);
    CHECK(ring.bufferCount() == 4);

    ring.beginFrame();
    const auto grown = ring.alloc(device, large);
    REQUIRE(grown.valid);
    CHECK(grown.capacityBytes >= large.size());
    CHECK(ring.bufferCount() == 4);
    CHECK(device.stats().bufferCount == 4);

    ring.destroy(device);
    CHECK(device.stats().bufferCount == 0);
}

TEST_CASE("M12: renderer culls world bounds and batches visible opaque instances") {
    gfx::NullDevice device;
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    {
        ecs::Registry registry(32);
        engine::Renderer renderer(registry, device);
        REQUIRE(renderer.init() == gfx::GfxError::None);
        const resources::MeshHandle mesh = renderer.createMesh(makeTriangle());
        const resources::MaterialHandle material =
            renderer.createMaterial(engine::StandardMaterialDesc{});
        REQUIRE(mesh.isValid());
        REQUIRE(material.isValid());

        const ecs::Entity cameraEntity = registry.create();
        engine::Camera camera;
        camera.eye = {0.0f, 0.0f, 3.0f};
        camera.center = {0.0f, 0.0f, 0.0f};
        registry.emplace<engine::Camera>(cameraEntity, camera);
        addObject(registry, mesh, material, {-1.0f, 0.0f, 0.0f});
        addObject(registry, mesh, material, {0.0f, 0.0f, 0.0f});
        addObject(registry, mesh, material, {1.0f, 0.0f, 0.0f});
        addObject(registry, mesh, material, {1000.0f, 0.0f, 0.0f});
        ecs::updateWorldTransforms(registry);

        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
                gfx::GfxError::None);
        engine::FrameStats stats = renderer.stats();
        CHECK(stats.candidates == 4);
        CHECK(stats.frustumCulled == 1);
        CHECK(stats.visible == 3);
        CHECK(stats.draws == 1);
        CHECK(stats.instances == 3);
        CHECK(device.stats().drawIndexedCalls == 1);
        CHECK(device.stats().instances == 3);

        device.resetFrameStats();
        renderer.pbrSettings().batchingEnabled = false;
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
                gfx::GfxError::None);
        stats = renderer.stats();
        CHECK(stats.draws == 3);
        CHECK(stats.instances == 3);
        CHECK(device.stats().drawIndexedCalls == 3);

        device.resetFrameStats();
        renderer.pbrSettings().batchingEnabled = true;
        renderer.pbrSettings().frustumCullingEnabled = false;
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
                gfx::GfxError::None);
        stats = renderer.stats();
        CHECK(stats.frustumCulled == 0);
        CHECK(stats.visible == 4);
        CHECK(stats.draws == 1);
        CHECK(stats.instances == 4);
    }

    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}
