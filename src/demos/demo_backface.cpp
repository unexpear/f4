#include "demos/demo_backface.h"
#include <cstring>
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
namespace f4::demo {
namespace {
constexpr float kVerts[] = {
    // left triangle CCW (should survive Back cull) - red
    -0.8f, -0.5f, 0.f,  1.f, 0.2f, 0.2f,
    -0.2f, -0.5f, 0.f,  1.f, 0.2f, 0.2f,
    -0.5f,  0.5f, 0.f,  1.f, 0.2f, 0.2f,
    // right triangle CW (should be culled with Back) - blue
     0.2f, -0.5f, 0.f,  0.2f, 0.4f, 1.f,
     0.5f,  0.5f, 0.f,  0.2f, 0.4f, 1.f,
     0.8f, -0.5f, 0.f,  0.2f, 0.4f, 1.f,
};
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
gfx::PipelineHandle makePipe(gfx::IDevice* dev, gfx::ShaderHandle vs, gfx::ShaderHandle fs, gfx::CullMode cull){
    gfx::PipelineStateDesc pd; pd.vertexShader=vs; pd.fragmentShader=fs;
    pd.topology=gfx::PrimitiveTopology::TriangleList; pd.raster.cullMode=cull;
    pd.depthStencil.depthTestEnable=true; pd.depthStencil.depthWriteEnable=true; pd.depthStencil.depthCompare=gfx::CompareOp::Less;
    { gfx::VertexInputLayout::VertexBinding b; b.binding=0; b.strideBytes=24; pd.vertexInput.bindings.push(b);
      gfx::VertexInputLayout::VertexAttribute pa; pa.binding=0; pa.location=0; pa.format=gfx::VertexFormat::Float32x3; pa.offsetBytes=0; pd.vertexInput.attributes.push(pa);
      gfx::VertexInputLayout::VertexAttribute ca; ca.binding=0; ca.location=1; ca.format=gfx::VertexFormat::Float32x3; ca.offsetBytes=12; pd.vertexInput.attributes.push(ca);
    }
    auto r=dev->createPipeline(pd); return r.hasValue()?r.value():gfx::PipelineHandle{};
}
} // namespace
bool BackfaceScene::init(gfx::IDevice* dev, platform::Window*){
    dev_=dev;
    gfx::BufferDesc bd; bd.sizeBytes=sizeof(kVerts); bd.usage=gfx::BufferUsage::Vertex;
    auto vb=dev->createBuffer(bd, std::span(reinterpret_cast<const std::byte*>(kVerts), sizeof(kVerts)));
    if (!vb.hasValue()) return false;
    vb_ = vb.value();
    gfx::BufferDesc ubd; ubd.sizeBytes=128; ubd.usage=gfx::BufferUsage::Uniform;
    auto ub=dev->createBuffer(ubd, {});
    if (!ub.hasValue()) return false;
    ubo_=ub.value();
    auto mk=[&](gfx::ShaderStage s, const char* src){
        gfx::ShaderDesc d; d.stage=s; d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size()); return dev->createShader(d);
    };
    auto vs=mk(gfx::ShaderStage::Vertex,kVs);
    auto fs=mk(gfx::ShaderStage::Fragment,kFs);
    if (!vs.hasValue() || !fs.hasValue()) return false;
    vs_ = vs.value();
    fs_ = fs.value();
    pipeBack_=makePipe(dev,vs_,fs_,gfx::CullMode::Back);
    pipeNone_=makePipe(dev,vs_,fs_,gfx::CullMode::None);
    return pipeBack_.isValid() && pipeNone_.isValid();
}
void BackfaceScene::shutdown(gfx::IDevice* dev){
    if(pipeBack_.isValid()) dev->destroyPipeline(pipeBack_);
    if(pipeNone_.isValid()) dev->destroyPipeline(pipeNone_);
    if(fs_.isValid()) dev->destroyShader(fs_);
    if(vs_.isValid()) dev->destroyShader(vs_);
    if(ubo_.isValid()) dev->destroyBuffer(ubo_);
    if(vb_.isValid()) dev->destroyBuffer(vb_);
}
void BackfaceScene::update(float dt, const Uint8* keys, const SDL_MouseMotionEvent*){
    bool cHeld = keys[SDL_SCANCODE_C];
    if(cHeld && !keyHeld_) cullBack_ = !cullBack_;
    keyHeld_ = cHeld;
    bool rHeld = keys[SDL_SCANCODE_R];
    if(rHeld && !rotKeyHeld_) autoRot_ = !autoRot_;
    rotKeyHeld_ = rHeld;
    // Space resets angle
    if(keys[SDL_SCANCODE_SPACE]) angle_=0.f;
    if(autoRot_) angle_ += dt * 0.6f;
}
void BackfaceScene::render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t width, std::uint32_t height){
    float aspect = height>0 ? float(width)/float(height) : 1.f;
    math::Mat4 viewProj = cam.viewProjMatrix(aspect);
    // If free cam is at default (no input), use orbit so both tris are inspectable immediately
    bool camAtDefault = (cam.position.x==0.f && cam.position.y==5.f && cam.position.z==8.f && cam.yaw==1.57079633f && cam.pitch==0.55f);
    if(camAtDefault){
        const float orbitAngle = angle_ + 1.57079633f;
        math::Vec3 eye = {2.2f * std::cos(orbitAngle), 1.2f, 2.2f * std::sin(orbitAngle)};
        math::Mat4 view = math::Mat4::lookAtRH(eye, math::Vec3(0.f,0.f,0.f), math::Vec3(0.f,1.f,0.f));
        math::Mat4 proj = math::Mat4::perspectiveRH_ZO(1.04719758f, aspect, 0.1f, 100.f);
        viewProj = proj.mul(view);
    }
    math::Mat4 model = math::Mat4::rotationY(autoRot_ ? 0.f : angle_);
    struct Ubo{ math::Mat4 viewProj; math::Mat4 model; };
    Ubo ubo{viewProj, model};
    if(dev_) dev_->writeBuffer(ubo_, 0, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
    cb.bindPipeline(cullBack_ ? pipeBack_ : pipeNone_);
    cb.bindVertexBuffer(0, vb_, 24);
    cb.bindResource(0, gfx::BindingType::UniformBuffer, ubo_.index, ubo_.generation);
    cb.draw(6,1);
}
} // namespace f4::demo
