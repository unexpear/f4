#include "demos/demo_gizmo.h"
#include <cstring>
#include <cmath>
#include "core/math/mat4.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
namespace f4::demo {
namespace {
// 1x1x1 cube centered at origin, 36 verts non-indexed with pos/normal/color
// We will reuse for gizmo axes: 3 scaled cubes as axes, plus one asymmetrical "F" object
// F object: small 3D F made of 2 boxes to prove handedness (asymmetrical)
constexpr const char* kVs = R"glsl(
#version 430 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(std140,binding=0) uniform Ubo{ mat4 viewProj; mat4 model; };
out vec3 vColor;
void main(){ gl_Position = viewProj * model * vec4(aPos,1.0); vColor=aColor; }
)glsl";
constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;
void main(){ fragColor = vec4(vColor,1.0); }
)glsl";
struct Vert{ float x,y,z; float r,g,b; };
void pushCube(std::vector<Vert>& out, math::Vec3 center, math::Vec3 half, math::Vec3 color){
    float hx=half.x, hy=half.y, hz=half.z;
    float cx=center.x, cy=center.y, cz=center.z;
    // 12 tris = 36 verts
    const Vert v[36] = {
        // -X
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx-hx,cy-hy,cz+hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz+hz, color.x,color.y,color.z},
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz+hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz-hz, color.x,color.y,color.z},
        // +X
        {cx+hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz+hz, color.x,color.y,color.z},
        {cx+hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},
        // -Y
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz+hz, color.x,color.y,color.z},
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz+hz, color.x,color.y,color.z},{cx-hx,cy-hy,cz+hz, color.x,color.y,color.z},
        // +Y
        {cx-hx,cy+hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz-hz, color.x,color.y,color.z},
        {cx-hx,cy+hy,cz-hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},
        // -Z
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz-hz, color.x,color.y,color.z},
        {cx-hx,cy-hy,cz-hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz-hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz-hz, color.x,color.y,color.z},
        // +Z
        {cx-hx,cy-hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy-hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},
        {cx-hx,cy-hy,cz+hz, color.x,color.y,color.z},{cx+hx,cy+hy,cz+hz, color.x,color.y,color.z},{cx-hx,cy+hy,cz+hz, color.x,color.y,color.z},
    };
    for(auto &e: v) out.push_back(e);
}
} // namespace
bool GizmoScene::init(gfx::IDevice* dev, platform::Window*){
    dev_=dev;
    std::vector<Vert> verts;
    // 3 axes: X red 1.5 long, Y green, Z blue (RH: +Z out of screen toward viewer when looking down -Z)
    pushCube(verts, {0.75f,0,0}, {0.75f,0.05f,0.05f}, {1,0,0}); // X
    pushCube(verts, {0,0.75f,0}, {0.05f,0.75f,0.05f}, {0,1,0}); // Y
    pushCube(verts, {0,0,0.75f}, {0.05f,0.05f,0.75f}, {0,0,1}); // Z
    // Arrow heads as small cubes at ends
    pushCube(verts, {1.6f,0,0}, {0.15f,0.15f,0.15f}, {1,0.2f,0.2f});
    pushCube(verts, {0,1.6f,0}, {0.15f,0.15f,0.15f}, {0.2f,1,0.2f});
    pushCube(verts, {0,0,1.6f}, {0.15f,0.15f,0.15f}, {0.2f,0.2f,1});
    // Asymmetrical "F" object at (0.3,0.3,0.3): vertical bar + two horizontals to prove handedness
    // Vertical stem
    pushCube(verts, {0.35f,0.35f,0.35f}, {0.05f,0.25f,0.05f}, {1,1,0.2f});
    // Top bar of F (along +X)
    pushCube(verts, {0.5f,0.55f,0.35f}, {0.15f,0.05f,0.05f}, {1,1,0.2f});
    // Middle bar of F (shorter)
    pushCube(verts, {0.45f,0.35f,0.35f}, {0.10f,0.05f,0.05f}, {1,1,0.2f});
    // Validate the actual uploaded triangles, so smoke/static gates catch a
    // winding regression even when a particular camera hides the broken face.
    for (std::size_t box = 0; box < verts.size(); box += 36) {
        math::Vec3 center{};
        for (std::size_t i = 0; i < 36; ++i) {
            const auto& v = verts[box + i];
            center += math::Vec3{v.x, v.y, v.z} / 36.f;
        }
        for (std::size_t i = 0; i < 36; i += 3) {
            const auto& va = verts[box + i];
            const auto& vb = verts[box + i + 1];
            const auto& vc = verts[box + i + 2];
            const math::Vec3 a{va.x, va.y, va.z}, b{vb.x, vb.y, vb.z}, c{vc.x, vc.y, vc.z};
            if (math::dot(math::cross(b - a, c - a), a - center) <= 0.f) return false;
        }
    }
    gfx::BufferDesc bd; bd.sizeBytes = verts.size()*sizeof(Vert); bd.usage=gfx::BufferUsage::Vertex;
    auto vb=dev->createBuffer(bd, std::span(reinterpret_cast<const std::byte*>(verts.data()), verts.size()*sizeof(Vert)));
    if (!vb.hasValue()) return false;
    vbCube_ = vb.value();
    gfx::BufferDesc ubd; ubd.sizeBytes=128; ubd.usage=gfx::BufferUsage::Uniform;
    auto ub=dev->createBuffer(ubd, {});
    if (!ub.hasValue()) return false;
    ubo_ = ub.value();
    auto mk=[&](gfx::ShaderStage s, const char* src){
        gfx::ShaderDesc d; d.stage=s; d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size()); return dev->createShader(d);
    };
    auto vs=mk(gfx::ShaderStage::Vertex,kVs);
    auto fs=mk(gfx::ShaderStage::Fragment,kFs);
    if (!vs.hasValue() || !fs.hasValue()) return false;
    vs_ = vs.value();
    fs_ = fs.value();
    gfx::PipelineStateDesc pd; pd.vertexShader=vs_; pd.fragmentShader=fs_;
    pd.topology=gfx::PrimitiveTopology::TriangleList; pd.raster.cullMode=gfx::CullMode::Back;
    pd.depthStencil.depthTestEnable=true; pd.depthStencil.depthWriteEnable=true; pd.depthStencil.depthCompare=gfx::CompareOp::Less;
    { gfx::VertexInputLayout::VertexBinding b; b.binding=0; b.strideBytes=sizeof(Vert); pd.vertexInput.bindings.push(b);
      gfx::VertexInputLayout::VertexAttribute pa; pa.binding=0; pa.location=0; pa.format=gfx::VertexFormat::Float32x3; pa.offsetBytes=0; pd.vertexInput.attributes.push(pa);
      gfx::VertexInputLayout::VertexAttribute ca; ca.binding=0; ca.location=1; ca.format=gfx::VertexFormat::Float32x3; ca.offsetBytes=12; pd.vertexInput.attributes.push(ca);
    }
    auto pipe=dev->createPipeline(pd); if(!pipe.hasValue()) return false; pipe_=pipe.value();
    pd.raster.frontClockwise = true;
    auto mirrored = dev->createPipeline(pd);
    if (!mirrored.hasValue()) return false;
    mirroredPipe_ = mirrored.value();
    return true;
}
void GizmoScene::shutdown(gfx::IDevice* dev){
    if(mirroredPipe_.isValid()) dev->destroyPipeline(mirroredPipe_);
    if(pipe_.isValid()) dev->destroyPipeline(pipe_);
    if(fs_.isValid()) dev->destroyShader(fs_);
    if(vs_.isValid()) dev->destroyShader(vs_);
    if(ubo_.isValid()) dev->destroyBuffer(ubo_);
    if(vbCube_.isValid()) dev->destroyBuffer(vbCube_);
}
void GizmoScene::update(float dt, const Uint8* keys, const SDL_MouseMotionEvent*){
    bool h = keys[SDL_SCANCODE_H];
    if(h && !hHeld_) isRH_ = !isRH_;
    hHeld_ = h;
    angle_ += dt * 0.4f;
}
bool GizmoScene::prepareStaticCapture(FreeCamera& camera) {
    camera.position = {2.2f, 2.f, 4.5f};
    camera.yaw = std::atan2(4.f, 1.7f);
    camera.pitch = std::atan2(1.5f, std::sqrt(4.f * 4.f + 1.7f * 1.7f));
    return true;
}

void GizmoScene::render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t w, std::uint32_t h){
    float aspect = h>0? float(w)/float(h):1.f;
    math::Mat4 viewProj = cam.viewProjMatrix(aspect);
    bool camAtDefault = (cam.position.x==0.f && cam.position.y==5.f && cam.position.z==8.f && cam.yaw==1.57079633f && cam.pitch==0.55f);
    if(camAtDefault){
        math::Vec3 eye = {3.5f*std::cos(angle_*0.3f), 2.5f, 3.5f*std::sin(angle_*0.3f)};
        math::Mat4 view = math::Mat4::lookAtRH(eye, math::Vec3(0.3f,0.3f,0.3f), math::Vec3(0.f,1.f,0.f));
        math::Mat4 proj = math::Mat4::perspectiveRH_ZO(1.04719758f, aspect, 0.1f, 100.f);
        viewProj = proj.mul(view);
    }
    // Model: scale Z by -1 when LH to visibly flip handedness
    math::Mat4 model = math::Mat4::identity();
    if(!isRH_){
        model = math::Mat4::scaling(math::Vec3(1.f,1.f,-1.f));
    }
    struct Ubo{ math::Mat4 viewProj; math::Mat4 model; };
    Ubo ubo{viewProj, model};
    if(dev_) dev_->writeBuffer(ubo_,0,std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
    cb.bindPipeline(isRH_ ? pipe_ : mirroredPipe_);
    cb.bindVertexBuffer(0, vbCube_, sizeof(Vert));
    cb.bindResource(0, gfx::BindingType::UniformBuffer, ubo_.index, ubo_.generation);
    // 9 cubes *36 verts = 324 verts
    cb.draw(324,1);
}
} // namespace f4::demo
