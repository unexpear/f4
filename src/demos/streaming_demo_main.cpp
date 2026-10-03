#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/memory/memory.h"
#include "demos/demo_bench.h"
#include "demos/streaming_demo_io.h"
#include "demos/streaming_demo_state.h"
#include "engine/world/terrain_boundaries.h"
#include "engine/world/terrain_boundary_walls.h"
#include "engine/world/terrain_regions.h"
#include "engine/world/boundary_profile_job.h"

namespace {
using namespace f4;
namespace demo = f4::demo;
using Clock = std::chrono::steady_clock;
using Bytes = containers::Array<std::byte>;

auto micros(Clock::duration duration) -> double {
    return std::chrono::duration<double, std::micro>(duration).count();
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
auto homeCamera(float offset = 0.f) -> demo::FreeCamera {
    demo::FreeCamera camera;
    camera.position = {32.f + offset, 92.f, 108.f};
    camera.pitch = std::atan2(72.f, 76.f);
    camera.moveSpeed = 45.f;
    return camera;
}

class Application {
public:
    Application(std::filesystem::path output, bool verify,
                containers::Array<world::ChunkDelta> edits)
        : output_(std::move(output)), verify_(verify),
          window_("f4 M16 Streaming World | E edit, F5 save, F9 reload, H home", 1280, 720, !verify),
          hud_(window_.device()), edits_(std::move(edits)) {
        require(window_.ok(), "Cannot create OpenGL demo window");
        const int requested = verify ? 0 : 1;
        if (SDL_GL_SetSwapInterval(requested) != 0) {
            std::fprintf(stderr, "Swap interval request failed: %s\n", SDL_GetError());
        }
        std::printf("OpenGL streaming demo; swap interval=%d; output=%s\n",
                    SDL_GL_GetSwapInterval(), output_.string().c_str());
        require(hud_.init(1024, 1.5f) == gfx::GfxError::None, "Cannot initialize HUD");
        resize();
        rendererBaseline_ = window_.device()->stats();
        events_.open(output_ / "events.csv");
        require(events_.good(), "Cannot create operation profile");
        events_ << "operation,wall_us\n";
        const auto rendererStart = Clock::now();
        view_ = std::make_unique<demo::StreamingDemoView>(*window_.device());
        require(view_->init(), "Cannot initialize streaming renderer");
        operation("renderer_init_with_terrain_pipeline", rendererStart);
        preparedPipelineCount_ = window_.device()->stats().pipelineCount;
        world_ = std::make_unique<world::WorldStreamer>(demo::streamingDemoConfig());
        pendingEdits_ = !edits_.isEmpty();
        csv_.open(output_ / "frames.csv");
        require(csv_.good(), "Cannot create frame profile");
        csv_ << "phase,frame,frame_interval_us,frame_work_us,stream_us,sync_us,render_submit_us,swap_us,"
                "planning_us,publication_us,mesh_schedule_us,cpu_chunks,gpu_chunks,uploads,retires,"
                "upload_bytes,draws,visible,gpu_buffer_bytes,engine_allocations,engine_allocated_bytes,"
                "terrain_render_us,terrain_record_us,material_pipeline_us,graph_execute_us,hud_us,pipeline_count,"
                "stream_sync_us,stream_sync_over_budget_us,upload_deferred,upload_budget_deferred,upload_progress_after_deadline,io_poll_us,io_busy,replay_us,reload_gpu_retired,reload_gpu_pending\n";
        SDL_SetRelativeMouseMode(verify ? SDL_FALSE : SDL_TRUE);
    }

    ~Application() {
        window_.device()->waitIdle();
        view_.reset();
        if (target_.isValid()) (void)window_.device()->destroyFramebuffer(target_);
        if (overlay_.isValid()) (void)window_.device()->destroyFramebuffer(overlay_);
    }

    auto frame(demo::FreeCamera& camera, const char* phase, const char* capture = nullptr) -> Bytes {
        const auto start = Clock::now();
        const double interval = lastStart_ == Clock::time_point{} ? 0.0 : micros(start - lastStart_);
        lastStart_ = start;
        memory::heapTracker().beginFrame();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                if (verify_) throw std::runtime_error("Verification window closed");
                running_ = false;
                break;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                if (verify_) throw std::runtime_error("Verification interrupted");
                running_ = false;
            }
            if (verify_) continue;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) resize();
            if (event.type != SDL_KEYDOWN || event.key.repeat != 0) continue;
            try {
                switch (event.key.keysym.sym) {
                case SDLK_h: camera = homeCamera(); break;
                case SDLK_e: {
                    const auto home = world_->chunk(demo::kEditedChunk);
                    if (home && home->state >= world::ChunkState::CellsReady) edit();
                    else std::puts("Press H and wait for the home chunk before editing.");
                    break;
                }
                case SDLK_F5: save(); break;
                case SDLK_F9: reload(); break;
                default: break;
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Operation rejected; current world retained: %s\n", error.what());
            }
        }
        if (!verify_) {
            SDL_MouseMotionEvent mouse{};
            SDL_GetRelativeMouseState(&mouse.xrel, &mouse.yrel);
            const auto* keys = SDL_GetKeyboardState(nullptr);
            const float dt = static_cast<float>(std::clamp(interval / 1e6, 0.0, 0.05));
            camera.update(dt, keys, &mouse, keys[SDL_SCANCODE_LSHIFT] != 0);
        }
        const auto beforeIo = Clock::now();
        pollIo(camera);
        const auto beforeStream = Clock::now();
        const auto streamed = world_->update(diagnosticAnchor_.value_or(camera.position));
        double replayUs = 0;
        const auto home = pendingEdits_ ? world_->chunk(demo::kEditedChunk) : std::nullopt;
        if (home && home->state >= world::ChunkState::CellsReady) {
            const auto replayStart = Clock::now();
            require(demo::applyStreamingDemoEdits(*world_, edits_), "Cannot replay saved edits");
            replayUs = micros(Clock::now() - replayStart);
            pendingEdits_ = false;
        }
        const auto beforeSync = Clock::now();
        require(view_->sync(*world_, 2, beforeStream + demo::kStreamingDemoFrameBudget),
                "GPU chunk upload failed; prior mesh retained");
        const auto beforeRender = Clock::now();
        draw(camera, capture != nullptr);
        const auto beforeCapture = Clock::now();
        Bytes pixels;
        if (capture != nullptr) pixels = captureImage(capture);
        const auto beforeSwap = Clock::now();
        window_.window()->swap();
        const auto end = Clock::now();
        memory::heapTracker().endFrame();
        require(world_->stats().failedCompletions == 0, "Streaming worker failure");
        const auto gpu = window_.device()->stats();
        if (verify_) {
            require(gpu.pipelineCount == preparedPipelineCount_,
                    "Terrain created an unprepared pipeline during verification");
        }
        require(gpu.shaderFailed == 0 && gpu.shaderWarnings == 0, "Shader diagnostics are not clean");
        require(glGetError() == GL_NO_ERROR, "OpenGL error during streaming frame");
        const auto upload = view_->uploadStats();
        const auto rendered = view_->renderStats();
        const auto allocations = memory::heapTracker().frame();
        // Capture readback/writing deliberately does not contaminate profile rows.
        if (capture == nullptr) {
            csv_ << phase << ',' << frames_ << ',' << interval << ',' << micros(end - start) << ','
                 << micros(beforeSync - beforeStream) << ',' << micros(beforeRender - beforeSync) << ','
                 << micros(beforeCapture - beforeRender) << ',' << micros(end - beforeSwap) << ','
                 << micros(streamed.timings.planning) << ',' << micros(streamed.timings.publication) << ','
                 << micros(streamed.timings.mesh) << ',' << world_->residentCount() << ',' << view_->residentCount() << ','
                 << upload.uploaded << ',' << upload.retired << ',' << upload.bytes << ','
                 << rendered.draws << ',' << rendered.visible << ',' << gpu.bufferBytes << ','
                 << allocations.allocations << ',' << allocations.bytesAllocated << ','
                 << terrainRenderUs_ << ',' << rendered.mainRecordUs << ',' << rendered.materialPipelineUs << ','
                 << rendered.graphExecuteUs << ',' << hudUs_ << ',' << gpu.pipelineCount << ','
                 << micros(beforeRender - beforeStream) << ','
                 << std::max(0.0, micros(beforeRender - beforeStream) -
                                      static_cast<double>(demo::kStreamingDemoFrameBudget.count())) << ','
                 << upload.deferred << ',' << upload.budgetDeferred << ','
                 << upload.progressAfterDeadline << ',' << micros(beforeStream - beforeIo) << ','
                 << io_.busy() << ',' << replayUs << ',' << upload.reloadRetired << ','
                 << view_->pendingRetirements() << '\n';
            require(csv_.good(), "Cannot write frame profile");
        } else {
            lastStart_ = {}; // Next interval must not include screenshot readback.
        }
        ++frames_;
        window_.device()->resetFrameStats();
        return pixels;
    }

    void settle(demo::FreeCamera& camera, const char* phase) {
        for (unsigned frameIndex = 0; frameIndex < 1200; ++frameIndex) {
            (void)frame(camera, phase);
            if (!io_.busy() && !pendingEdits_ && world_->residentCount() == expectedResidents_ && view_->caughtUp(*world_)) return;
        }
        throw std::runtime_error("Streaming/GPU residency did not converge");
    }

    void edit() {
        require(!reloadPending_, "Wait for reload before editing (current world retained)");
        const auto start = Clock::now();
        const auto patch = demo::streamingDemoPatch();
        require(demo::applyStreamingDemoEdits(*world_, patch), "Cannot edit home terrain");
        edits_ = patch;
        pendingEdits_ = false;
        operation("edit", start);
    }

    void save() {
        const auto start = Clock::now();
        require(io_.save(output_ / "world.f4sd", edits_), "Save/reload worker busy; request not queued");
        ioStarted_ = start;
        operation("save_enqueue", start);
    }

    void reload() {
        const auto start = Clock::now();
        require(view_->pendingRetirements() == 0, "GPU reload cleanup busy; request not queued");
        require(io_.load(output_ / "world.f4sd"), "Save/reload worker busy; request not queued");
        reloadPending_ = true;
        ioStarted_ = start;
        operation("reload_enqueue", start);
    }

    void verify() {
        auto camera = homeCamera();
        settle(camera, "cold");
        const auto original = checkpoint(camera, "00_home");
        edit();
        save();
        waitIo(camera, "save_pending");
        save(); // Replacement of an existing demo save must also work.
        waitIo(camera, "save_pending");
        settle(camera, "edit");
        const auto edited = checkpoint(camera, "01_edited");
        require(different(original, edited) > 100, "Edit did not visibly affect the rendered terrain");
        travel(camera, 0.f, 512.f, "outbound");
        settle(camera, "away");
        require(!world_->chunk(demo::kEditedChunk), "Home chunk did not unload");
        (void)checkpoint(camera, "02_away");
        travel(camera, 512.f, 0.f, "inbound");
        settle(camera, "return");
        const auto sample = world_->sampleHeightTerrain(32.f, 32.f);
        require(sample && sample->height == 38.f, "Edit lost after unload/reload");
        const auto returned = checkpoint(camera, "03_returned");
        require(different(edited, returned) == 0, "Returned view differs from edited view");
        reload();
        waitIo(camera, "reload_pending");
        settle(camera, "disk_reload");
        const auto restored = checkpoint(camera, "04_reloaded");
        require(different(edited, restored) == 0, "Fresh-world disk reload differs from edited view");
        finishVerification();
        std::puts("PASS: 25 CPU/GPU chunks; visible edit; unload/return; fresh-world disk reload; stable captures; clean GPU teardown.");
    }

    void verifyLoaded() {
        auto camera = homeCamera();
        settle(camera, "process_reload");
        (void)checkpoint(camera, "loaded");
        finishVerification();
        std::puts("PASS: separate-process saved-world load and clean GPU teardown.");
    }

    void verifyPolicies() {
        diagnosticAnchor_ = math::Vec3{};
        expectedResidents_ = 9;
        auto config = demo::streamingDemoConfig();
        config.horizontalRadius = 1;
        config.policyParams.hex.radius = 2;
        auto camera = homeCamera();
        camera.position = {16.f, 105.f, 130.f};
        camera.pitch = std::atan2(90.f, 114.f);
        for (const auto policy : {world::PolicyKind::Voxel, world::PolicyKind::Hex}) {
            require(view_->beginReload(*world_), "Previous diagnostic GPU cleanup not drained");
            config.defaultPolicy = policy;
            world_ = std::make_unique<world::WorldStreamer>(config);
            const bool voxel = policy == world::PolicyKind::Voxel;
            diagnosticCaption_ = voxel ? "VOXEL: nine chunks; complete tops and side faces; no internal cracks."
                                       : "HEX: nine tiled chunks; continuous tops/undersides; material markers; outer walls only.";
            if (!voxel) {
                camera.position = {0.f, 35.f, 45.f};
                camera.pitch = std::atan2(35.f, 45.f);
            }
            settle(camera, "policy_cold");
            if (!voxel) {
                for (const auto& chunk : world_->residentChunks()) {
                    for (std::uint8_t terrain = 1; terrain <= 3; ++terrain) {
                        require(world_->edit(chunk.key, 2u + 5u * terrain,
                            world::CellValue::hexValue({terrain, 0})), "Cannot color diagnostic hex cell");
                    }
                }
                settle(camera, "hex_materials");
            }
            require(view_->renderStats().draws == 9, "Not all diagnostic chunks were drawn");
            const auto original = checkpoint(camera, voxel ? "voxel_home" : "hex_home");
            diagnosticAnchor_ = math::Vec3{512.f, 0.f, 0.f};
            settle(camera, "policy_away");
            require(!world_->chunk({policy, {}}), "Policy home chunk did not unload");
            diagnosticAnchor_ = math::Vec3{};
            settle(camera, "policy_return");
            const auto returned = checkpoint(camera, voxel ? "voxel_returned" : "hex_returned");
            require(different(original, returned) == 0, "Policy unload/return changed the settled image");
            auto reverse = camera;
            if (voxel) {
                reverse.position = {16.f, 105.f, -100.f};
                reverse.yaw = -1.5707963267948966f;
                reverse.pitch = std::atan2(90.f, 116.f);
            } else {
                reverse.position = {0.f, -30.f, 40.f};
                reverse.pitch = std::atan2(-25.f, 40.f);
            }
            (void)checkpoint(reverse, voxel ? "voxel_reverse" : "hex_under");
            if (voxel) {
                diagnosticAnchor_ = math::Vec3{0.f, 64.f, 0.f};
                settle(camera, "voxel_empty");
                require(view_->renderStats().draws == 0, "Empty voxel chunks still draw old terrain");
                for (const auto& chunk : world_->residentChunks()) {
                    require(world_->mesh(chunk.key)->indices.isEmpty(), "Air fixture contains geometry");
                }
                const auto* previousCaption = diagnosticCaption_;
                diagnosticCaption_ = "EMPTY VOXEL: nine resident air chunks; zero terrain draws; background only is correct.";
                const auto empty = checkpoint(camera, "voxel_empty");
                diagnosticCaption_ = previousCaption;
                require(different(original, empty) > 100, "Empty voxel transition did not remove terrain");
                diagnosticAnchor_ = math::Vec3{};
                settle(camera, "voxel_ground_return");
            }
        }

        diagnosticAnchor_.reset();
        diagnosticCaption_ = "HEX CAMERA ROUTE: camera drives residency; tiled terrain follows travel and edits survive return.";
        auto routeCamera = homeCamera();
        routeCamera.position = {0.f, 30.f, 0.5f};
        routeCamera.pitch = std::atan2(30.f, 0.5f);
        settle(routeCamera, "hex_camera_home");
        const auto routeHome = checkpoint(routeCamera, "hex_camera_home");
        const auto destination = world_->meshOrigin({world::PolicyKind::Hex, {12, 0, -8}});
        for (unsigned step = 1; step <= 120; ++step) {
            const float t = static_cast<float>(step) / 120.f;
            routeCamera.position = {destination.x * t, 30.f, destination.z * t + 0.5f};
            (void)frame(routeCamera, "hex_camera_outbound");
        }
        settle(routeCamera, "hex_camera_away");
        require(world_->chunk({world::PolicyKind::Hex, {12, 0, -8}}).has_value(), "Camera missed the rendered destination chunk");
        require(!world_->chunk({world::PolicyKind::Hex, {}}), "Camera route did not unload home");
        require(view_->renderStats().draws == 9, "Camera route lost visible terrain");
        (void)checkpoint(routeCamera, "hex_camera_away");
        for (unsigned step = 1; step <= 120; ++step) {
            const float t = 1.f - static_cast<float>(step) / 120.f;
            routeCamera.position = {destination.x * t, 30.f, destination.z * t + 0.5f};
            (void)frame(routeCamera, "hex_camera_inbound");
        }
        settle(routeCamera, "hex_camera_return");
        require(different(routeHome, checkpoint(routeCamera, "hex_camera_returned")) == 0,
                "Camera-driven hex return changed the edited view");
        diagnosticAnchor_ = math::Vec3{};

        require(view_->beginReload(*world_), "Diagnostic GPU cleanup not drained");
        config.defaultPolicy = world::PolicyKind::Height;
        config.horizontalRadius = 0;
        expectedResidents_ = 1;
        world_ = std::make_unique<world::WorldStreamer>(config);
        camera = homeCamera();
        camera.position = {24.f, 110.f, 130.f};
        camera.pitch = std::atan2(100.f, 114.f);
        diagnosticCaption_ = "LIVE REGION: fixed camera/anchor; height -> voxel -> hex -> height; old policy must disappear.";
        settle(camera, "region_height");
        const auto height = checkpoint(camera, "region_height");
        Bytes voxelImage;
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            for (const auto policy : {world::PolicyKind::Voxel, world::PolicyKind::Hex}) {
                world_->regions().set({0, 0}, policy);
                settle(camera, "region_switch");
                require(world_->chunk({policy, {}}).has_value(), "Live region failed to replace policy");
                require(!world_->chunk(demo::kEditedChunk), "Old height policy survived region switch");
                require(view_->renderStats().draws == 1, "Region switch did not draw exactly one chunk");
                const auto name = std::string(policy == world::PolicyKind::Voxel ? "region_voxel_" : "region_hex_") + std::to_string(iteration);
                const auto pixels = checkpoint(camera, name.c_str());
                require(different(height, pixels) > 100, "Region policy did not visibly change");
                if (policy == world::PolicyKind::Voxel) voxelImage = pixels;
                else require(different(voxelImage, pixels) > 100, "Hex switch retained voxel geometry");
            }
            world_->regions().erase({0, 0});
            settle(camera, "region_restore");
            const auto name = "region_restored_" + std::to_string(iteration);
            const auto restored = checkpoint(camera, name.c_str());
            require(different(height, restored) == 0, "Restoring default policy changed height geometry");
        }
        verifyMixedPolicies();
        verifyMeterCoverage();
        verifyRegionalSurfaces();
        verifyRegionalSurfaces(true);
        verifyVoxelCuts();
        finishVerification();
        std::puts("PASS: policy diagnostics; mixed-policy mutation/return; tiled hex camera route; voxel/hex unload-return; empty voxel retirement; three live region cycles; clean GPU teardown. Mixed-policy seams remain OPEN.");
    }

    void colorMixedHex() {
        for (const auto& chunk : world_->residentChunks()) {
            if (chunk.key.policy != world::PolicyKind::Hex) continue;
            const auto snapshot = world_->snapshot(chunk.key);
            require(snapshot.has_value(), "Missing mixed hex snapshot");
            const auto* tile = std::get_if<world::HexTile>(&snapshot->base);
            require(tile != nullptr, "Mixed hex snapshot has wrong base");
            for (std::uint32_t index = 0; index < tile->cells.size(); ++index) {
                if (tile->cells[index].terrainType == world::kInvalidTerrain) continue;
                require(world_->edit(chunk.key, index, world::CellValue::hexValue({0, 0})),
                        "Cannot color mixed hex diagnostic");
            }
        }
    }

    void verifyMixedPolicies() {
        require(view_->beginReload(*world_), "Diagnostic GPU cleanup not drained");
        world_ = std::make_unique<world::WorldStreamer>(demo::streamingMixedDemoConfig());
        demo::setStreamingMixedRegions(world_->regions());
        expectedResidents_ = 11; // Seven height, two voxel, two hex chunks.
        diagnosticAnchor_ = math::Vec3{};
        diagnosticCaption_ = "MIXED: green HEIGHT / white VOXEL / sand HEX. Regional gaps/overlaps remain OPEN.";
        auto camera = homeCamera();
        camera.position = {16.f, 175.f, 200.f};
        camera.pitch = std::atan2(165.f, 190.f);
        settle(camera, "mixed_cold");
        colorMixedHex();
        settle(camera, "mixed_markers");
        require(view_->renderStats().draws == 11, "Mixed fixture lost a policy draw");
        const auto original = checkpoint(camera, "mixed_home");
        auto reverse = camera;
        reverse.position = {16.f, 150.f, -150.f};
        reverse.yaw = -1.5707963267948966f;
        reverse.pitch = std::atan2(140.f, 160.f);
        (void)checkpoint(reverse, "mixed_reverse");
        auto overhead = camera;
        overhead.position = {32.f, 290.f, 32.5f};
        overhead.pitch = std::atan2(290.f, 0.5f);
        (void)checkpoint(overhead, "mixed_overhead");
        auto close = camera;
        close.position = {0.f, 60.f, 95.f};
        close.pitch = std::atan2(50.f, 85.f);
        (void)checkpoint(close, "mixed_boundary");

        // A centroid audit detects encroachment, not all holes or intersections.
        std::ofstream audit(output_ / "mixed_boundary_audit.csv");
        audit << "policy,x,y,z,top_triangles,foreign_region_centroids\n";
        for (const auto& chunk : world_->residentChunks()) {
            const auto* mesh = world_->mesh(chunk.key);
            require(mesh != nullptr, "Mixed audit missing mesh");
            const auto origin = world_->meshOrigin(chunk.key);
            std::size_t top = 0, foreign = 0;
            for (std::size_t index = 0; index + 2 < mesh->indices.size(); index += 3) {
                const auto& a = mesh->vertices[mesh->indices[index]];
                const auto& b = mesh->vertices[mesh->indices[index + 1]];
                const auto& c = mesh->vertices[mesh->indices[index + 2]];
                if (a.normal.y + b.normal.y + c.normal.y <= 1.5f) continue;
                ++top;
                const auto x = static_cast<std::int32_t>(std::floor(origin.x + (a.position.x + b.position.x + c.position.x) / 3.f));
                const auto z = static_cast<std::int32_t>(std::floor(origin.z + (a.position.z + b.position.z + c.position.z) / 3.f));
                if (world_->regions().policyAt(x, z) != chunk.key.policy) ++foreign;
            }
            audit << static_cast<unsigned>(chunk.key.policy) << ',' << chunk.key.coord.x << ','
                  << chunk.key.coord.y << ',' << chunk.key.coord.z << ',' << top << ',' << foreign << '\n';
        }
        audit.flush();
        require(audit.good(), "Cannot write mixed boundary audit");
        for (unsigned step = 0; step < 90; ++step) {
            demo::setStreamingMixedRegions(world_->regions(), step);
            diagnosticAnchor_ = math::Vec3{step % 2 == 0 ? 0.f : 512.f, 0.f, 0.f};
            (void)frame(camera, "mixed_mutation"); // No settle or worker wait between revisions.
        }
        demo::setStreamingMixedRegions(world_->regions());
        diagnosticAnchor_ = math::Vec3{};
        settle(camera, "mixed_restore");
        require(view_->renderStats().draws == 11, "Mixed restore lost a policy draw");
        require(different(original, checkpoint(camera, "mixed_restored")) == 0,
                "Mixed-policy mutation/return changed the settled image");
    }

    void verifyMeterCoverage() {
        require(view_->beginReload(*world_), "Diagnostic GPU cleanup not drained");
        auto config = demo::streamingMixedDemoConfig();
        config.horizontalHalfExtentMeters = 96.f;
        config.chunkCapacityPerPolicy = 25; // Fixed map/anchor: exactly 20, never a mutation burst.
        world_ = std::make_unique<world::WorldStreamer>(config);
        demo::setStreamingMixedRegions(world_->regions());
        expectedResidents_ = 20;
        diagnosticAnchor_ = math::Vec3{};
        diagnosticCaption_ = "METER FOOTPRINT: +/-96m chunk centers; full white voxel region. Cross-policy seams remain OPEN.";
        auto camera = homeCamera();
        camera.position = {16.f, 175.f, 200.f};
        camera.pitch = std::atan2(165.f, 190.f);
        settle(camera, "meters_cold");
        colorMixedHex();
        settle(camera, "meters_markers");
        require(view_->renderStats().draws == 20, "Meter footprint did not draw all 20 chunks");
        require(world_->chunk({world::PolicyKind::Voxel, {-2, 0, 0}}).has_value() &&
                world_->chunk({world::PolicyKind::Voxel, {-2, 0, 1}}).has_value(),
                "Meter footprint still omits half the voxel region");
        (void)checkpoint(camera, "meters_home");
        auto overhead = camera;
        overhead.position = {32.f, 350.f, 32.5f};
        overhead.pitch = std::atan2(350.f, 0.5f);
        (void)checkpoint(overhead, "meters_overhead");
        auto close = camera;
        close.position = {0.f, 60.f, 95.f};
        close.pitch = std::atan2(50.f, 85.f);
        (void)checkpoint(close, "meters_boundary");
        // Offline evidence against the same published geometry as these views.
        // These probes do not imply collision/GPU synchronization during uploads.
        std::ofstream probes(output_ / "meter_surface_probes.csv");
        probes.precision(17);
        probes << "ray_x,ray_z,result,policy,chunk_x,chunk_y,chunk_z,triangle,distance,hit_y,normal_y\n";
        unsigned probeIndex = 0;
        constexpr world::PolicyKind expected[]{world::PolicyKind::Height, world::PolicyKind::Voxel, world::PolicyKind::Hex};
        for (const auto point : {math::Vec3{-80, 100, -80}, math::Vec3{-48.5f, 100, 16.5f},
                                 math::Vec3{12, 100, 12}, math::Vec3{8, 100, 56}, math::Vec3{0, 100, 32}}) {
            const auto hit = world_->raycastPublishedTerrain(point, {0, -1, 0}, 200.f, true);
            probes << point.x << ',' << point.z;
            if (probeIndex < 3) require(hit && hit->key.policy == expected[probeIndex], "Surface probe missed its diagnostic policy");
            ++probeIndex;
            if (!hit) {
                probes << ",miss,,,,,,,,\n";
                continue;
            }
            const auto chunk = world_->chunk(hit->key);
            require(chunk && hit->handle == chunk->handle && hit->meshVersion == chunk->meshVersion &&
                    hit->meshVersion == chunk->version, "Settled surface probe has stale mesh provenance");
            const auto repeat = world_->raycastPublishedTerrain(point, {0, -1, 0}, 200.f, true);
            require(repeat && repeat->key == hit->key && repeat->surface.triangleIndex == hit->surface.triangleIndex &&
                    repeat->surface.distance == hit->surface.distance, "Repeated surface probe changed");
            probes << ",hit," << static_cast<unsigned>(hit->key.policy) << ',' << hit->key.coord.x << ','
                   << hit->key.coord.y << ',' << hit->key.coord.z << ',' << hit->surface.triangleIndex << ','
                   << hit->surface.distance << ',' << hit->surface.position.y << ',' << hit->surface.normal.y << '\n';
        }
        probes.flush();
        require(probes.good(), "Cannot write surface probe evidence");
    }

    void verifyRegionalSurfaces(bool cutWalls = false) {
        require(view_->beginReload(*world_), "Diagnostic GPU cleanup not drained");
        auto config = demo::streamingMixedDemoConfig();
        config.horizontalHalfExtentMeters = 96.f;
        config.chunkCapacityPerPolicy = 25;
        config.regionalSurfaceOwnership = true;
        config.regionalHexCutWalls = cutWalls;
        // Corner-sweep residency ticks use the per-frame work caps. Interactive
        // pacing keeps the wall-clock cooperative budget. Not a hard deadline.
        if (cutWalls) config.deterministicAdmission = true;
        world_ = std::make_unique<world::WorldStreamer>(config);
        demo::setStreamingMixedRegions(world_->regions());
        expectedResidents_ = 22; // 14 height, four voxel, four intersecting hex bounds.
        diagnosticAnchor_ = math::Vec3{};
        diagnosticCaption_ = cutWalls ? "HEX CUT WALLS: closed to the actual prism floor. Height bridges / voxel cuts remain OPEN." :
            "REGIONAL SURFACES: intersecting bounds + CPU clipping. Cut walls / height transitions remain OPEN.";
        auto camera = homeCamera();
        camera.position = {16.f, 175.f, 200.f};
        camera.pitch = std::atan2(165.f, 190.f);
        settle(camera, "owned_cold");
        colorMixedHex();
        settle(camera, "owned_markers");
        require(view_->renderStats().draws == 22, "Regional surface fixture lost a draw");
        (void)checkpoint(camera, cutWalls ? "capped_home" : "owned_home");
        auto overhead = camera;
        overhead.position = {32.f, 350.f, 32.5f};
        overhead.pitch = std::atan2(350.f, 0.5f);
        if (cutWalls) { overhead.position = {92, -3, 32}; overhead.yaw = 0; overhead.pitch = 0; }
        (void)checkpoint(overhead, cutWalls ? "capped_east" : "owned_overhead");
        auto close = camera;
        close.position = {0.f, 60.f, 95.f};
        close.pitch = std::atan2(50.f, 85.f);
        if (cutWalls) { close.position = {86, -3, 86}; close.yaw = 0.7853981633974483f; close.pitch = 0; }
        (void)checkpoint(close, cutWalls ? "capped_corner" : "owned_boundary");
        std::ofstream probes(output_ / (cutWalls ? "capped_surface_probes.csv" : "owned_surface_probes.csv"));
        probes.precision(17);
        probes << "x,z,policy,chunk_x,chunk_z,triangle,distance,hit_y\n";
        for (int z = 4; z < 64; z += 8) {
            for (int x = 4; x < 64; x += 8) {
                const auto hit = world_->raycastPublishedTerrain({float(x), 100, float(z)}, {0, -1, 0}, 200, true);
                require(hit && hit->key.policy == world::PolicyKind::Hex, "Regional hex coverage probe missed");
                require(hit->meshRegionRevision == world_->regions().revision(), "Regional surface probe used stale ownership");
                probes << x << ',' << z << ',' << static_cast<unsigned>(hit->key.policy) << ',' << hit->key.coord.x << ','
                       << hit->key.coord.z << ',' << hit->surface.triangleIndex << ',' << hit->surface.distance << ','
                       << hit->surface.position.y << '\n';
            }
        }
        require(world_->raycastPublishedTerrain({8, 100, 56}, {0, -1, 0}, 200, true).has_value(),
                "Regional fixture retained the old observed gap");
        probes.flush();
        require(probes.good(), "Cannot write regional surface evidence");
        if (cutWalls) {
            auditHeightHexProfiles();
            std::ofstream walls(output_ / "capped_wall_probes.csv");
            walls.precision(17);
            walls << "side,t,chunk_x,chunk_z,triangle,distance,normal_x,normal_z\n";
            for (unsigned side = 0; side < 4; ++side) {
                const math::Vec3 normal = side == 0 ? math::Vec3{-1, 0, 0} : side == 1 ? math::Vec3{1, 0, 0} :
                    side == 2 ? math::Vec3{0, 0, -1} : math::Vec3{0, 0, 1};
                for (int t = 4; t < 64; t += 8) {
                    const math::Vec3 p = side == 0 ? math::Vec3{0, -4, float(t)} : side == 1 ? math::Vec3{64, -4, float(t)} :
                        side == 2 ? math::Vec3{float(t), -4, 0} : math::Vec3{float(t), -4, 64};
                    const auto hit = world_->raycastPublishedTerrain(p + normal * 2, normal * -1, 3, true);
                    require(hit && hit->key.policy == world::PolicyKind::Hex && hit->surface.normal == normal &&
                            std::abs(hit->surface.distance - 2) < 1e-5 && hit->meshRegionRevision == world_->regions().revision(),
                            "Hex cut wall probe missed or has incorrect winding/provenance");
                    walls << side << ',' << t << ',' << hit->key.coord.x << ',' << hit->key.coord.z << ',' << hit->surface.triangleIndex
                          << ',' << hit->surface.distance << ',' << hit->surface.normal.x << ',' << hit->surface.normal.z << '\n';
                }
            }
            walls.flush();
            require(walls.good(), "Cannot write cut wall evidence");
            verifyCornerPublication();
        }
    }

    void verifyCornerPublication() {
        const std::array requests{
            world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, 64}},
            world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, 64}}};
        std::array<world::StreamingBoundarySnapshot, 2> captures;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto captured = world_->captureBoundary(requests[i]);
            require(captured.hasValue(), "Cannot capture displayed corner dependencies");
            captures[i] = std::move(captured).value();
        }
        auto built = world::StreamingBoundaryComposition::build(captures);
        require(built.hasValue() && world_->boundaryCurrent(built.value()), "Cannot build current displayed corner");
        auto camera = homeCamera(); camera.position = {40, 10, 40}; camera.yaw = -2.356194490192345f; camera.pitch = 0;
        const auto* oldCaption = diagnosticCaption_;
        diagnosticCaption_ = "CORNER BEFORE: original skirts leave the Height/Hex height discontinuity open.";
        const auto before = checkpoint(camera, "corner_before");
        require(view_->installBoundaryComposition(*world_, built.value()), "Cannot stage and install corner GPU/query replacement");
        diagnosticCaption_ = "CORNER AFTER: composed tops + transition walls; originals suppressed. Frozen render/query snapshot.";
        const auto after = checkpoint(camera, "corner_after");
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(width_) * 64 * 4; i < after.size(); i += 4)
            if (before[i] != after[i] || before[i + 1] != after[i + 1] || before[i + 2] != after[i + 2]) ++changed;
        require(changed > 1000, "Corner installation changed only the HUD, not visible terrain");
        auto close = camera; close.position = {38, 40, 38};
        close.pitch = std::atan2(30.f, std::sqrt(26.f * 26.f * 2));
        (void)checkpoint(close, "corner_close");
        auto east = camera; east.position = {48, 10, 32}; east.yaw = -1.5707963267948966f;
        (void)checkpoint(east, "corner_east");
        auto north = camera; north.position = {32, 10, 48}; north.yaw = 3.141592653589793f;
        (void)checkpoint(north, "corner_north");
        std::ofstream probes(output_ / "corner_display_queries.csv");
        probes << "axis,t,composed_triangle,source_triangle,source_policy,source_x,source_z,distance\n";
        unsigned checks = 0;
        for (unsigned axis = 0; axis < 2; ++axis) {
            for (int t = 4; t < 64; t += 4) {
                const math::Vec3 eye = axis == 0 ? math::Vec3{62, 10, float(t)} : math::Vec3{float(t), 10, 62};
                const math::Vec3 direction = axis == 0 ? math::Vec3{1, 0, 0} : math::Vec3{0, 0, 1};
                const auto hit = view_->raycastDisplayedTerrain(eye, direction, 4, true);
                require(hit && hit->boundary == axis && std::abs(hit->source.surface.distance - 2) < 0.00001,
                        "Displayed transition disagrees with the installed query geometry");
                probes << axis << ',' << t << ',' << hit->composedTriangle << ',' << hit->source.surface.triangleIndex << ','
                    << static_cast<unsigned>(hit->source.key.policy) << ',' << hit->source.key.coord.x << ','
                    << hit->source.key.coord.z << ',' << hit->source.surface.distance << '\n';
                ++checks;
            }
        }
        require(checks == 30, "Displayed corner probe coverage changed");
        probes.flush(); require(probes.good(), "Cannot write displayed corner query evidence");
        for (unsigned step = 0; step < 4; ++step) {
            auto moving = camera;
            moving.position = {40.f + step * 4.f, 10, 48.f - step * 2.f};
            moving.yaw = std::atan2(moving.position.x - 64, moving.position.z - 64);
            const auto name = "corner_motion_" + std::to_string(step);
            (void)checkpoint(moving, name.c_str());
        }
        view_->removeBoundaryComposition(*world_);
        diagnosticCaption_ = "CORNER BEFORE: original skirts leave the Height/Hex height discontinuity open.";
        require(different(before, checkpoint(camera, "corner_restored")) == 0, "Removing corner composition did not restore original rendering");
        verifyLiveCornerPublication(requests, close);
        diagnosticCaption_ = oldCaption;
    }

    void verifyLiveCornerPublication(std::span<const world::StreamingBoundaryRequest> requests, demo::FreeCamera camera) {
        camera.position = {38, 80, 38};
        camera.pitch = std::atan2(70.f, std::sqrt(26.f * 26.f * 2));
        jobs::JobSystem pool(1);
        world::BoundaryCompositionJob job(pool);
        require(job.request(requests), "Cannot request live corner composition");
        std::ofstream reuseEvidence(output_ / "corner_reuse.csv");
        require(bool(reuseEvidence), "Cannot write corner reuse evidence");
        reuseEvidence << "phase,residents,reused,uploaded,full_reupload_units\n";
        auto previousResidents = world_->residentChunks();
        std::uint32_t uploaded = 0, reused = 0;
        const auto account = [&] {
            uploaded += view_->uploadStats().uploaded; reused += view_->uploadStats().reused;
        };
        const auto verifyReuse = [&](const char* phase) {
            const auto current = world_->residentChunks();
            std::uint32_t expected = 0;
            for (const auto& chunk : current) {
                if (std::any_of(previousResidents.begin(), previousResidents.end(), [&](const auto& old) {
                    return old.key == chunk.key && old.handle == chunk.handle && old.meshVersion == chunk.meshVersion &&
                        old.meshRegionRevision == chunk.meshRegionRevision;
                })) ++expected;
            }
            require(expected > 0 && reused == expected && uploaded == current.size() - expected + 3,
                "Live corner did not reuse exactly the unchanged resident versions");
            reuseEvidence << phase << ',' << current.size() << ',' << reused << ',' << uploaded << ',' << current.size() + 3 << '\n';
            previousResidents = current; uploaded = 0; reused = 0;
        };
        const auto converge = [&] {
            for (unsigned i = 0; i < 128; ++i) {
                world_->update({}); world_->waitForIdle();
                require(view_->syncBoundaryComposition(*world_, job), "Live corner build/upload failed");
                account();
                if (view_->hasBoundaryComposition() && view_->caughtUp(*world_)) return;
                pool.waitIdle();
            }
            throw std::runtime_error("Live corner replacement did not converge");
        };
        converge();
        verifyReuse("initial");
        diagnosticCaption_ = "LIVE CORNER: worker rebuild, staged whole-view swap, matching displayed-surface queries.";
        const auto before = checkpoint(camera, "corner_live_home");
        const world::StreamingChunkKey key{world::PolicyKind::Height, {0, 0, 1}};
        const auto index = world::heightSampleIndex(56, 0);
        const auto snapshot = world_->snapshot(key);
        require(snapshot.has_value(), "Missing live corner height source");
        const auto* tile = std::get_if<world::HeightTile>(&snapshot->base);
        require(tile != nullptr, "Unexpected live corner source policy");
        auto original = world::CellValue::heightValue(tile->samples[index]);
        for (const auto& delta : snapshot->deltas) if (delta.index == index) original = delta.value;
        auto edited = original; edited.height.height = 45;
        require(world_->edit(key, index, edited), "Cannot edit live corner");
        for (unsigned i = 0; i < 128; ++i) {
            world_->update({}); world_->waitForIdle();
            if (!world_->hasPendingWork()) break;
        }
        require(!world_->hasPendingWork(), "Edited source did not settle");
        require(!view_->caughtUp(*world_), "Old corner incorrectly reported current after edit");
        require(different(before, checkpoint(camera, "corner_live_pending")) == 0,
            "Pending corner exposed newer source geometry under an old transition");
        job.update(*world_); pool.waitIdle();
        require(job.update(*world_).state == world::BoundaryCompositionState::Ready, "Edited corner worker did not complete");
        require(view_->syncBoundaryComposition(*world_, job, {1, 0, {}}) && view_->boundaryUploadPending(),
            "Corner upload did not remain staged after a single work unit");
        account();
        require(different(before, checkpoint(camera, "corner_live_staging")) == 0,
            "Partially staged corner became visible before commit");
        converge();
        verifyReuse("edited");
        const auto after = checkpoint(camera, "corner_live_edited");
        require(different(before, after) > 100, "Live corner edit did not change the displayed geometry");
        const auto hit = view_->raycastDisplayedTerrain({56, 100, 64}, {0, -1, 0}, 200);
        require(hit && std::abs(hit->source.surface.position.y - 45) < 0.00001,
            "Live corner query did not follow the displayed edit");
        require(world_->edit(key, index, original), "Cannot restore live corner edit");
        converge();
        verifyReuse("restored");
        reuseEvidence.close(); require(bool(reuseEvidence), "Cannot finish corner reuse evidence");
        require(different(before, checkpoint(camera, "corner_live_restored")) == 0,
            "Live corner restoration changed the original composed image");
        require(job.stats().accepted == 3, "Live corner did not accept exactly initial/edit/restored compositions");
        verifyCornerTravel(job, pool, camera, before);
        view_->removeBoundaryComposition(*world_);
    }

    void verifyCornerTravel(world::BoundaryCompositionJob& job, jobs::JobSystem& pool,
        demo::FreeCamera camera, const Bytes& original) {
        std::ofstream movement(output_ / "corner_motion_residency.csv");
        require(bool(movement), "Cannot write continuous corner residency evidence");
        movement << "direction,step,anchor_x,anchor_z,residents,accepted_current,job_state,"
                    "probe_x,probe_z,probe_y,probe_status,probe_policy,probe_solid,record,tick\n";
        require(world_->deterministicAdmission(),
            "Corner residency ticks require fixed work-unit admission");
        const auto probeText = [&](double x, double y, double z) {
            const auto sample = world_->classifyPublishedSolid(x, y, z);
            if (!sample.hasValue()) {
                return std::to_string(static_cast<unsigned>(sample.error())) + ",,";
            }
            return std::string("ok,") + std::to_string(static_cast<unsigned>(sample.value().policy)) + "," +
                (sample.value().solid ? "1" : "0");
        };
        std::optional<std::pair<double, double>> hexColumn;
        for (int z = 4; z < 60 && !hexColumn; z += 8) {
            for (int x = 4; x < 60; x += 8) {
                const auto sample = world_->classifyPublishedSolid(x + 0.2, -8.0, z + 0.2);
                if (sample.hasValue() && sample.value().solid && sample.value().policy == world::PolicyKind::Hex) {
                    hexColumn = {x + 0.2, z + 0.2};
                    break;
                }
            }
        }
        require(hexColumn.has_value(), "Home travel has no stable published hex column");
        int stableHex = 0, droppedHex = 0, tickRows = 0, unadoptedEdit = 0, unadoptedRegion = 0;
        bool hexDropped = false;
        const auto noteHexColumn = [&](const char* direction, bool anchorRow) {
            const auto sample = world_->classifyPublishedSolid(hexColumn->first, -8.0, hexColumn->second);
            const bool outboundTravel = std::string(direction) == "outbound" || std::string(direction) == "away";
            const bool countAnchor = anchorRow &&
                (std::string(direction) == "outbound" || std::string(direction) == "home");
            if (!sample.hasValue()) {
                require(sample.error() == world::StreamingSolidError::NoPublishedSurface,
                    "Travel tick lost the hex column without unloading its published surface");
                if (outboundTravel) hexDropped = true;
                if (countAnchor) ++droppedHex;
            } else {
                require(sample.value().solid && sample.value().policy == world::PolicyKind::Hex,
                    "Resident hex column changed owner during a travel tick");
                if (outboundTravel) require(!hexDropped, "Outbound travel reloaded a column after unloading it");
                if (countAnchor) ++stableHex;
            }
            return probeText(hexColumn->first, -8.0, hexColumn->second);
        };
        const auto recordHexTick = [&](const char* direction, unsigned step, unsigned tick, double ax, double az) {
            const auto probe = noteHexColumn(direction, false);
            movement << direction << ',' << step << ',' << ax << ',' << az << ','
                << world_->residentCount() << ',' << (view_->acceptedSurfaceCurrent(*world_) ? 1 : 0) << ','
                << static_cast<unsigned>(job.status().state) << ','
                << hexColumn->first << ',' << hexColumn->second << ",-8," << probe
                << ",tick," << tick << '\n';
            ++tickRows;
        };
        movement << "home,0,0,0," << world_->residentCount() << ','
            << (view_->acceptedSurfaceCurrent(*world_) ? 1 : 0) << ",,"
            << hexColumn->first << ',' << hexColumn->second << ",-8," << noteHexColumn("home", true) << ",anchor,\n";
        const auto settleSources = [&](math::Vec3 anchor, const char* direction, unsigned step, unsigned& tick) {
            diagnosticAnchor_ = anchor;
            for (unsigned n = 0; n < 128; ++n) {
                world_->update(anchor); world_->waitForIdle();
                recordHexTick(direction, step, tick++, anchor.x, anchor.z);
                if (!world_->hasPendingWork() &&
                    world_->residentTerrainFootprint(world::PolicyKind::Height, world::PolicyKind::Hex).has_value()) return;
            }
            throw std::runtime_error("Retargeted terrain sources did not settle");
        };
        const auto converge = [&](bool composed) {
            for (unsigned tick = 0; tick < 128; ++tick) {
                require(view_->syncBoundaryComposition(*world_, job), "Retargeted terrain upload failed");
                if (view_->caughtUp(*world_) && view_->hasBoundaryComposition() == composed) return;
                pool.waitIdle();
            }
            throw std::runtime_error("Retargeted terrain view did not converge");
        };
        const auto walkSources = [&](math::Vec3 from, math::Vec3 to, const char* direction) {
            for (unsigned step = 1; step <= 8; ++step) {
                const float t = static_cast<float>(step) / 8.f;
                const math::Vec3 anchor = from * (1.f - t) + to * t;
                diagnosticAnchor_ = anchor;
                world_->update(anchor);
                const auto discover = job.requestResidentWindow(*world_, world::PolicyKind::Height, world::PolicyKind::Hex);
                if (discover) {
                    require(*discover == world::BoundaryDiscoveryError::NoResidentCoverage,
                        "Continuous resident boundary discovery failed");
                } else if (job.status().state != world::BoundaryCompositionState::NoBoundaries) {
                    require(view_->syncBoundaryComposition(*world_, job, {1, 0, {}}),
                        "Continuous resident upload failed");
                }
                pool.waitIdle();
                world_->waitForIdle();
                unsigned tick = 0;
                recordHexTick(direction, step, tick++, anchor.x, anchor.z);
                settleSources(anchor, direction, step, tick);
                const auto settledDiscovery = job.requestResidentWindow(*world_, world::PolicyKind::Height, world::PolicyKind::Hex);
                require(!settledDiscovery, "Settled continuous boundary discovery failed");
                if (job.status().state != world::BoundaryCompositionState::NoBoundaries) {
                    require(view_->syncBoundaryComposition(*world_, job, {1, 0, {}}),
                        "Settled continuous resident upload failed");
                }
                movement << direction << ',' << step << ',' << anchor.x << ',' << anchor.z << ','
                    << world_->residentCount() << ',' << (view_->acceptedSurfaceCurrent(*world_) ? 1 : 0) << ','
                    << static_cast<unsigned>(job.status().state) << ','
                    << hexColumn->first << ',' << hexColumn->second << ",-8," << noteHexColumn(direction, true)
                    << ",anchor,\n";
            }
        };
        walkSources({}, {512, 0, 512}, "outbound");
        require(stableHex >= 2, "Travel did not keep a resident hex column across camera steps");
        require(droppedHex >= 1, "Travel did not drop the hex column when its chunk unloaded");
        unsigned awayTick = 0;
        settleSources({512, 0, 512}, "away", 8, awayTick);
        require(!job.requestResidentWindow(*world_, world::PolicyKind::Height, world::PolicyKind::Hex),
            "Away resident boundary discovery failed");
        bool departureStaged = false;
        for (unsigned tick = 0; tick < 128 && !departureStaged; ++tick) {
            require(view_->syncBoundaryComposition(*world_, job, {1, 0, {}}),
                "Plain-terrain departure upload failed");
            departureStaged = view_->boundaryUploadPending();
        }
        require(departureStaged, "Plain-terrain departure was not staged");
        require(different(original, checkpoint(camera, "corner_departure_pending")) == 0,
            "Departure removed the displayed corner before plain terrain was ready");
        converge(false);
        auto away = homeCamera(); away.position.x += 512; away.position.z += 512;
        const auto plain = checkpoint(away, "corner_away_plain");
        const auto hit = view_->raycastDisplayedTerrain({544, 100, 544}, {0, -1, 0}, 200);
        require(hit && !hit->boundary && hit->source.key.policy == world::PolicyKind::Height,
            "Departed view did not publish plain height query geometry");
        const math::Vec3 awayAnchor{512, 0, 512};
        const world::StreamingChunkKey awayKey{world::PolicyKind::Height, {8, 0, 8}};
        require(world_->chunk(awayKey).has_value(), "Away travel is missing the height column chunk");
        const auto readHeight = [&](std::uint32_t index) {
            const auto snapshot = world_->snapshot(awayKey);
            require(snapshot.has_value(), "Away height column is not readable");
            const auto* tile = std::get_if<world::HeightTile>(&snapshot->base);
            require(tile != nullptr, "Away column chunk is not a height tile");
            auto value = world::CellValue::heightValue(tile->samples[index]);
            for (const auto& delta : snapshot->deltas) if (delta.index == index) value = delta.value;
            return value;
        };
        const std::uint32_t quad[]{world::heightSampleIndex(8, 8), world::heightSampleIndex(9, 8),
                                   world::heightSampleIndex(8, 9), world::heightSampleIndex(9, 9)};
        world::CellValue originalSamples[4];
        for (int i = 0; i < 4; ++i) originalSamples[i] = readHeight(quad[i]);
        const auto above = world_->classifyPublishedSolid(520.2, 200.0, 520.7);
        require(above.hasValue() && !above.value().solid && above.value().policy == world::PolicyKind::Height,
            "Away height column was not an empty sample above its published top");
        for (int i = 0; i < 4; ++i) {
            auto raised = originalSamples[i];
            raised.height.height = 400.f;
            require(world_->edit(awayKey, quad[i], raised), "Cannot edit the away height column");
        }
        const auto retained = world_->classifyPublishedSolid(520.2, 200.0, 520.7);
        require(retained.hasValue() && retained.value().solid == above.value().solid &&
                retained.value().policy == above.value().policy,
            "Height edit changed the published column before republication");
        ++unadoptedEdit;
        movement << "edit,retained,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,200," << probeText(520.2, 200.0, 520.7) << ",held,\n";
        // 0 raised height, 1 restored height, 2 hex owner, 3 height owner.
        const auto holdsPrevious = [](const auto& sample, int gate) {
            const bool height = sample.hasValue() && sample.value().policy == world::PolicyKind::Height;
            const bool hex = sample.hasValue() && sample.value().policy == world::PolicyKind::Hex;
            const bool heightSolid = height && sample.value().solid;
            if (gate == 0) return !heightSolid;
            if (gate == 1) return !sample.hasValue() || heightSolid;
            if (gate == 2) return !hex;
            return !height;
        };
        const auto waitColumn = [&](double x, double y, double z, world::PolicyKind policy, bool solid,
                                     const char* failure, const char* direction, int gate) {
            for (unsigned tick = 0; tick < 128; ++tick) {
                world_->update(awayAnchor); world_->waitForIdle();
                const auto sample = world_->classifyPublishedSolid(x, y, z);
                const bool published = sample.hasValue() && sample.value().policy == policy && sample.value().solid == solid;
                if (!published) {
                    require(holdsPrevious(sample, gate),
                        "In-flight edit or region change adopted the new owner before republication");
                    if (std::string(direction) == "edit") ++unadoptedEdit;
                    else ++unadoptedRegion;
                }
                movement << direction << ',' << (published ? "published" : "inflight") << ",512,512,"
                    << world_->residentCount() << ',' << (view_->acceptedSurfaceCurrent(*world_) ? 1 : 0) << ','
                    << static_cast<unsigned>(job.status().state) << ','
                    << x << ',' << z << ',' << y << ',' << probeText(x, y, z)
                    << ",tick," << tick << '\n';
                ++tickRows;
                if (!world_->hasPendingWork() && published) return;
            }
            throw std::runtime_error(failure);
        };
        waitColumn(520.2, 200.0, 520.7, world::PolicyKind::Height, true,
            "Republished height edit did not raise the published column", "edit", 0);
        movement << "edit,republished,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,200," << probeText(520.2, 200.0, 520.7) << ",anchor,\n";
        for (int i = 0; i < 4; ++i) require(world_->edit(awayKey, quad[i], originalSamples[i]), "Cannot restore the away height edit");
        const auto stillRaised = world_->classifyPublishedSolid(520.2, 200.0, 520.7);
        require(stillRaised.hasValue() && stillRaised.value().solid,
            "Restoring the height edit changed the published column before republication");
        ++unadoptedEdit;
        movement << "edit,restore,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,200," << probeText(520.2, 200.0, 520.7) << ",held,\n";
        waitColumn(520.2, 200.0, 520.7, world::PolicyKind::Height, false,
            "Restoring the height edit did not return the published column", "edit", 1);
        world_->regions().set({8, 8}, world::PolicyKind::Hex);
        const auto transitioning = world_->classifyPublishedSolid(520.2, -1000.0, 520.7);
        require(!(transitioning.hasValue() && transitioning.value().policy == world::PolicyKind::Height),
            "Region change kept the previous height owner");
        ++unadoptedRegion;
        movement << "region,transition,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,-1000," << probeText(520.2, -1000.0, 520.7) << ",held,\n";
        waitColumn(520.2, -8.0, 520.7, world::PolicyKind::Hex, true,
            "Region change did not publish the new hex owner", "region", 2);
        const auto belowFloor = world_->classifyPublishedSolid(520.2, -9.0, 520.7);
        require(belowFloor.hasValue() && !belowFloor.value().solid, "Changed hex column stayed solid below its floor");
        movement << "region,settled,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,-8," << probeText(520.2, -8.0, 520.7) << ",anchor,\n";
        require(world_->regions().erase({8, 8}), "Cannot restore the away height region");
        const auto reverting = world_->classifyPublishedSolid(520.2, -1000.0, 520.7);
        require(!(reverting.hasValue() && reverting.value().policy == world::PolicyKind::Hex),
            "Restoring the region kept the hex owner on the old mesh");
        ++unadoptedRegion;
        movement << "region,reverting,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,-1000," << probeText(520.2, -1000.0, 520.7) << ",held,\n";
        waitColumn(520.2, -1000.0, 520.7, world::PolicyKind::Height, true,
            "Restored region did not publish the height column again", "region", 3);
        movement << "region,restored,512,512," << world_->residentCount() << ",,,"
            << "520.2,520.7,-1000," << probeText(520.2, -1000.0, 520.7) << ",anchor,\n";
        walkSources({512, 0, 512}, {}, "inbound");
        unsigned homeTick = 0;
        settleSources({}, "inbound", 8, homeTick);
        require(!job.requestResidentWindow(*world_, world::PolicyKind::Height, world::PolicyKind::Hex),
                "Home resident boundary discovery failed");
        for (unsigned tick = 0; tick < 128 && !view_->boundaryUploadPending(); ++tick) {
            require(view_->syncBoundaryComposition(*world_, job, {1, 0, {}}), "Home corner staging failed");
            pool.waitIdle();
        }
        require(view_->boundaryUploadPending(), "Home corner worker did not produce a staged result");
        require(different(plain, checkpoint(away, "corner_return_pending")) == 0,
            "Return staging changed the accepted plain view");
        converge(true);
        require(different(original, checkpoint(camera, "corner_returned")) == 0,
            "Retargeted home corner failed exact image restoration");
        require(tickRows > 16, "Corner sweep did not record a world-update tick between the settled anchors");
        require(unadoptedEdit >= 1, "In-flight height edit was not recorded before republication");
        require(unadoptedRegion >= 1, "In-flight region change was not recorded before republication");
        movement.flush(); require(movement.good(), "Cannot finish continuous corner residency evidence");
    }

    void auditHeightHexProfiles() {
        std::vector<world::TerrainBoundaryMesh> hex, height;
        std::vector<world::StreamingChunkInfo> hexInfo, heightInfo;
        std::ofstream provenance(output_ / "height_hex_profile_sources.csv");
        provenance << "policy,source_index,chunk_x,chunk_y,chunk_z,mesh_version,region_revision\n";
        // No update/publication occurs during extraction. Provenance describes
        // this settled snapshot, not a future asynchronous transition dependency.
        for (const auto& chunk : world_->residentChunks()) {
            if (chunk.key.policy != world::PolicyKind::Height && chunk.key.policy != world::PolicyKind::Hex) continue;
            require(chunk.hasMesh && chunk.state == world::ChunkState::Meshed && chunk.meshVersion == chunk.version &&
                    chunk.meshRegionRevision == world_->regions().revision(), "Boundary audit requires current settled meshes");
            auto& sources = chunk.key.policy == world::PolicyKind::Hex ? hex : height;
            auto& info = chunk.key.policy == world::PolicyKind::Hex ? hexInfo : heightInfo;
            provenance << static_cast<unsigned>(chunk.key.policy) << ',' << sources.size() << ',' << chunk.key.coord.x << ','
                       << chunk.key.coord.y << ',' << chunk.key.coord.z << ',' << chunk.meshVersion << ',' << *chunk.meshRegionRevision << '\n';
            sources.push_back({world_->mesh(chunk.key), world_->meshOrigin(chunk.key)});
            info.push_back(chunk);
        }
        std::ofstream csv(output_ / "height_hex_profiles.csv"), summary(output_ / "height_hex_profile_summary.csv");
        std::ofstream svg(output_ / "height_hex_profiles.svg");
        std::ofstream wallCsv(output_ / "height_hex_wall_triangles.csv"), wallSummary(output_ / "height_hex_wall_summary.csv");
        std::ofstream wallSvg(output_ / "height_hex_walls.svg");
        std::ofstream trimCsv(output_ / "height_hex_trim.csv");
        std::ofstream stitchCsv(output_ / "height_hex_stitch.csv");
        std::ofstream packageCsv(output_ / "height_hex_replacement.csv");
        packageCsv << "axis,triangles,wall_triangles,vertices,surfaces,render_hash,provenance_hash,wall_ray_pairs\n";
        stitchCsv << "axis,positive,source,split_triangles,added_vertices,matched_edges\n";
        trimCsv.precision(17);
        trimCsv << "axis,positive,source,affected_triangles,remaining_triangles,removed_plane_area_m2\n";
        wallCsv.precision(17); wallSummary.precision(17); wallSvg.precision(9);
        wallCsv << "axis,triangle,span,positive_owner,material,ax,ay,az,bx,by,bz,cx,cy,cz\n";
        wallSummary << "axis,triangles,area_m2,front_back_ray_pairs\n";
        wallSvg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 1000 700' role='img' aria-label='Isolated height hex wall triangles'>"
            "<rect width='1000' height='700' fill='#f1eddf'/><g font-family='monospace' fill='#172f35'>"
            "<text x='50' y='32' font-size='22'>HEIGHT / HEX: ISOLATED CONNECTING WALLS</text>"
            "<text x='50' y='57' font-size='14'>Actual generated triangles. NOT published: old skirts/caps still need replacement.</text>"
            "<text x='50' y='80' font-size='14'>Orange: positive-side owner. Teal: negative-side owner. Edges show triangulation.</text>"
            "<text x='50' y='681' font-size='12'>Solid-column interpretation only. No world closure or moving-camera acceptance.</text>";
        csv.precision(17); summary.precision(17); svg.precision(6);
        csv << "axis,begin,end,hex_count,height_count,hex_begin_y,hex_end_y,hex_chunk_x,hex_chunk_z,hex_triangle,height_begin_y,height_end_y,height_chunk_x,height_chunk_z,height_triangle\n";
        summary << "axis,spans,paired_m,missing_m,overlap_m,max_height_gap_m,ray_checks\n";
        svg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 1000 700' role='img' aria-label='Measured height and hex boundary profiles'>"
            "<rect width='1000' height='700' fill='#f1eddf'/><g font-family='monospace' fill='#172f35'>"
            "<text x='50' y='32' font-size='22'>HEIGHT / HEX: MEASURED BOUNDARY PROFILES</text>"
            "<text x='50' y='57' font-size='14'>Published triangle tops. No bridge published; M16 transitions remain OPEN.</text>"
            "<text x='50' y='80' font-size='14' fill='#00746c'>TEAL: hex (negative side)</text>"
            "<text x='350' y='80' font-size='14' fill='#ad4e13'>ORANGE: height (positive side)</text>"
            "<text x='50' y='681' font-size='12'>Red bands: missing/overlapping profiles. CSV retains subpixel intervals; no epsilon welding.</text>";
        for (unsigned side = 0; side < 2; ++side) {
            const auto axis = side == 0 ? world::TerrainBoundaryAxis::X : world::TerrainBoundaryAxis::Z;
            const char* name = side == 0 ? "X=64 (east), tangent Z" : "Z=64 (north), tangent X";
            const auto result = world::compareTerrainBoundaryTops(hex, height, {axis, 64, 0, 64});
            require(result.hasValue(), "Cannot extract height/hex boundary profiles");
            jobs::JobSystem profilePool(1);
            world::BoundaryProfileJob profileJob(profilePool);
            profileJob.request({{axis, 64, 0, 64}});
            require(profileJob.update(*world_).state == world::BoundaryProfileState::Queued, "Cannot queue boundary profile job");
            profilePool.waitIdle();
            require(profileJob.update(*world_).state == world::BoundaryProfileState::Ready, "Boundary profile job did not publish a complete result");
            const auto* completed = profileJob.result(*world_);
            require(completed && completed->spans.size() == result.value().size(), "Captured boundary profile differs from live sources");
            for (std::size_t i = 0; i < result.value().size(); ++i) {
                const auto& live = result.value()[i]; const auto& copy = completed->spans[i];
                require(live.begin == copy.begin && live.end == copy.end && live.negativeCount == copy.negativeCount &&
                        live.positiveCount == copy.positiveCount, "Captured boundary subdivisions or coverage differ");
                for (const bool positive : {false, true}) {
                    const auto& a = positive ? live.positive : live.negative;
                    const auto& b = positive ? copy.positive : copy.negative;
                    require(a.has_value() == b.has_value(), "Captured boundary lost a unique source");
                    if (!a) continue;
                    const auto& info = positive ? heightInfo : hexInfo;
                    const auto& source = completed->snapshot.sources(positive)[b->sourceIndex];
                    require(a->beginHeight == b->beginHeight && a->endHeight == b->endHeight && a->triangleIndex == b->triangleIndex &&
                            info[a->sourceIndex].key == source.key && info[a->sourceIndex].meshVersion == source.meshVersion,
                            "Captured boundary geometry or source provenance differs");
                }
            }
            require(profileJob.result(*world_) != nullptr, "Boundary sources changed before audit acceptance");
            std::array<std::vector<world::TerrainBoundaryTrim>, 2> trimmed;
            std::array<std::vector<world::TerrainBoundaryMesh>, 2> trimmedViews;
            std::size_t trimmedCount = 0;
            for (const bool positive : {false, true}) {
                const auto& sources = positive ? height : hex;
                auto& copies = trimmed[positive ? 1 : 0];
                auto& views = trimmedViews[positive ? 1 : 0];
                copies.reserve(sources.size()); views.reserve(sources.size());
                for (std::size_t source = 0; source < sources.size(); ++source) {
                    const auto& view = sources[source];
                    const auto cut = world::trimTerrainBoundaryFaces(*view.mesh, view.translation, {axis, 64, 0, 64});
                    require(cut.hasValue(), "Cannot trim coplanar boundary faces on private source copies");
                    trimmedCount += cut.value().affectedTriangles;
                    copies.push_back(cut.value());
                    views.push_back({&copies.back().mesh, view.translation});
                    const auto planeArea = [&](const world::MeshData& mesh, bool requireOutside) {
                        double area = 0;
                        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                            const auto a = mesh.vertices[mesh.indices[i]].position + view.translation,
                                       b = mesh.vertices[mesh.indices[i + 1]].position + view.translation,
                                       c = mesh.vertices[mesh.indices[i + 2]].position + view.translation;
                            const auto fixed = [&](math::Vec3 p) { return side == 0 ? p.x : p.z; };
                            const auto t = [&](math::Vec3 p) { return side == 0 ? p.z : p.x; };
                            if (fixed(a) != 64 || fixed(b) != 64 || fixed(c) != 64) continue;
                            const double twiceArea = std::abs((double(b.y) - a.y) * (double(t(c)) - t(a)) -
                                (double(t(b)) - t(a)) * (double(c.y) - a.y));
                            if (requireOutside && twiceArea > 0)
                                require(std::max({t(a), t(b), t(c)}) <= 0 || std::min({t(a), t(b), t(c)}) >= 64,
                                        "Trimmed source retains positive-area coplanar overdraw inside boundary interval");
                            area += twiceArea * 0.5;
                        }
                        return area;
                    };
                    const double removed = planeArea(*view.mesh, false) - planeArea(copies.back().mesh, true);
                    require(removed >= 0, "Boundary trim increased source wall area");
                    trimCsv << side << ',' << positive << ',' << source << ',' << cut.value().affectedTriangles << ','
                            << cut.value().sourceTriangles.size() << ',' << removed << '\n';
                }
            }
            require(trimmedCount > 0, "Boundary trim audit did not exercise existing skirts/caps");
            const auto trimmedProfile = world::compareTerrainBoundaryTops(trimmedViews[0], trimmedViews[1], {axis, 64, 0, 64});
            require(trimmedProfile.hasValue() && trimmedProfile.value().size() == result.value().size(), "Boundary trim changed top subdivisions");
            for (std::size_t i = 0; i < result.value().size(); ++i) {
                const auto& old = result.value()[i]; const auto& current = trimmedProfile.value()[i];
                require(old.begin == current.begin && old.end == current.end && old.negativeCount == current.negativeCount &&
                        old.positiveCount == current.positiveCount, "Boundary trim changed top coverage");
                for (const bool positive : {false, true}) {
                    const auto& a = positive ? old.positive : old.negative;
                    const auto& b = positive ? current.positive : current.negative;
                    require(a && b && a->beginHeight == b->beginHeight && a->endHeight == b->endHeight &&
                            a->sourceIndex == b->sourceIndex && a->triangleIndex ==
                                trimmed[positive ? 1 : 0][b->sourceIndex].sourceTriangles[b->triangleIndex],
                            "Boundary trim altered a source top or lost its provenance");
                }
            }
            double minY = INFINITY, maxY = -INFINITY, paired = 0, missing = 0, overlap = 0, maxGap = 0;
            unsigned rayChecks = 0;
            const auto writeSample = [&](const auto& sample, const auto& info) {
                if (!sample) { csv << ",,,,,"; return; }
                const auto key = info[sample->sourceIndex].key;
                csv << ',' << sample->beginHeight << ',' << sample->endHeight << ',' << key.coord.x << ',' << key.coord.z << ',' << sample->triangleIndex;
                minY = std::min({minY, sample->beginHeight, sample->endHeight});
                maxY = std::max({maxY, sample->beginHeight, sample->endHeight});
            };
            for (const auto& span : result.value()) {
                const double length = span.end - span.begin;
                csv << side << ',' << span.begin << ',' << span.end << ',' << span.negativeCount << ',' << span.positiveCount;
                writeSample(span.negative, hexInfo); writeSample(span.positive, heightInfo); csv << '\n';
                if (span.negativeCount == 0 || span.positiveCount == 0) missing += length;
                if (span.negativeCount > 1 || span.positiveCount > 1) overlap += length;
                if (!span.negative || !span.positive) continue;
                paired += length;
                maxGap = std::max({maxGap, std::abs(span.negative->beginHeight - span.positive->beginHeight),
                                  std::abs(span.negative->endHeight - span.positive->endHeight)});
                // Sub-millimeter intervals remain in the CSV, but a float ray
                // origin can round onto a neighboring interval. Probe wider spans.
                if (length < 0.001) continue;
                const float t = static_cast<float>((span.begin + span.end) * 0.5);
                const math::Vec3 eye = side == 0 ? math::Vec3{64, 100, t} : math::Vec3{t, 100, 64};
                for (const bool positive : {false, true}) {
                    const auto& sample = positive ? *span.positive : *span.negative;
                    const auto& source = positive ? height[sample.sourceIndex] : hex[sample.sourceIndex];
                    const auto hit = world::raycastTerrainMesh(*source.mesh, eye, {0, -1, 0}, 200, source.translation, true);
                    require(hit && std::abs(hit->position.y - (sample.beginHeight + sample.endHeight) * 0.5) < 0.0001,
                            "Boundary profile disagrees with its published triangle ray");
                    ++rayChecks;
                }
            }
            require(paired > 0 && rayChecks > 0 && maxGap > 0 && std::isfinite(minY) && std::isfinite(maxY),
                    "Height/hex profile diagnostic lost its separated surfaces");
            require(paired == 64 && missing == 0 && overlap == 0,
                    "Canonical regional hex profile lost unique coverage at a shared endpoint");
            summary << side << ',' << result.value().size() << ',' << paired << ',' << missing << ',' << overlap << ',' << maxGap << ',' << rayChecks << '\n';
            const double top = 125 + side * 275, bottom = top + 195;
            const auto x = [](double t) { return 70 + t * (880.0 / 64); };
            const auto y = [&](double h) { return bottom - (h - minY + 1) / (maxY - minY + 2) * (bottom - top); };
            const auto walls = world::meshTerrainBoundaryWalls(
                {completed->spans.begin(), completed->spans.size()}, {axis, 64, 0, 64}, 13, 29);
            require(walls.hasValue() && !walls.value().mesh.indices.isEmpty(), "Cannot generate isolated height/hex walls");
            const auto& wallMesh = walls.value().mesh;
            const auto package = completed->snapshot.buildReplacement({}, 13, 29);
            require(package.hasValue() && world_->boundaryCurrent(package.value()), "Cannot build a current immutable boundary replacement");
            const auto* geometry = package.value().geometry();
            require(geometry && geometry->triangles.size() == geometry->mesh.indices.size() / 3,
                    "Replacement lost per-triangle provenance");
            std::size_t packagedWalls = 0;
            for (const auto& triangle : geometry->triangles) {
                require(triangle.sourceIndex < package.value().sources(triangle.positive).size(), "Replacement source index is invalid");
                if (triangle.wall) ++packagedWalls;
            }
            require(packagedWalls == walls.value().triangles.size(), "Replacement lost or duplicated wall faces");
            std::array<std::vector<world::TerrainBoundaryStitch>, 2> stitched;
            for (const bool positive : {false, true}) {
                const auto which = positive ? 1u : 0u;
                const auto& copies = trimmed[which];
                stitched[which].reserve(copies.size());
                for (std::size_t source = 0; source < copies.size(); ++source) {
                    std::vector<world::TerrainBoundaryTopCut> cuts;
                    for (const auto& edge : walls.value().topEdges) {
                        if (edge.positive != positive) continue;
                        const auto& span = trimmedProfile.value()[edge.spanIndex];
                        const auto& sample = positive ? *span.positive : *span.negative;
                        if (sample.sourceIndex != source) continue;
                        cuts.push_back({sample.triangleIndex, edge.begin, edge.beginHeight});
                        cuts.push_back({sample.triangleIndex, edge.end, edge.endHeight});
                    }
                    const auto translation = trimmedViews[which][source].translation;
                    auto built = world::stitchTerrainBoundaryTops(copies[source].mesh, translation, {axis, 64, 0, 64}, cuts);
                    require(built.hasValue(), "Cannot reconcile source top edges with canonical wall points");
                    auto& stitchedMesh = built.value().mesh;
                    std::size_t matchedEdges = 0;
                    const auto worldPoint = [&](std::uint32_t index) {
                        const auto p = stitchedMesh.vertices[index].position;
                        return std::array<double, 3>{double(p.x) + translation.x, double(p.y) + translation.y, double(p.z) + translation.z};
                    };
                    for (const auto& edge : walls.value().topEdges) {
                        if (edge.positive != positive) continue;
                        const auto& span = trimmedProfile.value()[edge.spanIndex];
                        const auto& sample = positive ? *span.positive : *span.negative;
                        if (sample.sourceIndex != source) continue;
                        const std::array<double, 3> a = side == 0 ? std::array<double, 3>{64, float(edge.beginHeight), float(edge.begin)} :
                            std::array<double, 3>{float(edge.begin), float(edge.beginHeight), 64};
                        const std::array<double, 3> b = side == 0 ? std::array<double, 3>{64, float(edge.endHeight), float(edge.end)} :
                            std::array<double, 3>{float(edge.end), float(edge.endHeight), 64};
                        unsigned matches = 0;
                        for (std::size_t i = 0; i < built.value().sourceTriangles.size(); ++i) {
                            if (built.value().sourceTriangles[i] != sample.triangleIndex) continue;
                            for (unsigned j = 0; j < 3; ++j) {
                                const auto p = worldPoint(stitchedMesh.indices[i * 3 + j]);
                                const auto q = worldPoint(stitchedMesh.indices[i * 3 + (j + 1) % 3]);
                                if ((p == a && q == b) || (p == b && q == a)) ++matches;
                            }
                        }
                        require(matches == 1, "Wall top edge does not match exactly one complete source top edge");
                        ++matchedEdges;
                    }
                    stitchCsv << side << ',' << positive << ',' << source << ',' << built.value().splitTriangles << ','
                              << stitchedMesh.vertices.size() - copies[source].mesh.vertices.size() << ',' << matchedEdges << '\n';
                    stitched[which].push_back(std::move(built).value());
                }
            }
            require(profileJob.result(*world_) != nullptr, "Boundary dependencies changed during edge reconciliation");
            wallSvg << "<text x='50' y='" << top - 15 << "' font-size='16'>" << name << "</text>"
                << "<rect x='70' y='" << top << "' width='880' height='195' fill='#fffcf4' stroke='#bac4be'/>";
            double wallArea = 0;
            unsigned wallRays = 0;
            for (std::size_t i = 0; i < walls.value().triangles.size(); ++i) {
                const auto& stamp = walls.value().triangles[i];
                const auto& va = wallMesh.vertices[wallMesh.indices[i * 3]];
                const auto a = va.position, b = wallMesh.vertices[wallMesh.indices[i * 3 + 1]].position,
                           c = wallMesh.vertices[wallMesh.indices[i * 3 + 2]].position;
                const auto tangent = [&](math::Vec3 v) { return axis == world::TerrainBoundaryAxis::X ? v.z : v.x; };
                const double signedArea = (double(b.y) - a.y) * (double(tangent(c)) - tangent(a)) -
                    (double(tangent(b)) - tangent(a)) * (double(c.y) - a.y);
                const double outward = (axis == world::TerrainBoundaryAxis::X ? 1 : -1) * (stamp.positiveOwner ? -1 : 1);
                require(signedArea * outward > 0, "Isolated wall has collapsed or reversed geometry");
                wallArea += std::abs(signedArea) * 0.5;
                wallCsv << side << ',' << i << ',' << stamp.spanIndex << ',' << stamp.positiveOwner << ',' << va.material;
                wallSvg << "<polygon points='";
                for (const auto v : {a, b, c}) {
                    wallCsv << ',' << v.x << ',' << v.y << ',' << v.z;
                    wallSvg << x(tangent(v)) << ',' << y(v.y) << ' ';
                }
                wallCsv << '\n';
                wallSvg << "' fill='" << (stamp.positiveOwner ? "#e1a46d" : "#73b8ae")
                    << "' stroke='#425857' stroke-width='0.5'/>";
                // Test raster-representable interior points, not rounded sliver edges.
                const float t = static_cast<float>((double(tangent(a)) + tangent(b) + tangent(c)) / 3);
                const auto& span = completed->spans[stamp.spanIndex];
                if (t <= span.begin || t >= span.end) continue;
                const float h = static_cast<float>((double(a.y) + b.y + c.y) / 3);
                const float sign = stamp.positiveOwner ? -1.f : 1.f;
                const auto eye = side == 0 ? math::Vec3{64 + sign * 2, h, t} : math::Vec3{t, h, 64 + sign * 2};
                const auto behind = side == 0 ? math::Vec3{64 - sign * 2, h, t} : math::Vec3{t, h, 64 - sign * 2};
                const auto direction = side == 0 ? math::Vec3{-sign, 0, 0} : math::Vec3{0, 0, -sign};
                const auto reverse = side == 0 ? math::Vec3{sign, 0, 0} : math::Vec3{0, 0, sign};
                const auto hit = world::raycastTerrainMesh(wallMesh, eye, direction, 4, {}, true);
                require(hit && hit->frontFace && std::abs(hit->distance - 2) < 0.00001,
                        "Generated boundary wall misses its outward triangle ray");
                require(!world::raycastTerrainMesh(wallMesh, behind, reverse, 4, {}, true),
                        "Generated boundary wall contains a reversed or duplicate back face");
                const auto packagedHit = world::raycastTerrainMesh(geometry->mesh, eye, direction, 4, {}, true);
                require(packagedHit && std::abs(packagedHit->distance - 2) < 0.00001 &&
                        geometry->triangles[packagedHit->triangleIndex].wall, "Combined replacement wall ray hit an old cap or missed");
                const auto& owner = geometry->triangles[packagedHit->triangleIndex];
                const auto& expected = owner.positive ? *completed->spans[stamp.spanIndex].positive : *completed->spans[stamp.spanIndex].negative;
                require(owner.positive == stamp.positiveOwner && owner.sourceIndex == expected.sourceIndex &&
                        owner.sourceTriangle == expected.triangleIndex, "Combined wall query lost original top provenance");
                // A reverse ray can hit unrelated source geometry, but not a coplanar old face at this wall.
                const auto reverseHit = world::raycastTerrainMesh(geometry->mesh, behind, reverse, 4, {}, true);
                require(!reverseHit || std::abs(reverseHit->distance - 2) > 0.00001,
                        "Combined replacement retains a reversed coplanar cap");
                ++wallRays;
            }
            require(wallRays > 0 && profileJob.result(*world_) != nullptr, "Wall audit lost valid source inputs");
            wallSummary << side << ',' << walls.value().triangles.size() << ',' << wallArea << ',' << wallRays << '\n';
            const auto resource = world::toResourceMesh(geometry->mesh);
            std::uint64_t renderHash = 1469598103934665603ull, provenanceHash = renderHash;
            const auto hashBytes = [&](const Bytes& bytes) {
                for (const auto byte : bytes) { renderHash ^= std::to_integer<std::uint8_t>(byte); renderHash *= 1099511628211ull; }
            };
            for (const auto& stream : resource.streams) hashBytes(stream.data);
            hashBytes(resource.indices);
            const auto hashWord = [&](std::uint64_t word) {
                for (unsigned i = 0; i < 8; ++i) { provenanceHash ^= (word >> (i * 8)) & 255u; provenanceHash *= 1099511628211ull; }
            };
            for (const auto& triangle : geometry->triangles) {
                hashWord(triangle.wall); hashWord(triangle.positive); hashWord(triangle.sourceIndex); hashWord(triangle.sourceTriangle);
            }
            for (const auto& surface : geometry->mesh.surfaces) { hashWord(surface.firstIndex); hashWord(surface.indexCount); hashWord(surface.material); }
            for (const auto& v : geometry->mesh.vertices) {
                hashWord(v.material); hashWord(v.biome); hashWord(v.ambientOcclusion);
                for (const auto weight : v.materialWeights) hashWord(weight);
            }
            require(world_->boundaryCurrent(package.value()), "Replacement became stale during audit");
            packageCsv << side << ',' << geometry->triangles.size() << ',' << packagedWalls << ',' << geometry->mesh.vertices.size() << ','
                       << geometry->mesh.surfaces.size() << ',' << renderHash << ',' << provenanceHash << ',' << wallRays << '\n';
            wallSvg << "<text x='70' y='" << bottom + 20 << "' font-size='12'>0 m</text><text x='915' y='" << bottom + 20
                << "' font-size='12'>64 m</text><text x='70' y='" << bottom + 40 << "' font-size='12'>Triangles: "
                << walls.value().triangles.size() << "; area: " << wallArea << " m2; culled ray pairs: " << wallRays << "</text>";
            svg << "<text x='50' y='" << top - 15 << "' font-size='16'>" << name << "; gap up to " << maxGap << " m</text>"
                << "<rect x='70' y='" << top << "' width='880' height='195' fill='#fffcf4' stroke='#bac4be'/>";
            for (const auto& span : result.value()) {
                if (!span.negative || !span.positive)
                    svg << "<rect x='" << x(span.begin) << "' y='" << top << "' width='" << x(span.end) - x(span.begin) << "' height='195' fill='#f3a7a0'/>";
                for (const bool positive : {false, true}) {
                    const auto& sample = positive ? span.positive : span.negative;
                    if (!sample) continue;
                    svg << "<path d='M " << x(span.begin) << ' ' << y(sample->beginHeight) << " L " << x(span.end) << ' ' << y(sample->endHeight)
                        << "' fill='none' stroke='" << (positive ? "#ad4e13" : "#00746c") << "' stroke-width='2'/>";
                }
            }
            svg << "<text x='70' y='" << bottom + 20 << "' font-size='12'>0 m</text><text x='915' y='" << bottom + 20
                << "' font-size='12'>64 m</text><text x='210' y='" << bottom + 20 << "' font-size='12'>Y range: " << minY << " to " << maxY << " m</text>"
                << "<text x='70' y='" << bottom + 40 << "' font-size='12'>Missing: " << missing << " m; overlap: " << overlap << " m; ray checks: " << rayChecks << "</text>";
        }
        svg << "</g></svg>";
        wallSvg << "</g></svg>";
        // Retain the disjoint baseline, then audit the complete shared corner.
        // Both remain CPU-only compositions, not published world geometry.
        std::vector<world::TerrainBoundaryMesh> shared = hex;
        shared.insert(shared.end(), height.begin(), height.end());
        std::vector<std::size_t> negativeIds, positiveIds;
        for (std::size_t i = 0; i < hex.size(); ++i) negativeIds.push_back(i);
        for (std::size_t i = 0; i < height.size(); ++i) positiveIds.push_back(hex.size() + i);
        const auto auditComposition = [&](bool corner) {
            const double end = corner ? 64 : 48;
            const std::array requests{
                world::TerrainBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, end}, negativeIds, positiveIds, 13, 29},
                world::TerrainBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, end}, negativeIds, positiveIds, 13, 29}};
            const auto composed = world::composeTerrainBoundaryReplacements(shared, requests);
            require(composed.hasValue(), "Cannot compose native boundaries over shared sources");
            const auto& combined = composed.value();
            std::size_t composedWalls = 0, composedRays = 0, composedEdges = 0;
            for (const auto& stamp : combined.triangles) {
                require(stamp.sourceIndex < shared.size() && stamp.sourceTriangle < shared[stamp.sourceIndex].mesh->indices.size() / 3,
                        "Composition lost original source provenance");
                if (stamp.wall) ++composedWalls;
            }
            std::size_t expectedWalls = 0;
            for (std::size_t boundary = 0; boundary < requests.size(); ++boundary) {
                const auto line = requests[boundary].line;
                const auto profile = world::compareTerrainBoundaryTops(hex, height, line);
                require(profile.hasValue(), "Cannot profile native composition reference");
                const auto reference = world::meshTerrainBoundaryWalls({profile.value().begin(), profile.value().size()}, line, 13, 29);
                require(reference.hasValue(), "Cannot build native composition reference walls");
                expectedWalls += reference.value().triangles.size();
                const bool xAxis = line.axis == world::TerrainBoundaryAxis::X;
                for (const auto& edge : reference.value().topEdges) {
                    const auto& span = profile.value()[edge.spanIndex];
                    const auto& sample = edge.positive ? *span.positive : *span.negative;
                    const auto source = sample.sourceIndex + (edge.positive ? hex.size() : 0);
                    const math::Vec3 a = xAxis ? math::Vec3{64, float(edge.beginHeight), float(edge.begin)} :
                        math::Vec3{float(edge.begin), float(edge.beginHeight), 64};
                    const math::Vec3 b = xAxis ? math::Vec3{64, float(edge.endHeight), float(edge.end)} :
                        math::Vec3{float(edge.end), float(edge.endHeight), 64};
                    unsigned matches = 0;
                    for (std::size_t i = 0; i < combined.triangles.size(); ++i) {
                        const auto& stamp = combined.triangles[i];
                        if (stamp.wall || stamp.sourceIndex != source || stamp.sourceTriangle != sample.triangleIndex) continue;
                        for (unsigned e = 0; e < 3; ++e) {
                            const auto p = combined.mesh.vertices[combined.mesh.indices[i * 3 + e]].position;
                            const auto q = combined.mesh.vertices[combined.mesh.indices[i * 3 + (e + 1) % 3]].position;
                            if ((a == p && b == q) || (a == q && b == p)) ++matches;
                        }
                    }
                    require(matches == 1, "Later boundary composition lost an earlier complete source edge");
                    ++composedEdges;
                }
                const auto& mesh = reference.value().mesh;
                for (std::size_t i = 0; i < reference.value().triangles.size(); ++i) {
                    const auto& stamp = reference.value().triangles[i];
                    const auto& span = profile.value()[stamp.spanIndex];
                    const auto a = mesh.vertices[mesh.indices[i * 3]].position, b = mesh.vertices[mesh.indices[i * 3 + 1]].position,
                        c = mesh.vertices[mesh.indices[i * 3 + 2]].position;
                    const auto tangent = [&](math::Vec3 v) { return xAxis ? v.z : v.x; };
                    const float t = float((double(tangent(a)) + tangent(b) + tangent(c)) / 3);
                    if (t <= span.begin || t >= span.end) continue;
                    const float h = float((double(a.y) + b.y + c.y) / 3), sign = stamp.positiveOwner ? -1.f : 1.f;
                    const math::Vec3 eye = xAxis ? math::Vec3{64 + sign * 2, h, t} : math::Vec3{t, h, 64 + sign * 2};
                    const math::Vec3 direction = xAxis ? math::Vec3{-sign, 0, 0} : math::Vec3{0, 0, -sign};
                    const auto hit = world::raycastTerrainMesh(combined.mesh, eye, direction, 4, {}, true);
                    require(hit && std::abs(hit->distance - 2) < 0.00001, "Composed wall ray missed");
                    const auto& owner = combined.triangles[hit->triangleIndex];
                    const auto& expected = stamp.positiveOwner ? *span.positive : *span.negative;
                    require(owner.wall && owner.boundaryIndex == boundary && owner.positive == stamp.positiveOwner &&
                            owner.sourceIndex == expected.sourceIndex + (stamp.positiveOwner ? hex.size() : 0) &&
                            owner.sourceTriangle == expected.triangleIndex, "Composed ray lost boundary/source identity");
                    const auto back = world::raycastTerrainMesh(combined.mesh, eye + direction * 4, -direction, 4, {}, true);
                    require(!back || std::abs(back->distance - 2) > 0.00001, "Composition retains a reversed coplanar cap");
                    ++composedRays;
                }
            }
            require(composedWalls == expectedWalls && composedRays > 0 && composedEdges > 0, "Composition lost or duplicated walls");
            std::size_t junctionPairs = 0;
            if (corner) {
                struct EdgeCount { unsigned count = 0; int orientation = 0; };
                std::map<std::pair<float, float>, EdgeCount> vertical;
                for (std::size_t i = 0; i < combined.triangles.size(); ++i) {
                    if (!combined.triangles[i].wall) continue;
                    for (unsigned e = 0; e < 3; ++e) {
                        const auto a = combined.mesh.vertices[combined.mesh.indices[i * 3 + e]].position;
                        const auto b = combined.mesh.vertices[combined.mesh.indices[i * 3 + (e + 1) % 3]].position;
                        if (a.x != 64 || a.z != 64 || b.x != 64 || b.z != 64) continue;
                        require(a.y != b.y, "Composed junction edge collapsed");
                        auto& edge = vertical[{std::min(a.y, b.y), std::max(a.y, b.y)}];
                        ++edge.count; edge.orientation += a.y < b.y ? 1 : -1;
                    }
                }
                for (const auto& entry : vertical) {
                    require(entry.second.count == 2 && entry.second.orientation == 0, "Native corner has an unmatched or reversed vertical wall edge");
                    ++junctionPairs;
                }
                require(junctionPairs == 1, "Native corner lost its shared vertical wall edge");
            }
            std::uint64_t renderHash = 1469598103934665603ull, metadataHash = renderHash;
            const auto resource = world::toResourceMesh(combined.mesh);
            const auto hashBytes = [&](const Bytes& bytes) {
                for (const auto byte : bytes) { renderHash ^= std::to_integer<std::uint8_t>(byte); renderHash *= 1099511628211ull; }
            };
            for (const auto& stream : resource.streams) hashBytes(stream.data);
            hashBytes(resource.indices);
            const auto hashWord = [&](std::uint64_t word) {
                for (unsigned i = 0; i < 8; ++i) { metadataHash ^= (word >> (i * 8)) & 255u; metadataHash *= 1099511628211ull; }
            };
            for (const auto& stamp : combined.triangles) {
                hashWord(stamp.wall); hashWord(stamp.positive); hashWord(stamp.sourceIndex); hashWord(stamp.sourceTriangle);
                hashWord(stamp.wall ? stamp.boundaryIndex : UINT64_MAX);
            }
            for (const auto& surface : combined.mesh.surfaces) { hashWord(surface.firstIndex); hashWord(surface.indexCount); hashWord(surface.material); }
            for (const auto& v : combined.mesh.vertices) {
                hashWord(v.material); hashWord(v.biome); hashWord(v.ambientOcclusion);
                for (const auto weight : v.materialWeights) hashWord(weight);
            }
            std::ofstream compositionCsv(output_ / (corner ? "height_hex_junction.csv" : "height_hex_composition.csv"));
            compositionCsv << "boundaries,sources,triangles,wall_triangles,vertices,surfaces,render_hash,metadata_hash,wall_ray_pairs,matched_top_edges"
                << (corner ? ",junction_edge_pairs\n" : "\n")
                << requests.size() << ',' << shared.size() << ',' << combined.triangles.size() << ',' << composedWalls << ','
                << combined.mesh.vertices.size() << ',' << combined.mesh.surfaces.size() << ',' << renderHash << ',' << metadataHash << ','
                << composedRays << ',' << composedEdges;
            if (corner) compositionCsv << ',' << junctionPairs;
            compositionCsv << '\n';
            compositionCsv.flush(); require(compositionCsv.good(), "Cannot write boundary composition evidence");
        };
        auditComposition(false);
        auditComposition(true);
        for (auto* stream : {&csv, &summary, &svg, &provenance, &wallCsv, &wallSummary, &wallSvg, &trimCsv, &stitchCsv, &packageCsv}) {
            stream->flush(); require(stream->good(), "Cannot write boundary profile evidence");
        }
    }

    void verifyVoxelCuts() {
        for (const bool cutFaces : {false, true}) {
            require(view_->beginReload(*world_), "Diagnostic GPU cleanup not drained");
            auto config = demo::streamingDemoConfig();
            config.horizontalRadius = 0;
            config.regionExtentMeters = 16;
            config.regionalSurfaceOwnership = true;
            config.regionalVoxelCutFaces = cutFaces;
            world_ = std::make_unique<world::WorldStreamer>(config);
            world_->regions().set({0, 0}, world::PolicyKind::Voxel);
            expectedResidents_ = 2;
            diagnosticAnchor_ = math::Vec3{};
            diagnosticCaption_ = cutFaces ? "VOXEL CUT: cell ownership before meshing; tunnel and roof preserved. Height transitions remain OPEN." :
                "VOXEL CUT BEFORE: surface-only clipping leaves an open section through solid cells.";
            auto camera = homeCamera();
            camera.position = {32, 8, 8}; camera.yaw = 0; camera.pitch = 0;
            settle(camera, "voxel_cut_cold");
            const world::StreamingChunkKey key{world::PolicyKind::Voxel, {0, 0, 0}};
            // Offline diagnostic: deterministic solid extends past the x=16 cut.
            // The tunnel stops at x=4 and retains a roof at y=10.
            for (std::uint32_t index = 0; index < world::kVoxelCellCount; ++index) {
                const auto x = index & 31u, z = (index >> 5u) & 31u, y = index >> 10u;
                const bool block = x < 24 && z < 16 && y < 16;
                const bool tunnel = x >= 4 && y >= 5 && y < 10 && z >= 5 && z < 11;
                require(world_->edit(key, index, world::CellValue::voxelValue(block && !tunnel ? 2 : 0)), "Cannot author voxel cut fixture");
            }
            settle(camera, "voxel_cut_authored");
            require(view_->renderStats().draws == 2, "Voxel cut fixture lost a draw");
            (void)checkpoint(camera, cutFaces ? "voxel_cut_east" : "voxel_cut_open");
            if (!cutFaces) {
                require(!world_->raycastPublishedTerrain({18, 4.5f, 8.5f}, {-1, 0, 0}, 3, true), "Before fixture unexpectedly has a cut face");
                continue;
            }
            auto oblique = camera;
            oblique.position = {32, 24, 28};
            oblique.yaw = std::atan2(20.f, 24.f);
            oblique.pitch = std::atan2(16.f, std::sqrt(24.f * 24.f + 20.f * 20.f));
            (void)checkpoint(oblique, "voxel_cut_oblique");
            std::ofstream probes(output_ / "voxel_cut_probes.csv");
            probes.precision(17);
            probes << "y,z,expected_distance,distance,normal_x,triangle\n";
            for (int y = 3; y <= 11; ++y) {
                for (int z = 3; z <= 12; ++z) {
                    const auto hit = world_->raycastPublishedTerrain({18, y + 0.5f, z + 0.5f}, {-1, 0, 0}, 16, true);
                    const double expected = y >= 5 && y < 10 && z >= 5 && z < 11 ? 14 : 2;
                    // Barycentric ray distances need not represent integer plane
                    // distances exactly; image/CSV repeatability stays byte-exact.
                    if (hit && std::abs(hit->surface.distance - expected) > 1e-12) {
                        std::fprintf(stderr, "Voxel cut probe y=%d z=%d expected=%.17g actual=%.17g normal=(%.9g,%.9g,%.9g)\n",
                            y, z, expected, hit->surface.distance, hit->surface.normal.x, hit->surface.normal.y, hit->surface.normal.z);
                    }
                    require(hit && hit->key == key && std::abs(hit->surface.distance - expected) <= 1e-12 && hit->surface.normal == math::Vec3{1, 0, 0} &&
                            hit->meshRegionRevision == world_->regions().revision(), "Voxel cut closed a cavity or lost an exposed face");
                    probes << y + 0.5f << ',' << z + 0.5f << ',' << expected << ',' << hit->surface.distance << ','
                           << hit->surface.normal.x << ',' << hit->surface.triangleIndex << '\n';
                }
            }
            const auto roof = world_->raycastPublishedTerrain({12.5f, 7, 8.5f}, {0, 1, 0}, 5, true);
            require(roof && roof->key == key && roof->surface.position.y == 10 && roof->surface.normal == math::Vec3{0, -1, 0},
                    "Voxel cut lost its overhang underside");
            probes.flush();
            require(probes.good(), "Cannot write voxel cut evidence");
        }
    }

    void interactive() {
        auto camera = homeCamera();
        while (running_) (void)frame(camera, "interactive");
        // Shutdown may wait for accepted writes, but never blocks a live frame.
        io_.shutdown();
        if (auto result = io_.poll()) {
            if (result->error) std::rethrow_exception(result->error);
            if (result->kind == demo::StreamingIoKind::Save) std::puts("Pending save completed on exit.");
        }
        csv_.flush();
        events_.flush();
        require(csv_.good() && events_.good(), "Cannot flush demo profiles");
    }

private:
    void waitIo(demo::FreeCamera& camera, const char* phase) {
        for (unsigned i = 0; i < 1200 && io_.busy(); ++i) (void)frame(camera, phase);
        require(!io_.busy(), "Background save/reload did not complete");
    }

    void pollIo(demo::FreeCamera& camera) {
        auto result = io_.poll();
        if (!result) return;
        const char* workerName = result->kind == demo::StreamingIoKind::Save ? "save_worker" :
            result->kind == demo::StreamingIoKind::Load ? "reload_worker" : "retire_worker";
        events_ << workerName << ',' << result->workerUs << '\n';
        require(events_.good(), "Cannot write worker profile");
        operation("io_request_to_completion", ioStarted_);
        if (result->kind == demo::StreamingIoKind::Load) reloadPending_ = false;
        if (result->error) {
            if (verify_) std::rethrow_exception(result->error);
            try { std::rethrow_exception(result->error); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "Background operation failed; current world retained: %s\n", error.what());
            }
            return;
        }
        if (result->kind == demo::StreamingIoKind::Save) {
            std::puts("Saved world.f4sd (snapshot taken when F5 was accepted).");
        } else if (result->kind == demo::StreamingIoKind::Load) {
            require(result->world != nullptr, "Reload did not prepare a world");
            const auto start = Clock::now();
            // Only a fully decoded result can replace the live world. The worker
            // receives exclusive CPU ownership; all GPU retirement stays here.
            require(io_.retire(world_), "Cannot schedule old world retirement");
            world_ = std::move(result->world);
            const auto gpuStart = Clock::now();
            require(view_->beginReload(*world_), "Prior GPU reload cleanup is still pending");
            operation("reload_gpu_detach", gpuStart);
            edits_ = std::move(result->edits);
            pendingEdits_ = !edits_.isEmpty();
            camera = homeCamera();
            ioStarted_ = start;
            operation("reload_publish", start);
        }
    }

    void finishVerification() {
        window_.device()->waitIdle();
        view_.reset();
        const auto retired = window_.device()->stats();
        require(retired.bufferBytes == rendererBaseline_.bufferBytes &&
                retired.bufferCount == rendererBaseline_.bufferCount &&
                retired.textureCount == rendererBaseline_.textureCount &&
                retired.pipelineCount == rendererBaseline_.pipelineCount &&
                retired.shaderCount == rendererBaseline_.shaderCount,
                "Renderer/chunk teardown leaked GPU resources");
        csv_.flush();
        events_.flush();
        require(csv_.good(), "Cannot flush frame profile");
        require(events_.good(), "Cannot flush operation profile");
    }

    void operation(const char* name, Clock::time_point start) {
        events_ << name << ',' << micros(Clock::now() - start) << '\n';
        require(events_.good(), "Cannot write operation profile");
    }
    void resize() {
        auto* device = window_.device();
        device->waitIdle();
        if (target_.isValid()) (void)device->destroyFramebuffer(target_);
        if (overlay_.isValid()) (void)device->destroyFramebuffer(overlay_);
        target_ = {}; overlay_ = {};
        width_ = static_cast<int>(std::max(1u, window_.window()->width()));
        height_ = static_cast<int>(std::max(1u, window_.window()->height()));
        gfx::FramebufferDesc desc;
        desc.extent = {static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_)};
        gfx::AttachmentDesc color;
        color.type = gfx::AttachmentType::Color;
        color.clearOnLoad = true;
        color.clear.color[0] = 0.12f; color.clear.color[1] = 0.18f; color.clear.color[2] = 0.23f;
        color.clear.color[3] = 1.f;
        gfx::AttachmentDesc depth;
        depth.type = gfx::AttachmentType::Depth;
        depth.clearOnLoad = true; depth.clear.depth = 1.f;
        desc.attachments.push(color); desc.attachments.push(depth);
        const auto target = device->createFramebuffer(desc);
        require(target.hasValue(), "Cannot create framebuffer");
        target_ = target.value();
        desc.attachments[0].clearOnLoad = false;
        desc.attachments[1].clearOnLoad = false;
        const auto overlay = device->createFramebuffer(desc);
        require(overlay.hasValue(), "Cannot create overlay framebuffer");
        overlay_ = overlay.value();
    }

    void draw(const demo::FreeCamera& camera, bool fixedCaption) {
        const auto start = Clock::now();
        engine::Camera view;
        const float cp = std::cos(camera.pitch);
        view.eye = camera.position;
        view.center = view.eye + math::Vec3{-std::cos(camera.yaw) * cp, -std::sin(camera.pitch),
                                           -std::sin(camera.yaw) * cp};
        view.farPlane = 600.f;
        require(view_->render(target_, view, static_cast<float>(width_) / static_cast<float>(height_)) ==
                    gfx::GfxError::None, "Renderer frame failed");
        const auto beforeHud = Clock::now();
        terrainRenderUs_ = micros(beforeHud - start);
        char status[200]{};
        std::snprintf(status, sizeof(status), "CPU %zu / GPU %zu (+%zu retiring) | IO %s | WASD mouse | E edit | F5 save | F9 reload | H home",
                      world_->residentCount(), view_->residentCount(), view_->pendingRetirements(),
                      io_.busy() ? "BUSY" : "IDLE");
        const gfx::hud::HudLine lines[]{
            {16, 16, diagnosticAnchor_ ? "M16 POLICY DIAGNOSTICS | FIXED RESIDENCY ANCHOR | OPENGL"
                     : diagnosticCaption_[0] != '\0' ? "M16 POLICY DIAGNOSTICS | CAMERA-DRIVEN RESIDENCY | OPENGL"
                     : "M16 STREAMING WORLD | HEIGHT TERRAIN | OPENGL"},
            {16, 38, diagnosticCaption_[0] != '\0' ? diagnosticCaption_ : fixedCaption ? "CHECK: continuous terrain; raised stone patch survives unload and disk reload." : status,
             0.95f, 0.88f, 0.6f, 1.f}};
        hud_.setLines(lines);
        gfx::CommandBuffer commands;
        commands.begin(); commands.beginRenderPass(overlay_);
        commands.setViewport({static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_)});
        hud_.render(commands, static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_));
        commands.endRenderPass(); commands.end();
        require(window_.device()->submit(commands) == gfx::GfxError::None, "HUD submit failed");
        hudUs_ = micros(Clock::now() - beforeHud);
    }

    auto captureImage(const char* name) -> Bytes {
        Bytes pixels;
        pixels.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        require(glGetError() == GL_NO_ERROR, "Capture readback failed");
        const auto stride = static_cast<std::size_t>(width_) * 4;
        for (int y = 0; y < height_ / 2; ++y) {
            for (std::size_t x = 0; x < stride; ++x) {
                std::swap(pixels[static_cast<std::size_t>(y) * stride + x],
                          pixels[static_cast<std::size_t>(height_ - y - 1) * stride + x]);
            }
        }
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width_, height_, 32,
                                                          width_ * 4, SDL_PIXELFORMAT_RGBA32);
        require(surface != nullptr, "Cannot create capture surface");
        const auto file = (output_ / (std::string(name) + ".bmp")).string();
        const int saved = SDL_SaveBMP(surface, file.c_str());
        SDL_FreeSurface(surface);
        require(saved == 0, "Cannot save screenshot");
        return pixels;
    }

    static auto different(const Bytes& a, const Bytes& b) -> std::size_t {
        require(a.size() == b.size(), "Capture sizes differ");
        std::size_t count = 0;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ++count;
        }
        return count;
    }

    auto checkpoint(demo::FreeCamera& camera, const char* name) -> Bytes {
        std::ofstream meshes(output_ / (std::string(name) + "_meshes.csv"));
        meshes << "policy,x,y,z,mesh_hash\n";
        for (const auto& chunk : world_->residentChunks()) {
            const auto* source = world_->mesh(chunk.key);
            require(source != nullptr, "Checkpoint missing CPU mesh");
            const auto resource = world::toResourceMesh(*source);
            std::uint64_t hash = 1469598103934665603ull;
            const auto hashBytes = [&](const Bytes& bytes) {
                for (const auto byte : bytes) {
                    hash ^= std::to_integer<std::uint8_t>(byte);
                    hash *= 1099511628211ull;
                }
            };
            for (const auto& stream : resource.streams) hashBytes(stream.data);
            hashBytes(resource.indices);
            meshes << static_cast<unsigned>(chunk.key.policy) << ',' << chunk.key.coord.x << ','
                   << chunk.key.coord.y << ',' << chunk.key.coord.z << ',' << hash << '\n';
        }
        meshes.flush();
        require(meshes.good(), "Cannot save checkpoint mesh digests");
        const auto first = frame(camera, "capture", name);
        const std::string repeated = std::string(name) + "_repeat";
        const auto second = frame(camera, "capture", repeated.c_str());
        require(different(first, second) == 0, "Settled checkpoint is blinking");
        return first;
    }

    void travel(demo::FreeCamera& camera, float from, float to, const char* phase) {
        // Fixed positions, asynchronous workers: no waitForIdle in the frame loop.
        for (unsigned step = 1; step <= 160; ++step) {
            camera = homeCamera(from + (to - from) * (static_cast<float>(step) / 160.f));
            const auto capture = std::string("motion_") + phase + "_" + std::to_string(step);
            (void)frame(camera, phase, step % 40 == 0 ? capture.c_str() : nullptr);
        }
    }

    std::filesystem::path output_;
    bool verify_ = false;
    std::optional<math::Vec3> diagnosticAnchor_;
    const char* diagnosticCaption_ = "";
    std::size_t expectedResidents_ = 25;
    ::demo::DemoWindow window_;
    gfx::hud::Overlay hud_;
    std::unique_ptr<demo::StreamingDemoView> view_;
    std::unique_ptr<world::WorldStreamer> world_;
    demo::StreamingDemoIo io_;
    Clock::time_point ioStarted_;
    bool reloadPending_ = false;
    containers::Array<world::ChunkDelta> edits_;
    bool pendingEdits_ = false;
    bool running_ = true;
    gfx::FramebufferHandle target_, overlay_;
    gfx::DeviceStats rendererBaseline_;
    std::uint32_t preparedPipelineCount_ = 0;
    int width_ = 1280, height_ = 720;
    std::ofstream csv_;
    std::ofstream events_;
    std::uint64_t frames_ = 0;
    double terrainRenderUs_ = 0;
    double hudUs_ = 0;
    Clock::time_point lastStart_;
};
} // namespace

int main(int argc, char** argv) {
    try {
        bool verify = false, policies = false;
        std::filesystem::path output, load;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--verify") verify = true;
            else if (option == "--verify-policies") { verify = true; policies = true; }
            else if (option == "--output" && i + 1 < argc) output = argv[++i];
            else if (option == "--load" && i + 1 < argc) load = argv[++i];
            else {
                std::fputs("Usage: f4_streaming_demo [--verify | --verify-policies] [--output NEW-directory] [--load world.f4sd]\n", stderr);
                return 2;
            }
        }
        require(!policies || load.empty(), "Policy diagnostics do not use the height-only save format");
        auto edits = load.empty() ? containers::Array<world::ChunkDelta>{} : f4::demo::readStreamingDemoSave(load);
        if (output.empty()) {
            std::filesystem::create_directories("build");
            output = std::filesystem::path("build") /
                ("streaming-demo-" + std::to_string(Clock::now().time_since_epoch().count()));
        }
        require(std::filesystem::create_directory(output), "Output must be a new directory in an existing parent");
        // Enabled before worker creation; this tracks the engine allocation seam,
        // not STL/driver allocations. It stays enabled until all workers stop.
        memory::enableHeapTracking(true);
        {
            Application app(output, verify, std::move(edits));
            if (policies) app.verifyPolicies();
            else if (verify && load.empty()) app.verify();
            else if (verify) app.verifyLoaded();
            else app.interactive();
        }
        memory::enableHeapTracking(false);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Streaming demo: %s\n", error.what());
        return 1;
    }
}
