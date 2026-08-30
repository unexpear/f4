#include "doctest/doctest.h"

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
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"

using namespace f4;

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
        gfx::FramebufferDesc fbd; fbd.extent={640,480}; gfx::AttachmentDesc ca; ca.type=gfx::AttachmentType::Color; ca.clearOnLoad=true; fbd.attachments.push(ca); auto fbr=dev->createFramebuffer(fbd); REQUIRE(fbr.hasValue());
        auto err=rnd.render(fbr.value(), camE, 640.f/480.f); CHECK(err==gfx::GfxError::None);
        auto st=dev->stats(); CHECK(st.drawIndexedCalls == 1); CHECK(st.batches == 1);
    }
    gfx::deleteDevice(dev);
}

TEST_CASE("M8: RenderGraph declarations-only order") {
    gfx::RenderGraph g;
    auto r0 = g.declareResource(gfx::ResourceDesc{});
    auto r1 = g.declareResource(gfx::ResourceDesc{});
    CHECK(g.resourceCount()==2);
    int order=0;
    gfx::PassDesc p0; p0.name=containers::String("a"); p0.writes.push(r0); p0.record=[&order](gfx::CommandBuffer&){ order= order*10+1;};
    gfx::PassDesc p1; p1.name=containers::String("b"); p1.writes.push(r1); p1.record=[&order](gfx::CommandBuffer&){ order= order*10+2;};
    (void)g.declarePass(std::move(p0)); (void)g.declarePass(std::move(p1));
    CHECK(g.passCount()==2);
    gfx::IDevice* dev=gfx::createDevice();
    gfx::CommandBuffer cb; cb.begin();
    // Exercise execute path without real framebuffer: use dummy pass execution via direct record
    for (uint32_t i=0;i<g.passCount();++i) g.pass(i).record(cb);
    CHECK(order==12);
    gfx::deleteDevice(dev);
}
