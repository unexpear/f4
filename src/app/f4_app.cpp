// f4_app — windowed OpenGL demo (M7). Multi-mesh ricochet sandbox inside an
// enclosed room, exercising two engine capabilities:
//   * Frame timing: high-resolution performance-counter clock driving a fixed
//     60 Hz physics accumulator, with render interpolation between physics
//     ticks so motion stays smooth at any refresh rate.
//   * Hardware instancing: objects are grouped by mesh and each group is drawn
//     once (four cubes = one draw) using a divisor-1 transform/tint stream.
// The camera orbits the room center (mouse drag) and zooms (scroll); click an
// object to select (highlight) it. Manual smoke test only — NOT part of the
// headless test gate (a window/display is required).
//
// Controls:
//   Left-drag      orbit camera (yaw/pitch) around room center
//   Scroll wheel   zoom camera in/out
//   Left click     select / deselect the nearest object

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <algorithm>
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

// Initial window size (logical pixels). The framebuffer, projection and
// picking all track the live drawable size, so the window is freely resizable.
constexpr int kInitWidth  = 1200;
constexpr int kInitHeight = 800;

void fillBlob(Array<std::byte>& blob, const std::string& text) {
    blob.resize(text.size());
    std::memcpy(blob.data(), text.data(), text.size());
}

struct Vertex {
    float x, y, z;
    float r, g, b;
};

struct Mesh {
    const Vertex* verts;
    std::uint32_t vertCount;
    const std::uint16_t* indices;
    std::uint32_t indexCount;
    float radius;
};

// Per-instance data fed to the GPU through a divisor-1 vertex binding. World is
// the object's model matrix, color its tint. Layout mirrors the instanced
// vertex shader attributes (locations 2..6). 80 bytes, matches a std140 record.
struct InstanceData {
    Mat4 world;
    float color[4];
};

struct PhysObject {
    const Mesh* mesh;
    Vec3 pos;
    Vec3 vel;
    Vec3 rotation;
    Vec3 spin;
    Vec3 scale;
    Vec3 color;
    bool selected = false;
    Vec3 prevPos;        // state at start of this render frame's physics steps
    Vec3 prevRotation;
};

// ── Mesh data ──────────────────────────────────────────────────────

constexpr float S = 0.5f;

// clang-format off
constexpr Vertex kCubeVerts[] = {
    {-S, -S,  S, 1, 0, 0}, { S, -S,  S, 1, 0, 0},
    { S,  S,  S, 1, 0, 0}, {-S,  S,  S, 1, 0, 0},
    { S, -S, -S, 0, 1, 0}, {-S, -S, -S, 0, 1, 0},
    {-S,  S, -S, 0, 1, 0}, { S,  S, -S, 0, 1, 0},
    { S, -S,  S, 0, 0, 1}, { S, -S, -S, 0, 0, 1},
    { S,  S, -S, 0, 0, 1}, { S,  S,  S, 0, 0, 1},
    {-S, -S, -S, 0, 1, 1}, {-S, -S,  S, 0, 1, 1},
    {-S,  S,  S, 0, 1, 1}, {-S,  S, -S, 0, 1, 1},
    {-S,  S,  S, 1, 0, 1}, { S,  S,  S, 1, 0, 1},
    { S,  S, -S, 1, 0, 1}, {-S,  S, -S, 1, 0, 1},
    {-S, -S, -S, 1, 1, 0}, { S, -S, -S, 1, 1, 0},
    { S, -S,  S, 1, 1, 0}, {-S, -S,  S, 1, 1, 0},
};
constexpr std::uint16_t kCubeIndices[] = {
    0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
    12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
};

constexpr Vertex kPyramidVerts[] = {
    {-S, -S,  S, 1, 0, 0}, { S, -S,  S, 1, 0, 0},
    { 0,   S,  0, 1, 0, 0}, { 0,  S,  0, 1, 0, 0},
    { S, -S,  S, 0, 0, 1}, { S, -S, -S, 0, 0, 1},
    { 0,   S,  0, 0, 0, 1}, { 0,  S,  0, 0, 0, 1},
    { S, -S, -S, 0, 1, 0}, {-S, -S, -S, 0, 1, 0},
    { 0,   S,  0, 0, 1, 0}, { 0,  S,  0, 0, 1, 0},
    {-S, -S, -S, 0, 1, 1}, {-S, -S,  S, 0, 1, 1},
    { 0,   S,  0, 0, 1, 1}, { 0,  S,  0, 0, 1, 1},
    {-S, -S, -S, 1, 1, 0}, {-S, -S,  S, 1, 1, 0},
    { S, -S,  S, 1, 1, 0}, { S, -S, -S, 1, 1, 0},
};
constexpr std::uint16_t kPyramidIndices[] = {
    0,1,2, 3,4,5, 6,7,8, 9,10,11, 12,13,14, 12,14,15,
};

constexpr Vertex kOctaVerts[] = {
    { 0,  S,  0, 1, 0, 1},
    { 0, -S,  0, 1, 1, 0},
    { S,  0,  0, 0, 0, 1},
    { 0,  0,  S, 1, 0, 0},
    {-S,  0,  0, 0, 1, 1},
    { 0,  0, -S, 0, 1, 0},
};
constexpr std::uint16_t kOctaIndices[] = {
    0,3,2, 0,2,5, 0,5,4, 0,4,3,
    1,2,3, 1,5,2, 1,4,5, 1,3,4,
};

constexpr Vertex kDiamondVerts[] = {
    { 0,  S*2, 0, 0.8f, 0.2f, 0.2f},
    {-S,  0,   S, 0.2f, 0.8f, 0.2f},
    { S,  0,   S, 0.2f, 0.2f, 0.8f},
    { S,  0,  -S, 0.8f, 0.8f, 0.2f},
    {-S,  0,  -S, 0.2f, 0.8f, 0.8f},
    { 0, -S*2, 0, 0.8f, 0.2f, 0.8f},
};
constexpr std::uint16_t kDiamondIndices[] = {
    0,1,2, 0,2,3, 0,3,4, 0,4,1,
    5,2,1, 5,3,2, 5,4,3, 5,1,4,
};

constexpr Vertex kTetraVerts[] = {
    { 0,    S,   0,   1, 0.5f, 0},
    {-S,   -S,   S,   0, 1, 0.5f},
    { S,   -S,   S,   0.5f, 0, 1},
    { 0,   -S,  -S,   1, 0, 0.5f},
};
constexpr std::uint16_t kTetraIndices[] = {
    0,1,2, 0,2,3, 0,3,1, 1,3,2,
};
// clang-format on

constexpr Mesh kMeshes[] = {
    {kCubeVerts,     24, kCubeIndices,     36, 0.707f},
    {kPyramidVerts,  20, kPyramidIndices,  18, 0.75f},
    {kOctaVerts,      6, kOctaIndices,     24, 0.5f},
    {kDiamondVerts,   6, kDiamondIndices,  24, 1.0f},
    {kTetraVerts,     4, kTetraIndices,    12, 0.75f},
};

enum MeshId { MESH_CUBE = 0, MESH_PYRAMID, MESH_OCTA, MESH_DIAMOND, MESH_TETRA };
constexpr int kMeshCount = 5;
constexpr int kObjCount = 8;

auto worldMatrix(const Vec3& pos, const Vec3& rot, const Vec3& scale) -> Mat4 {
    return Mat4::translation(pos)
        .mul(Mat4::rotationZ(rot.z))
        .mul(Mat4::rotationX(rot.x))
        .mul(Mat4::rotationY(rot.y))
        .mul(Mat4::scaling(scale));
}

struct ScreenHit {
    float sx, sy;  // screen pixel center
};

ScreenHit toScreen(const Vec3& pos, const Mat4& mvp, std::uint32_t width, std::uint32_t height) {
    const Vec4 clip = mvp.mul(Vec4(pos.x, pos.y, pos.z, 1.0f));
    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    const float wf = static_cast<float>(width);
    const float hf = static_cast<float>(height);
    return {(ndcX + 1.0f) * 0.5f * wf,
            (1.0f - ndcY) * 0.5f * hf};
}

float maxScale(const Vec3& s) { return std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z))); }

// Room half-extent along each axis — objects bounce off these world-space
// bounds (the visible room walls/floor/ceiling).
constexpr float kRoom = 6.0f;

// Reflect velocity against the room's axis-aligned walls and clamp position so
// the object's bounding sphere never pokes through a wall.
void collideRoom(PhysObject& o) {
    const float r = o.mesh->radius * maxScale(o.scale);
    if (o.pos.x - r < -kRoom) { o.vel.x = std::abs(o.vel.x); o.pos.x = -kRoom + r; }
    if (o.pos.x + r >  kRoom) { o.vel.x = -std::abs(o.vel.x); o.pos.x =  kRoom - r; }
    if (o.pos.y - r < -kRoom) { o.vel.y = std::abs(o.vel.y); o.pos.y = -kRoom + r; }
    if (o.pos.y + r >  kRoom) { o.vel.y = -std::abs(o.vel.y); o.pos.y =  kRoom - r; }
    if (o.pos.z - r < -kRoom) { o.vel.z = std::abs(o.vel.z); o.pos.z = -kRoom + r; }
    if (o.pos.z + r >  kRoom) { o.vel.z = -std::abs(o.vel.z); o.pos.z =  kRoom - r; }
}

void collideObjects(PhysObject& a, PhysObject& b) {
    const float ra = a.mesh->radius * maxScale(a.scale);
    const float rb = b.mesh->radius * maxScale(b.scale);
    const Vec3 diff = b.pos - a.pos;
    const float distSq = lengthSq(diff);
    const float minDist = ra + rb;
    if (distSq < minDist * minDist && distSq > kEpsilon) {
        const float dist = std::sqrt(distSq);
        const Vec3 n = diff / dist;
        const float overlap = minDist - dist;
        a.pos -= n * (overlap * 0.5f);
        b.pos += n * (overlap * 0.5f);
        const Vec3 relVel = a.vel - b.vel;
        const float vn = dot(relVel, n);
        if (vn > 0.0f) {
            constexpr float kCOR = 0.9f;
            const float impulse = (1.0f + kCOR) * vn * 0.5f;
            a.vel -= n * impulse;
            b.vel += n * impulse;
            const Vec3 tangent = relVel - n * vn;
            const float tLen = length(tangent);
            if (tLen > kEpsilon) {
                const Vec3 t = tangent / tLen;
                a.spin += cross(n, t) * impulse * 0.2f;
                b.spin -= cross(n, t) * impulse * 0.2f;
            }
        }
    }
}

} // namespace

int main() {
    auto w = platform::Window::create({"f4 — ricochet", kInitWidth, kInitHeight, true});
    if (!w.hasValue()) return 1;
    platform::Window* window = w.value();
    window->makeCurrent();

    auto glInfo = platform::initGl(4, 3);
    if (!glInfo.hasValue()) return 1;

    IDevice* dev = createOpenGLDevice();
    if (dev == nullptr) return 1;

    // Geometry and instance streams are grouped by mesh. Repeated objects that
    // share geometry become one instanced draw; unlike a combined heterogeneous
    // draw, each instance consumes only its own mesh's index range.
    struct MeshGpu {
        BufferHandle vb;
        BufferHandle ib;
        BufferHandle instances;
        std::uint32_t indexCount;
    };
    std::array<MeshGpu, kMeshCount> meshGpu{};
    for (int i = 0; i < kMeshCount; ++i) {
        const Mesh& mesh = kMeshes[i];
        BufferDesc vbd; vbd.sizeBytes = mesh.vertCount * sizeof(Vertex); vbd.usage = BufferUsage::Vertex;
        meshGpu[i].vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(mesh.verts), vbd.sizeBytes)).value();
        BufferDesc ibd; ibd.sizeBytes = mesh.indexCount * sizeof(std::uint16_t); ibd.usage = BufferUsage::Index;
        meshGpu[i].ib = dev->createBuffer(ibd, std::span(reinterpret_cast<const std::byte*>(mesh.indices), ibd.sizeBytes)).value();
        BufferDesc instDesc; instDesc.sizeBytes = kObjCount * sizeof(InstanceData); instDesc.usage = BufferUsage::Vertex;
        meshGpu[i].instances = dev->createBuffer(instDesc, {}).value();
        meshGpu[i].indexCount = mesh.indexCount;
    }

    // Shared camera UBO (view-projection) — binding 0.
    BufferDesc camDesc; camDesc.sizeBytes = sizeof(Mat4); camDesc.usage = BufferUsage::Uniform;
    BufferHandle camUbo = dev->createBuffer(camDesc, {}).value();

    // Shared UBO + combined geometry ranges for the room (walls/floor/ceiling).
    // The room is drawn with front-face culling so its interior is visible.
    BufferDesc roomDesc; roomDesc.sizeBytes = 80; roomDesc.usage = BufferUsage::Uniform;
    BufferHandle roomUbo = dev->createBuffer(roomDesc, {}).value();

    // ── Shaders ────────────────────────────────────────────────────────
    // Instanced object shader: per-vertex pos/color (locations 0..1) plus
    // per-instance world transform rows + tint (locations 2..6, divisor 1).
    constexpr const char* kObjVS = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec4 aWorld0;
layout(location = 3) in vec4 aWorld1;
layout(location = 4) in vec4 aWorld2;
layout(location = 5) in vec4 aWorld3;
layout(location = 6) in vec4 aTint;
layout(std140, binding = 0) uniform CamUbo { mat4 viewProj; };
out vec4 vColor;
void main() {
    mat4 world = mat4(aWorld0, aWorld1, aWorld2, aWorld3);
    gl_Position = viewProj * world * vec4(aPos, 1.0);
    vColor = vec4(aColor * aTint.rgb, aTint.a);
}
)glsl";

    // Room shader: mvp + tint packed in one UBO (kept separate from objects).
    constexpr const char* kRoomVS = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform RoomUbo { mat4 mvp; vec4 tint; };
out vec4 vColor;
void main() { gl_Position = mvp * vec4(aPos, 1.0); vColor = vec4(aColor, 1.0) * tint; }
)glsl";

    constexpr const char* kOpaqueFS = R"glsl(
#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)glsl";

    auto makeVS = [&](const char* src) {
        ShaderDesc d; d.stage = ShaderStage::Vertex; fillBlob(d.blob, src);
        return dev->createShader(d);
    };
    auto makeFS = [&](const char* src) {
        ShaderDesc d; d.stage = ShaderStage::Fragment; fillBlob(d.blob, src);
        return dev->createShader(d);
    };

    auto objVs = makeVS(kObjVS); if (!objVs.hasValue()) return 1;
    auto roomVs = makeVS(kRoomVS); if (!roomVs.hasValue()) return 1;
    auto fs = makeFS(kOpaqueFS); if (!fs.hasValue()) return 1;

    // ── Object pipeline (instanced) ────────────────────────────────────
    PipelineStateDesc objPd;
    objPd.vertexShader = objVs.value(); objPd.fragmentShader = fs.value();
    objPd.topology = PrimitiveTopology::TriangleList;
    objPd.raster.cullMode = CullMode::Back;
    objPd.depthStencil.depthTestEnable = true;
    objPd.depthStencil.depthWriteEnable = true;
    objPd.depthStencil.depthCompare = CompareOp::Less;

    VertexInputLayout::VertexBinding vb0; vb0.binding = 0; vb0.strideBytes = sizeof(Vertex); vb0.divisor = 0;
    objPd.vertexInput.bindings.push(vb0);
    {   // per-vertex attributes
        VertexInputLayout::VertexAttribute pa; pa.binding = 0; pa.location = 0;
        pa.format = VertexFormat::Float32x3; pa.offsetBytes = 0;
        objPd.vertexInput.attributes.push(pa);
        VertexInputLayout::VertexAttribute ca; ca.binding = 0; ca.location = 1;
        ca.format = VertexFormat::Float32x3; ca.offsetBytes = 3 * sizeof(float);
        objPd.vertexInput.attributes.push(ca);
    }
    VertexInputLayout::VertexBinding vb1; vb1.binding = 1; vb1.strideBytes = sizeof(InstanceData); vb1.divisor = 1;
    objPd.vertexInput.bindings.push(vb1);
    {   // per-instance attributes: world matrix (4x vec4) + tint
        VertexInputLayout::VertexAttribute w0; w0.binding = 1; w0.location = 2; w0.format = VertexFormat::Float32x4; w0.offsetBytes = 0;
        objPd.vertexInput.attributes.push(w0);
        VertexInputLayout::VertexAttribute w1; w1.binding = 1; w1.location = 3; w1.format = VertexFormat::Float32x4; w1.offsetBytes = 16;
        objPd.vertexInput.attributes.push(w1);
        VertexInputLayout::VertexAttribute w2; w2.binding = 1; w2.location = 4; w2.format = VertexFormat::Float32x4; w2.offsetBytes = 32;
        objPd.vertexInput.attributes.push(w2);
        VertexInputLayout::VertexAttribute w3; w3.binding = 1; w3.location = 5; w3.format = VertexFormat::Float32x4; w3.offsetBytes = 48;
        objPd.vertexInput.attributes.push(w3);
        VertexInputLayout::VertexAttribute tint; tint.binding = 1; tint.location = 6; tint.format = VertexFormat::Float32x4; tint.offsetBytes = 64;
        objPd.vertexInput.attributes.push(tint);
    }
    auto objPipeline = dev->createPipeline(objPd); if (!objPipeline.hasValue()) return 1;

    // ── Room pipeline (front-cull, separate mvp+tint UBO) ──────────────
    PipelineStateDesc roomPd;
    roomPd.vertexShader = roomVs.value(); roomPd.fragmentShader = fs.value();
    roomPd.topology = PrimitiveTopology::TriangleList;
    roomPd.raster.cullMode = CullMode::Front;
    roomPd.depthStencil.depthTestEnable = true;
    roomPd.depthStencil.depthWriteEnable = true;
    roomPd.depthStencil.depthCompare = CompareOp::Less;
    {   VertexInputLayout::VertexBinding b; b.binding = 0; b.strideBytes = sizeof(Vertex);
        roomPd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute pa; pa.binding = 0; pa.location = 0;
        pa.format = VertexFormat::Float32x3; pa.offsetBytes = 0;
        roomPd.vertexInput.attributes.push(pa);
        VertexInputLayout::VertexAttribute ca; ca.binding = 0; ca.location = 1;
        ca.format = VertexFormat::Float32x3; ca.offsetBytes = 3 * sizeof(float);
        roomPd.vertexInput.attributes.push(ca);
    }
    auto roomPipeline = dev->createPipeline(roomPd); if (!roomPipeline.hasValue()) return 1;

    // Framebuffer + projection are rebuilt whenever the drawable (backbuffer)
    // size changes. The framebuffer has no texture attachments, so it maps to
    // the default framebuffer; resizing requires (re)building it to match.
    FramebufferHandle fb{};
    Mat4 proj = Mat4::identity();
    std::uint32_t fbWidth = 0;
    std::uint32_t fbHeight = 0;

    auto rebuildFramebuffer = [&]() -> bool {
        const std::uint32_t dw = window->width();
        const std::uint32_t dh = window->height();
        if (dw == 0 || dh == 0) return false; // minimized / not yet sized
        if (dw == fbWidth && dh == fbHeight && fb.index != 0) return true;
        if (fb.index != 0) dev->destroyFramebuffer(fb);
        FramebufferDesc fbd; fbd.extent = {dw, dh};
        AttachmentDesc colorAtt; colorAtt.type = AttachmentType::Color; colorAtt.clearOnLoad = true;
        colorAtt.clear.color[0] = 0.04f; colorAtt.clear.color[1] = 0.04f; colorAtt.clear.color[2] = 0.08f;
        fbd.attachments.push(colorAtt);
        AttachmentDesc depthAtt; depthAtt.type = AttachmentType::Depth; depthAtt.clearOnLoad = true;
        depthAtt.clear.depth = 1.0f; fbd.attachments.push(depthAtt);
        auto res = dev->createFramebuffer(fbd);
        if (!res.hasValue()) return false;
        fb = res.value();
        fbWidth = dw;
        fbHeight = dh;
        proj = Mat4::perspectiveRH_ZO(1.2f, static_cast<float>(dw) / static_cast<float>(dh), 0.1f, 100.0f);
        return true;
    };
    if (!rebuildFramebuffer()) return 1;

    // Four cubes share one geometry batch; four distinct shapes demonstrate
    // that unrelated geometry remains in separate batches.
    // clang-format off
    PhysObject objects[kObjCount] = {
        //  mesh                pos                             vel                               rotation                       spin                            scale                      color
        { &kMeshes[0], {-3.0f,  2.0f,  0.0f}, { 6.0f, -4.0f,  2.0f}, {0,0,0}, {0.8f,1.2f,0.3f}, {0.70f,0.70f,0.70f}, {1.0f,0.3f,0.3f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[0], { 3.0f,  2.0f,  0.0f}, {-5.0f,-3.0f,  4.0f}, {0,0,0}, {0.3f,0.9f,1.4f}, {0.55f,0.55f,0.55f}, {0.3f,0.7f,1.0f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[0], {-3.0f, -2.0f,  2.0f}, { 4.0f, 6.0f, -3.0f}, {0,0,0}, {1.4f,0.2f,0.7f}, {0.65f,0.65f,0.65f}, {0.9f,0.4f,1.0f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[0], { 3.0f, -2.0f, -2.0f}, {-6.0f, 4.0f,  3.0f}, {0,0,0}, {0.6f,1.1f,0.4f}, {0.60f,0.60f,0.60f}, {0.3f,1.0f,0.5f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[1], { 0.0f,  3.0f, -3.0f}, { 3.0f,-6.0f,  5.0f}, {0,0,0}, {1.2f,0.5f,0.8f}, {0.60f,0.60f,0.60f}, {0.2f,0.5f,1.0f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[2], { 0.0f, -3.0f,  3.0f}, {-3.0f, 5.0f, -4.0f}, {0,0,0}, {0.5f,1.3f,0.6f}, {0.85f,0.85f,0.85f}, {1.0f,1.0f,0.2f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[3], {-1.0f,  0.0f, -4.0f}, { 7.0f, 3.0f, -4.0f}, {0,0,0}, {0.6f,1.4f,0.2f}, {0.50f,0.50f,0.50f}, {0.2f,1.0f,0.4f}, false, {0,0,0}, {0,0,0} },
        { &kMeshes[4], { 1.0f,  0.0f,  4.0f}, {-4.0f,-5.0f, -2.0f}, {0,0,0}, {1.0f,0.3f,1.1f}, {0.65f,0.65f,0.65f}, {1.0f,0.4f,0.8f}, false, {0,0,0}, {0,0,0} },
    };
    // clang-format on

    for (auto& o : objects) { o.prevPos = o.pos; o.prevRotation = o.rotation; }

    // ── Camera: orbit around room center ──────────────────────────────
    float camYaw = 0.0f, camPitch = 0.0f, camDist = 10.0f;
    const Vec3 kTarget = Vec3(0.0f, 0.0f, 0.0f);
    bool dragging = false;
    int lastMouseX = 0, lastMouseY = 0;

    auto updateView = [&]() -> Mat4 {
        const float cp = std::cos(camPitch);
        const Vec3 eye = kTarget + Vec3(cp * std::sin(camYaw), std::sin(camPitch), cp * std::cos(camYaw)) * camDist;
        return Mat4::lookAtRH(eye, kTarget, Vec3(0.0f, 1.0f, 0.0f));
    };

    // ── High-resolution frame clock + fixed 60 Hz physics ─────────────
    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 prevCounter = SDL_GetPerformanceCounter();
    constexpr float kDt = 1.0f / 60.0f;
    float accumulator = 0.0f;
    std::array<std::array<InstanceData, kObjCount>, kMeshCount> batchData{};
    std::array<std::uint32_t, kMeshCount> batchCounts{};

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    rebuildFramebuffer();
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    lastMouseX = event.button.x;
                    lastMouseY = event.button.y;
                    // Selection: pick nearest object whose screen projection is
                    // close to the click point. Click coords are in logical
                    // pixels, so the projection must use the logical size.
                    if (window->logicalWidth() == 0 || window->logicalHeight() == 0) break;
                    const Mat4 mvp = proj.mul(updateView());
                    int best = -1;
                    float bestDist = 45.0f * 45.0f;
                    for (int i = 0; i < kObjCount; ++i) {
                        const ScreenHit sh = toScreen(objects[i].pos, mvp,
                                                      window->logicalWidth(), window->logicalHeight());
                        const float dx = sh.sx - event.button.x;
                        const float dy = sh.sy - event.button.y;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < bestDist) { bestDist = d2; best = i; }
                    }
                    if (best >= 0) {
                        const bool wasSelected = objects[best].selected;
                        for (auto& o : objects) o.selected = false;
                        objects[best].selected = !wasSelected;
                    } else {
                        for (auto& o : objects) o.selected = false;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    const int dx = event.motion.x - lastMouseX;
                    const int dy = event.motion.y - lastMouseY;
                    lastMouseX = event.motion.x;
                    lastMouseY = event.motion.y;
                    camYaw -= static_cast<float>(dx) * 0.008f;
                    camPitch -= static_cast<float>(dy) * 0.008f;
                    camPitch = std::clamp(camPitch, -1.5f, 1.5f);
                }
                break;
            case SDL_MOUSEWHEEL:
                camDist *= 1.0f + static_cast<float>(event.wheel.y) * -0.08f;
                camDist = std::clamp(camDist, 1.5f, 14.0f);
                break;
            }
        }

        // Measure the real elapsed time on a sub-millisecond clock.
        const Uint64 nowCounter = SDL_GetPerformanceCounter();
        double frameDt = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        if (frameDt > 0.1) frameDt = 0.1;  // clamp stalls ('spiral of death' guard)

        // Fixed 60 Hz physics. Leftover sub-step time stays in the accumulator
        // (never discarded) so the simulation tracks wall clock at any FPS.
        accumulator += static_cast<float>(frameDt);
        if (accumulator > 0.2f) accumulator = 0.2f;

        while (accumulator >= kDt) {
            accumulator -= kDt;
            // Keep the two most recent fixed-tick states. If a slow frame needs
            // several catch-up steps, interpolation uses only the final pair.
            for (auto& o : objects) { o.prevPos = o.pos; o.prevRotation = o.rotation; }
            for (auto& o : objects) {
                o.pos += o.vel * kDt;
                o.rotation += o.spin * kDt;
            }
            for (auto& o : objects) collideRoom(o);
            for (int i = 0; i < kObjCount; ++i)
                for (int j = i + 1; j < kObjCount; ++j)
                    collideObjects(objects[i], objects[j]);
        }

        // Render interpolation factor: how far between the previous and current
        // physics ticks we are (0..1). Smooths motion when render rate != 60 Hz.
        const float alpha = accumulator / kDt;

        const Mat4 view = updateView();
        const Mat4 viewProj = proj.mul(view);

        // Camera UBO (objects).
        dev->writeBuffer(camUbo, 0, std::span(reinterpret_cast<const std::byte*>(&viewProj), sizeof(Mat4)));

        // Group interpolated transforms by mesh. No per-frame allocation: each
        // mesh owns a persistent instance buffer sized for the worst case.
        batchCounts.fill(0);
        for (const PhysObject& o : objects) {
            const auto meshIndex = static_cast<std::size_t>(o.mesh - kMeshes);
            InstanceData& data = batchData[meshIndex][batchCounts[meshIndex]++];
            const Vec3 rp = lerp(o.prevPos, o.pos, alpha);
            const Vec3 rr = lerp(o.prevRotation, o.rotation, alpha);
            data.world = worldMatrix(rp, rr, o.scale);
            const float ti = o.selected ? 2.2f : 1.0f;
            data.color[0] = o.color.x * ti; data.color[1] = o.color.y * ti;
            data.color[2] = o.color.z * ti; data.color[3] = 1.0f;
        }
        for (int mesh = 0; mesh < kMeshCount; ++mesh) {
            if (batchCounts[mesh] == 0) continue;
            const std::size_t bytes = batchCounts[mesh] * sizeof(InstanceData);
            dev->writeBuffer(meshGpu[mesh].instances, 0,
                std::span(reinterpret_cast<const std::byte*>(batchData[mesh].data()), bytes));
        }

        // Room UBO.
        {
            struct { Mat4 mvp; float tint[4]; } RU;
            RU.mvp = viewProj.mul(Mat4::scaling(Vec3(kRoom * 2.0f)));
            RU.tint[0] = 0.45f; RU.tint[1] = 0.45f; RU.tint[2] = 0.5f; RU.tint[3] = 1.0f;
            dev->writeBuffer(roomUbo, 0, std::span(reinterpret_cast<const std::byte*>(&RU), sizeof(RU)));
        }

        // Keep the framebuffer/projection in sync if the window was resized
        // outside the resize event (e.g. via the OS while dragging moved).
        if (!rebuildFramebuffer()) { window->swap(); continue; }

        {
            CommandBuffer cb; cb.begin(); cb.beginRenderPass(fb);
            cb.setViewport({fbWidth, fbHeight});

            // Room (enclosure interior faces) — single non-instanced draw.
            cb.bindPipeline(roomPipeline.value());
            cb.bindVertexBuffer(0, meshGpu[MESH_CUBE].vb, sizeof(Vertex));
            cb.bindIndexBuffer(meshGpu[MESH_CUBE].ib, IndexFormat::U16);
            cb.bindResource(0, roomUbo.index, roomUbo.generation);
            cb.drawIndexed(meshGpu[MESH_CUBE].indexCount);

            // One instanced draw per geometry batch. The four cubes share one
            // call with instanceCount=4; all commands share this one submit().
            cb.bindPipeline(objPipeline.value());
            cb.bindResource(0, camUbo.index, camUbo.generation);
            for (int mesh = 0; mesh < kMeshCount; ++mesh) {
                if (batchCounts[mesh] == 0) continue;
                cb.bindVertexBuffer(0, meshGpu[mesh].vb, sizeof(Vertex));
                cb.bindVertexBuffer(1, meshGpu[mesh].instances, sizeof(InstanceData));
                cb.bindIndexBuffer(meshGpu[mesh].ib, IndexFormat::U16);
                cb.drawIndexed(meshGpu[mesh].indexCount, batchCounts[mesh]);
            }

            cb.endRenderPass(); cb.end(); dev->submit(cb);
        }

        window->swap();
    }

    dev->waitIdle(); deleteDevice(dev); delete window;
    return 0;
}
