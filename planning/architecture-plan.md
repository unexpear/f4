# Engine Architecture Plan

Single source of truth for engine design decisions. Reference material: [awesome3dengine](https://github.com/jcoder58/awesome3dengine).

## 1. Goals & Constraints

- Build a 3D game engine **from scratch** in C++ — custom math library, no engine frameworks.
- Target platforms: **Windows + Linux**.
- Graphics backends: **OpenGL, Vulkan, DirectX 12** — all three, behind one abstraction.
- No deadline. Slow, correct foundation over fast results.
- The goal is **not** "make three APIs work." The goal is to build an **API-independent rendering model that happens to have three implementations.**

## 2. Locked Decisions

| Decision | Choice | Notes |
|----------|--------|-------|
| Language | C++20 | Concepts/spans/constexpr; solid MSVC + GCC + Clang support |
| Platform layer | SDL2 | Window + input, wrapped in engine-owned abstraction |
| Testing | doctest | Header-only, fast compiles, fits template-heavy code |
| Shaders | Per-backend native | GLSL (GL), HLSL (DX12), GLSL→SPIR-V via glslang (Vulkan) |
| ECS | Data-oriented | No `Component` base class; components are data, systems operate on them |
| Component storage | `ComponentStorage<T>` sparse-set default | Interface swappable; semantics over layout |
| Render graph | Declarations-only version at M8 | Before Vulkan |
| Math | Fully custom | Vec/Mat/Quat written by hand, every op tested |

## 3. Architectural Corrections (adopted)

The plan deliberately avoids these traps:

1. **No inheritance-based `Component` interface.** No virtual base classes per component. Components are plain data in storage; systems are free functions or thin structs operating on batches of that data.
2. **Scene graph is NOT part of the ECS foundation.** ECS = entities + component storage + systems. Transform hierarchy = components (`Parent`, `LocalTransform`, world recomputation pass). Two separate concerns; hierarchy is *implemented with* ECS, never a second object model.
3. **Storage semantics before storage layout.** Engine code depends on `ComponentStorage<T>` behavior (add/remove/get/iterate), never on physical memory layout. Implementation may later become SoA, paged, chunked, or archetypal without rewriting systems.
4. **Math before ECS.** Fundamental types exist before transform-related components consume them.

### Amendments (decisions where this plan deviates from initial suggestions)

- **Initial storage impl: sparse sets**, not dense-array + hashmap. ~100 lines, O(1) add/remove/has, stable iteration order, production-proven (EnTT). Least likely to need replacing.
- **Command buffers from day one.** Engine interacts with the device ONLY by recording into `CommandBuffer` — even while the GL backend executes immediately. Retrofitting deferred recording onto an immediate-mode-shaped API is the classic trap. Backend selected at build/link time (one RHI implementation compiled per build) → virtual-call overhead irrelevant.
- **Render graph v1 = declarations only.** Passes + read/write declarations + resource sizes, executed in declaration order on GL. No auto-reordering/barrier synthesis until Vulkan demands it (M9) / scheduling optimization (M12).
- **M2 serialization trimmed.** POD/chunk save-load helpers only. Reflection-driven serialization waits until M5+ when resources need it.

## 4. Layer Architecture

Dependencies flow strictly downward.

```text
+------------------------------------------+
|              GAME / EDITOR               |
+------------------------------------------+
|          ENGINE SYSTEMS / ECS            |
+------------------------------------------+
|       ENGINE SERVICES / RESOURCES        |
+------------------------------------------+
|         RENDERING ABSTRACTION            |
+------------------------------------------+
|      PLATFORM + GRAPHICS BACKENDS        |
+------------------------------------------+
```

Renderer ↔ backends relationship (RHI):

```text
             Engine
                |
                v
       Graphics Abstraction (RHI)
                |
     +----------+-----------+
     v          v           v
   OpenGL     Vulkan      DX12
```

## 5. Folder Layout

```text
src/
├── core/                  # M0–M2: math, containers, memory, ids, logging
│   ├── math/
│   ├── containers/
│   ├── memory/
│   ├── ids/
│   └── logging/
├── ecs/                   # M3–M4
│   ├── entity/
│   ├── component/
│   ├── storage/
│   ├── query/
│   ├── world/
│   └── systems/
├── engine/                # M4–M5, M8
│   ├── scene/
│   ├── transform/
│   ├── resources/
│   ├── assets/
│   ├── cameras/
│   ├── lighting/
│   └── rendering/
├── graphics/              # M6: RHI — zero backend types leak upward
│   ├── device/
│   ├── buffer/
│   ├── texture/
│   ├── sampler/
│   ├── shader/
│   ├── pipeline/
│   ├── command/
│   ├── framebuffer/
│   └── render_graph/
├── platform/              # SDL2 wrapper (window, input, events)
└── backends/
    ├── opengl/            # M7
    ├── vulkan/            # M9
    └── dx12/              # M10
tests/                     # doctest suites, mirrors src/ structure
planning/                  # this folder
```

## 6. Conventions (locked once, documented forever)

- **Matrices:** column-major, right-handed world space.
- **Clip space:** depth range **0..1** (D3D-style). One projection matrix feeds all three backends; GL's native −1..1 handled inside the GL projection helper only.
- **Handles everywhere:** engine code never touches raw API objects or raw resource pointers — opaque handles only (`BufferHandle`, `TextureHandle`, ...).
- Generational IDs for entities and resources (stale-handle detection).
- Warnings-as-errors on all compilers; sanitizers (ASan/UBSan) run wherever the toolchain provides them: full on Linux/CI and on MSVC (via `EnableASAN` + test-time DLL path prepend). On Windows: MSYS2 GCC ships no sanitizer runtimes by upstream design; a stray `BuildTools` install without ASan libs is pinned around via `CMAKE_GENERATOR_INSTANCE`.

## 7. Shader Strategy (per-backend native)

Chosen trade-off: effects are maintained twice (GLSL + HLSL). Mitigations baked into milestones:

- Identical **vertex input layouts** and **uniform block structures** across backends, enforced via shared layout-constant headers per material.
- **Shader parity tests** from M8+: same scene through each backend must match a reference frame pixel-for-pixel.
- Escape hatch: migrating to HLSL+DXC single-source later touches only the asset pipeline — no engine changes, because M6 keeps shader sources as opaque blobs behind handles.
- Vulkan path: GLSL → SPIR-V via glslang at build time.

## 8. Development Roadmap

### Milestone 0 — Infrastructure
- `git init`; CMake presets (MSVC + GCC/Clang); warnings-as-errors
- Debug/Release configs; ASan/UBSan runs (availability auto-detected at configure time: full on Linux/CI/Clang64; skipped with warning on MSYS2 GCC, which ships no sanitizer runtimes)
- doctest wired up with one trivial passing test
- Logging + assert macros
- Folder skeleton per §5
- Optional GitHub Actions CI once a remote exists

### Milestone 1 — Math
- `Vec2/3/4`, `Mat3/4`, `Quaternion`
- Transform composition; AABB, Sphere, Plane, Frustum
- Numerical utilities (epsilon compare, deg/rad)
- Conventions doc (§6)
- **Every operation unit-tested**

### Milestone 1.5 — Quaternion Certification (DONE)
- `toQuat(Mat3)` hardened (Shepperd branches, sqrt args clamped for drifted
  inputs, sign contract documented) + `toQuat(Mat4)` overload (orthonormal
  upper-left block; translation ignored)
- `slerp` general branch now renormalizes output (endpoints land on a/b's
  rotation regardless of arc length; matches the nlerp fallback's behavior)
- Deterministic fixture generator (`tests/random.hpp`): seeded mt19937,
  canonical `u32>>8 / 2^24` float mapping - NO `uniform_real_distribution`
  (not portable across stdlibs). This generator is THE source of M8 parity
  fixtures: same seeds must feed CPU and GPU sides.
- Property battery (`test_quat_properties.cpp`, 15 cases / ~4600 assertions):
  composition homomorphism, norm preservation, normalization idempotence,
  inverse==conj (unit), slerp endpoints/constant angular velocity/shortest-path
  sign-fix, double cover, mat->quat->mat authoritative roundtrip,
  yaw*pitch*roll order pin, zero axis, gimbal +-90 deg stability, drift
  tolerance. Rotation-semantic assertions wherever double cover applies -
  NEVER componentwise quat equality across a possible sign flip.

### M8 carry-forwards (from M1/M1.5 lessons)
- kTightTolerance sits at the residue floor of exact-angle trig constants;
  chained rotations floor at kDefaultTolerance, never tighter
- Before any GPU-parity fixtures: reversed-Z decision, deep-hierarchy drift
  study, frustum adversarial tests, orthographic projection,
  plane/frustum reconstruction roundtrips
- kParityTolerance{1e-5,1e-4} reserved name; calibrate from measured deltas x4

### Milestone 2 — Memory & Containers
- Generational IDs, handles (`src/core/ids/`)
- Containers: dynamic array, string, hash map wrappers (`src/core/containers/`)
- Allocators: arena (linear), pool (fixed-chunk) (`src/core/memory/`)
- Resource handle types (tag-typed `Handle<Tag>`, concrete tags arrive M5/M6)
- POD/chunk serialization helpers only

Design decisions locked at M2 start:
- **Heap backing via one seam**: all dynamic memory flows through
  `f4::memory::heapAllocate/heapFree`, implemented with aligned
  `::operator new/delete` (portable GCC/MSVC/Clang). Future custom backing
  swaps exactly one place.
- **Containers are NOT allocator-templated yet.** Documented trade-off:
  allocator polymorphism deferred until a proven need (M12 job system /
  frame arenas); the single heap seam keeps that retrofit cheap.
- **Arena contract**: bump allocation, `reset()` between frames, NEVER runs
  destructors (placement-new'd types must be lifetime-trivial or externally
  torn down). Exhaustion returns nullptr - caller decides; sizing bugs are
  caught in tests, not hidden.
- **Pool contract**: fixed stride >= sizeof(void*) so the free list is
  intrusive; LIFO reuse; double-free is UB like operator delete (ASan
  presets are the net). TypedPool<T> adds ctor/dtor discipline on top.
- **Generational handles**: {u32 index, u32 generation}, null = index
  0xFFFFFFFF. GenerationalArena<T> proves stale-handle detection (ABA-safe)
  before M3 builds entity registries and ComponentStorage on it.
- **Serialization trimmed** (per §3 amendment): MemoryWriter/MemoryReader +
  chunk framing (magic/version/payload) over byte buffers; FILE IO plumbing
  defers to M5 resources.
- **String/StringView contracts**: StringView is strictly non-owning and
  read-only; caller guarantees the backing outlives it. Null view
  (data==nullptr) is distinct from empty view (size 0). String owns a heap
  buffer, always null-terminated for C interop; copy = deep copy, move =
  steal (source left valid-empty). NO small-string optimization - documented
  trade-off, revisit at M12 if profiling demands. Equal contents hash equal
  across String and StringView (shared FNV-1a).
- **HashMap contract**: thin engine API over std::unordered_map (per plan:
  "wrappers"). insert() never overwrites; set() upserts; find() returns
  pointer-or-null (matches GenerationalArena::get convention). Iteration
  order is UNSPECIFIED - any logic needing determinism must sort keys
  explicitly. Reference/pointer stability of values under rehash holds
  (node-based backing) and is tested. Handle<Tag> keys work via the
  std::hash specialization in handle.h.
- **Serialization contract**: byte image = raw in-memory representation
  copied verbatim (memcpy; NO alignment requirements on the stream). POD
  paths enforce std::is_trivially_copyable at compile time. Endian policy is
  EXPLICIT little-endian: streams begin with an endian marker that readers
  verify against std::endian::native - mismatched streams are rejected, not
  silently swapped. Chunk framing: {u32 id, u32 version, u32 payloadSize}
  header per chunk; reader validates declared size against remaining bytes
  and caller-provided capacity before copying.

### Milestone 3 — ECS
- Entity create/destroy with generational validation
- Component registration; `ComponentStorage<T>` (sparse set)
- add/remove/get; queries/views; system execution runner
- Entity lifetime stress tests

### Milestone 4 — Transform
- `LocalTransform` + parent links as components
- World-matrix recomputation pass, dirty propagation
- Reparent/orphan/cycle tests

### Milestone 5 — Resources
- Unified model: Mesh, Texture, Shader, Material, Pipeline, Buffer
- Central registry; opaque handles only; reflection-based serialization starts here if needed

### Milestone 6 — Graphics Abstraction (RHI)
- Device, Buffer, Texture, Sampler, Shader, PipelineState, CommandBuffer, Swapchain, Framebuffer
- Command-recording-first API (see §3 amendment)
- Zero OpenGL/Vulkan/DX12-specific types above this line

### Milestone 7 — OpenGL Backend
- Minimum to prove abstraction: window → device → shader → vertex/index buffers → pipeline → draw
- Triangle → cube → hierarchy of cubes
- SDL2 window/context integration

### Milestone 8 — Renderer + Render Graph v1
- Camera, Light, MeshRenderer components; Material concept
- RenderQueue sorting
- Render graph v1: declared passes (e.g., shadow → main), declared resource lifetimes/sizes, executed in declaration order
- Shader parity test harness introduced

### Milestone 9 — Vulkan Backend
- Same abstraction; render-graph metadata drives barriers/transitions/submission
- Expect and log every abstraction mistake — this milestone's purpose

### Milestone 10 — DX12 Backend
- Same abstraction; descriptor heaps mapped onto our binding model
- Validates that binding/descriptor design was API-neutral

### Milestone 11 — PBR
- BRDF (GGX), IBL, normal mapping, metallic/roughness workflow
- Shadow filtering; tone mapping
- Implemented natively in both GLSL and HLSL under §7 parity rules

### Milestone 12 — Optimization
- Frustum culling, occlusion culling
- Batching, instancing
- Job system, multithreaded command recording
- SIMD math paths
- Render graph scheduling, async compute

## 9. Key References (from awesome3dengine)

| Topic | Resource |
|-------|----------|
| Overall architecture | *Game Engine Architecture* — Jason Gregory (Ch. 3–6, 8) |
| Renderer design | "Designing a Modern Rendering Engine" thesis; Data Driven Renderer PDF; Modern Renderer Design thread |
| Data-oriented rendering | "ECS and Rendering" (Our Machinery); Data Driven Rendering (Sassone) |
| PBR | Allegorithmic PBR Guide; UE4 PBR docs; Moving Frostbite to PBR |
| Lighting/pipelines | Forward+ paper; Forward vs Deferred vs Forward+ (3DGEP); 2048 point lights @60fps |
| Vulkan | Writing an Efficient Vulkan Renderer (zeux.io); BansheeEngine Vulkan blog/code |
| Optimization | Fast Hierarchical Culling (Cesium); Frostbite compute pipeline; Agner's optimization resources |
| Physics (later) | Introduction to Networked Physics (Gaffer on Games); Unified Particle Physics preprint |
| Code study | BansheeEngine (C++14/Vulkan), Tiny Renderers, EntityX |
## 10. Immediate Next Step

Milestone 2 is complete and verified (139 cases / 5,405 assertions, all six
presets green): memory seam (heapAllocate/heapFree), Arena/Pool allocators,
Array<T>, generational handles + GenerationalArena, String/StringView with
shared FNV-1a hashing, HashMap wrapper, POD/chunk serialization with explicit
little-endian policy.

Next: Milestone 3 (Application Layer) - platform/window abstraction,
input events, and the main loop skeleton that the renderer will plug into.
