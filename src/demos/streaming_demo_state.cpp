#include "demos/streaming_demo_state.h"

#include <algorithm>
#include <utility>
#include <limits>
#include <tuple>
#include <vector>

#include "core/ecs/transform.h"
#include "engine/lighting/light.h"

namespace f4::demo {
namespace {
constexpr std::uint32_t kSaveId = 0x444d5453u;
constexpr std::uint32_t kSaveVersion = 1;

auto makeSaveChunk(world::ChunkStore& store) -> world::ChunkHandle {
    const auto config = streamingDemoConfig();
    const auto handle = store.create(kEditedChunk.coord, config.worldSeed,
                                     kEditedChunk.policy, config.policyParams);
    if (!store.queue(handle) || !store.generate(handle)) return {};
    return handle;
}
} // namespace

auto streamingDemoConfig() -> world::WorldStreamingConfig {
    world::WorldStreamingConfig config;
    config.worldSeed = kStreamingDemoSeed;
    config.defaultPolicy = world::PolicyKind::Height;
    config.horizontalRadius = 2;
    config.voxelVerticalRadius = 0;
    config.chunkCapacityPerPolicy = 25;
    config.workerCount = 4;
    config.frameBudget = kStreamingDemoFrameBudget - kStreamingDemoUploadReserve;
    config.profileTimings = true;
    config.cullSharedHeightSkirts = true;
    return config;
}

auto streamingMixedDemoConfig() -> world::WorldStreamingConfig {
    auto config = streamingDemoConfig();
    config.horizontalRadius = 1;
    config.regionExtentMeters = 64;
    // Three independent stores can coexist during policy replacement.
    config.chunkCapacityPerPolicy = 8; // At most 24 total, below the view's 25.
    config.policyParams.hex.radius = 2;
    config.hexRadius = 8.f;
    return config;
}

void setStreamingMixedRegions(world::RegionMap& regions, unsigned phase) {
    if (phase % 3 == 2) {
        regions.erase({-1, 0});
        regions.erase({0, 0});
    } else {
        regions.set({-1, 0}, phase % 3 == 0 ? world::PolicyKind::Voxel : world::PolicyKind::Hex);
        regions.set({0, 0}, phase % 3 == 0 ? world::PolicyKind::Hex : world::PolicyKind::Voxel);
    }
}

auto streamingDemoPatch() -> containers::Array<world::ChunkDelta> {
    containers::Array<world::ChunkDelta> edits;
    for (std::uint32_t z = 24; z <= 40; ++z) {
        for (std::uint32_t x = 24; x <= 40; ++x) {
            edits.push({world::heightSampleIndex(x, z),
                world::CellValue::heightValue({38.f, 2, {0, 0, 255, 0}})});
        }
    }
    return edits;
}

auto encodeStreamingDemoSave(const containers::Array<world::ChunkDelta>& edits)
    -> std::optional<containers::Array<std::byte>> {
    world::ChunkStore store(1);
    const auto handle = makeSaveChunk(store);
    if (!handle.isValid()) return std::nullopt;
    for (const auto& edit : edits) {
        if (!store.edit(handle, edit.index, edit.value)) return std::nullopt;
    }
    memory::MemoryWriter deltas;
    if (!store.saveDeltas(handle, deltas)) return std::nullopt;
    memory::MemoryWriter payload;
    payload.writeValue(kStreamingDemoSeed);
    payload.write(deltas.bytes().data(), deltas.bytes().size());
    memory::MemoryWriter output;
    memory::writeStreamPreamble(output);
    if (!memory::writeChunk(output, kSaveId, kSaveVersion,
                            payload.bytes().data(), payload.bytes().size())) return std::nullopt;
    return output.bytes();
}

auto decodeStreamingDemoSave(const containers::Array<std::byte>& bytes)
    -> std::optional<containers::Array<world::ChunkDelta>> {
    memory::MemoryReader stream(bytes);
    memory::ChunkView chunk;
    if (!memory::checkStreamPreamble(stream) || !memory::readChunkView(stream, chunk) ||
        !stream.atEnd() || chunk.id != kSaveId || chunk.version != kSaveVersion) return std::nullopt;
    memory::MemoryReader payload(chunk.payload, chunk.payloadSize);
    std::uint64_t seed = 0;
    if (!payload.readValue(seed) || seed != kStreamingDemoSeed) return std::nullopt;
    containers::Array<std::byte> deltas;
    deltas.resize(payload.remaining());
    if (!payload.read(deltas.data(), deltas.size())) return std::nullopt;
    world::ChunkStore store(1);
    const auto handle = makeSaveChunk(store);
    if (!handle.isValid() || !store.loadDeltas(handle, deltas)) return std::nullopt;
    return store.snapshotDeltas(handle);
}

auto applyStreamingDemoEdits(world::WorldStreamer& world,
                             const containers::Array<world::ChunkDelta>& edits) -> bool {
    const auto chunk = world.chunk(kEditedChunk);
    if (!chunk || chunk->state < world::ChunkState::CellsReady ||
        edits.size() > std::numeric_limits<std::uint64_t>::max() - chunk->version) return false;
    // Preflight every edit without generating/serializing a temporary chunk.
    // The height rule is shared with ChunkStore, including its save-codec checks.
    for (const auto& edit : edits) {
        if (edit.index >= world::kHeightSampleCount || edit.value.kind != world::PolicyKind::Height ||
            !world::validHeightSample(edit.value.height)) return false;
    }
    for (const auto& edit : edits) {
        if (!world.edit(kEditedChunk, edit.index, edit.value)) return false;
    }
    return true;
}

StreamingDemoView::StreamingDemoView(gfx::IDevice& device) : renderer_(registry_, device) {}
StreamingDemoView::~StreamingDemoView() { clear(); }

auto StreamingDemoView::init() -> bool {
    reloadMeshes_.reserve(25);
    if (renderer_.init() != gfx::GfxError::None) return false;
    renderer_.pbrSettings().shadowsEnabled = false;
    renderer_.pbrSettings().ambientIntensity = 0.6f;
    renderer_.pbrSettings().profileTimings = true;
    engine::StandardMaterialDesc material;
    material.roughnessFactor = 0.9f;
    const std::array<math::Vec4, 3> policyTints{{{0.85f, 0.93f, 1.f, 1.f},
        {1.f, 1.f, 1.f, 1.f}, {1.f, 0.94f, 0.82f, 1.f}}};
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        // Distinct visible tints also prevent descriptor-cache deduplication.
        material.tint = policyTints[i];
        materials_[i] = renderer_.createMaterial(material);
        if (!materials_[i].isValid()) return false;
    }
    // Use the real terrain conversion/upload path, including completed PBR
    // inputs and instancing. A canonical material pipeline is a different variant.
    world::MeshData triangle;
    triangle.vertices.push({{0.f, 0.f, 0.f}});
    triangle.vertices.push({{0.f, 0.f, 1.f}});
    triangle.vertices.push({{1.f, 0.f, 0.f}});
    triangle.indices.push(0); triangle.indices.push(1); triangle.indices.push(2);
    const auto representative = renderer_.createMesh(world::toResourceMesh(triangle));
    if (!representative.isValid()) return false;
    bool prepared = true;
    for (const auto handle : materials_) {
        if (renderer_.ensureMaterialPipeline(handle, representative) != gfx::GfxError::None) {
            prepared = false;
            break;
        }
    }
    renderer_.destroyMesh(representative);
    if (!prepared) return false;
    camera_ = registry_.create();
    registry_.emplace<engine::Camera>(camera_);
    const auto light = registry_.create();
    registry_.emplace<engine::DirectionalLight>(light,
        engine::DirectionalLight{{0.45f, -1.f, 0.3f}, {1.f, 0.96f, 0.88f}, 2.f});
    return true;
}

auto StreamingDemoView::makeSurfaceView(
    const std::unordered_map<world::StreamingChunkKey, Resident>& residents,
    const world::StreamingBoundaryComposition* composition) const
    -> std::optional<world::StreamingSurfaceView> {
    if (residents.empty()) return std::nullopt;
    std::vector<world::StreamingSurfaceSource> sources;
    sources.reserve(residents.size());
    for (const auto& [key, resident] : residents) {
        sources.push_back({key, resident.source, resident.version, resident.regionRevision,
                           resident.origin, resident.owner, resident.geometry});
    }
    std::optional<world::StreamingBoundaryComposition> accepted;
    if (composition) accepted = *composition;
    auto built = world::StreamingSurfaceView::build(sources, std::move(accepted));
    if (!built) return std::nullopt;
    return std::move(built).value();
}

void StreamingDemoView::clear() {
    plainOwner_.reset();
    removeBoundaryComposition();
    for (const auto mesh : reloadMeshes_) renderer_.destroyMesh(mesh);
    reloadMeshes_.clear();
    for (const auto& [key, resident] : chunks_) {
        (void)key;
        if (resident.entity.isValid()) registry_.destroy(resident.entity);
        if (resident.mesh.isValid()) renderer_.destroyMesh(resident.mesh);
    }
    chunks_.clear();
    surfaceView_.reset();
}

void StreamingDemoView::clear(world::WorldStreamer& world) {
    world.clearSurfaceView();
    clear();
}

auto StreamingDemoView::acceptedSurfaceCurrent(const world::WorldStreamer& world) const -> bool {
    return surfaceView_ && surfaceView_->current(world);
}

auto StreamingDemoView::beginReload() -> bool {
    if (pendingBoundary_ || !boundaryRetired_.isEmpty() || !reloadMeshes_.isEmpty()) return false;
    removeBoundaryComposition();
    for (const auto& [key, resident] : chunks_) {
        (void)key;
        if (resident.entity.isValid()) registry_.destroy(resident.entity);
        if (resident.mesh.isValid()) reloadMeshes_.push(resident.mesh);
    }
    chunks_.clear();
    surfaceView_.reset();
    plainOwner_.reset();
    return true;
}

auto StreamingDemoView::beginReload(world::WorldStreamer& world) -> bool {
    if (!beginReload()) return false;
    world.clearSurfaceView();
    return true;
}

auto StreamingDemoView::sync(world::WorldStreamer& world, std::uint32_t uploadLimit,
                             std::optional<std::chrono::steady_clock::time_point> deadline) -> bool {
    uploads_ = {};
    retireBoundaryMeshes({0, 2, deadline});
    if (pendingBoundary_ || !boundaryRetired_.isEmpty()) return true;
    if (boundary_) return true; // Frozen render/query inspection snapshot, even after world mutation.
    if (world.residentCount() > 25) return false;
    while (!reloadMeshes_.isEmpty() && uploads_.reloadRetired < 2) {
        if (uploads_.reloadRetired != 0 && deadline && std::chrono::steady_clock::now() >= *deadline) break;
        renderer_.destroyMesh(reloadMeshes_.back());
        reloadMeshes_.pop();
        ++uploads_.reloadRetired;
    }
    std::vector<world::StreamingChunkKey> retireKeys;
    for (const auto& [key, resident] : chunks_) {
        (void)resident;
        if (!world.chunk(key)) retireKeys.push_back(key);
    }
    struct PreparedUpload {
        world::StreamingChunkInfo chunk;
        std::shared_ptr<const world::MeshData> geometry;
        resources::MeshHandle mesh;
        bool expired = false;
        std::size_t bytes = 0;
    };
    std::vector<PreparedUpload> prepared;
    std::uint32_t admitted = 0;
    const auto rollbackPrepared = [&] {
        for (const auto& upload : prepared) if (upload.mesh.isValid()) renderer_.destroyMesh(upload.mesh);
    };
    for (const auto& chunk : world.residentChunks()) {
        if (!chunk.hasMesh || chunk.state != world::ChunkState::Meshed) continue;
        const auto found = chunks_.find(chunk.key);
        if (found != chunks_.end() && residentCurrent(found->second, world, chunk)) continue;
        if (admitted >= uploadLimit) {
            ++uploads_.deferred;
            continue;
        }
        const bool expired = deadline && std::chrono::steady_clock::now() >= *deadline;
        if (expired && admitted != 0) {
            ++uploads_.deferred;
            ++uploads_.budgetDeferred;
            continue;
        }
        const auto* source = world.mesh(chunk.key);
        if (source == nullptr || source->vertices.isEmpty() != source->indices.isEmpty()) {
            rollbackPrepared();
            return false;
        }
        PreparedUpload upload;
        upload.chunk = chunk;
        upload.geometry = std::make_shared<const world::MeshData>(*source);
        upload.expired = expired;
        auto resource = world::toResourceMesh(*source);
        upload.bytes = resource.indices.size();
        for (const auto& stream : resource.streams) upload.bytes += stream.data.size();
        if (!source->indices.isEmpty()) {
            // One vertex-colored material per policy covers every terrain surface.
            // Keep the entire index range, not just material slot zero.
            resource.submeshes.clear();
            resource.submeshes.push({0, source->indices.size()});
            upload.mesh = renderer_.createMesh(resource);
            if (!upload.mesh.isValid()) {
                rollbackPrepared();
                return false;
            }
        }
        prepared.push_back(std::move(upload));
        ++admitted;
    }
    bool unready = false;
    for (const auto& [key, resident] : chunks_) {
        const auto chunk = world.chunk(key);
        if (!chunk || residentCurrent(resident, world, *chunk)) continue;
        const bool uploaded = std::any_of(prepared.begin(), prepared.end(),
            [&](const PreparedUpload& upload) { return upload.chunk.key == key; });
        if (uploaded) continue;
        if (!chunk->hasMesh || chunk->state != world::ChunkState::Meshed) unready = true;
        else if (admitted > 0) retireKeys.push_back(key);
    }
    // A resident with no replacement yet keeps the previous picture. Ready
    // replacements that do not fit this frame leave the picture until a later
    // upload, so the slot never queries geometry that is no longer drawn.
    if (unready) {
        rollbackPrepared();
        uploads_.deferred += admitted;
        plainOwner_.reset();
        return true;
    }
    std::unordered_map<world::StreamingChunkKey, Resident> planned = chunks_;
    for (const auto& key : retireKeys) planned.erase(key);
    for (const auto& upload : prepared) {
        Resident& resident = planned[upload.chunk.key];
        const auto previous = chunks_.find(upload.chunk.key);
        resident = {upload.chunk.handle, upload.chunk.version,
            previous == chunks_.end() ? ecs::Entity{} : previous->second.entity, upload.mesh,
            upload.geometry, world.meshOrigin(upload.chunk.key), upload.chunk.meshRegionRevision,
            world.identityToken()};
    }
    std::optional<world::StreamingSurfaceView> plannedView;
    if (const auto accepted = makeSurfaceView(planned)) plannedView = *accepted;
    else if (!planned.empty()) {
        rollbackPrepared();
        return false;
    }
    // A package that fails the slot gate is not drawn. The previous publication
    // stays on screen and remains the query.
    if (plannedView ? !plannedView->current(world) : false) {
        rollbackPrepared();
        uploads_.deferred += admitted;
        plainOwner_.reset();
        return true;
    }
    if (plannedView) {
        if (!world.publishSurfaceView(*plannedView)) {
            rollbackPrepared();
            return false;
        }
    } else {
        world.clearSurfaceView();
    }
    for (const auto& key : retireKeys) {
        const auto found = chunks_.find(key);
        if (found->second.entity.isValid()) registry_.destroy(found->second.entity);
        if (found->second.mesh.isValid()) renderer_.destroyMesh(found->second.mesh);
        chunks_.erase(found);
        ++uploads_.retired;
    }
    for (auto& upload : prepared) {
        const auto previous = chunks_.find(upload.chunk.key);
        ecs::Entity entity = previous == chunks_.end() ? ecs::Entity{} : previous->second.entity;
        const resources::MeshHandle oldMesh = previous == chunks_.end() ? resources::MeshHandle{} : previous->second.mesh;
        if (upload.mesh.isValid()) {
            if (!entity.isValid()) {
                entity = registry_.create();
                registry_.emplace<ecs::LocalTransform>(entity).position = world.meshOrigin(upload.chunk.key);
                registry_.emplace<engine::MeshRenderer>(entity,
                    engine::MeshRenderer{upload.mesh, materials_[static_cast<std::size_t>(upload.chunk.key.policy)], 0, false, false});
            } else {
                registry_.get<engine::MeshRenderer>(entity)->mesh = upload.mesh;
                ecs::setLocalPosition(registry_, entity, world.meshOrigin(upload.chunk.key));
            }
        } else if (entity.isValid()) {
            registry_.destroy(entity);
            entity = {};
        }
        if (oldMesh.isValid()) renderer_.destroyMesh(oldMesh);
        Resident resident{upload.chunk.handle, upload.chunk.version, entity, upload.mesh,
            upload.geometry, world.meshOrigin(upload.chunk.key), upload.chunk.meshRegionRevision,
            world.identityToken()};
        if (previous == chunks_.end()) chunks_.emplace(upload.chunk.key, std::move(resident));
        else previous->second = std::move(resident);
        ++uploads_.uploaded;
        uploads_.progressAfterDeadline = uploads_.progressAfterDeadline || upload.expired;
        uploads_.bytes += upload.bytes;
    }
    ecs::updateWorldTransforms(registry_);
    if (plannedView) surfaceView_ = *plannedView;
    else surfaceView_.reset();
    plainOwner_.reset(); // Ordinary per-resident synchronization is not a composed transaction.
    return true;
}

auto StreamingDemoView::caughtUp(const world::WorldStreamer& world) const -> bool {
    if (plainOwner_ && (plainOwner_ != world.identityToken() || plainRevision_ != world.regions().revision())) return false;
    if (pendingBoundary_ || !boundaryRetired_.isEmpty()) return false;
    if (boundary_ && !world.boundaryCurrent(boundary_->composition)) return false;
    if (!reloadMeshes_.isEmpty() || chunks_.size() != world.residentCount() || world.hasPendingWork()) return false;
    for (const auto& chunk : world.residentChunks()) {
        const auto found = chunks_.find(chunk.key);
        if (!chunk.hasMesh || chunk.state != world::ChunkState::Meshed || found == chunks_.end() ||
            !residentCurrent(found->second, world, chunk)) return false;
    }
    return true;
}

void StreamingDemoView::removeBoundaryComposition() {
    cancelBoundaryUpload();
    // Explicit removal/clear is synchronous teardown, not a frame-budgeted tick.
    for (const auto mesh : boundaryRetired_) renderer_.destroyMesh(mesh);
    boundaryRetired_.clear();
    if (!boundary_) {
        if (const auto accepted = makeSurfaceView(chunks_)) surfaceView_ = *accepted;
        else surfaceView_.reset();
        return;
    }
    for (const auto& source : boundary_->composition.sources()) {
        const auto found = chunks_.find(source.key);
        if (found != chunks_.end() && found->second.entity.isValid())
            registry_.get<engine::MeshRenderer>(found->second.entity)->mesh = found->second.mesh;
    }
    for (const auto entity : boundary_->entities) if (entity.isValid()) registry_.destroy(entity);
    for (const auto mesh : boundary_->meshes) if (mesh.isValid()) renderer_.destroyMesh(mesh);
    boundary_.reset();
    if (const auto accepted = makeSurfaceView(chunks_)) surfaceView_ = *accepted;
    else surfaceView_.reset();
}

void StreamingDemoView::removeBoundaryComposition(world::WorldStreamer& world) {
    removeBoundaryComposition();
    if (surfaceView_ && surfaceView_->current(world)) (void)world.publishSurfaceView(*surfaceView_);
    else world.clearSurfaceView();
}

auto StreamingDemoView::installBoundaryComposition(world::WorldStreamer& world,
                                                   const world::StreamingBoundaryComposition& composition) -> bool {
    return publishBoundaryComposition(world, composition, false);
}

auto StreamingDemoView::replaceBoundaryComposition(world::WorldStreamer& world,
                                                   const world::StreamingBoundaryComposition& composition) -> bool {
    return publishBoundaryComposition(world, composition, true);
}

void StreamingDemoView::retireBoundaryMeshes(BoundaryUploadBudget budget) {
    while (!boundaryRetired_.isEmpty() && uploads_.retired < budget.retirements) {
        if (budget.deadline && std::chrono::steady_clock::now() >= *budget.deadline) break;
        renderer_.destroyMesh(boundaryRetired_.back()); boundaryRetired_.pop();
        ++uploads_.retired;
    }
}

void StreamingDemoView::cancelBoundaryUpload() {
    if (!pendingBoundary_) return;
    for (const auto& [key, resident] : pendingBoundary_->chunks) {
        (void)key;
        if (resident.borrowed) continue;
        if (resident.entity.isValid()) registry_.destroy(resident.entity);
        if (resident.mesh.isValid()) boundaryRetired_.push(resident.mesh);
    }
    for (const auto entity : pendingBoundary_->boundary.entities) if (entity.isValid()) registry_.destroy(entity);
    for (const auto mesh : pendingBoundary_->boundary.meshes) if (mesh.isValid()) boundaryRetired_.push(mesh);
    pendingBoundary_.reset();
}

auto StreamingDemoView::residentCurrent(const Resident& resident, const world::WorldStreamer& world,
                                         const world::StreamingChunkInfo& chunk) const -> bool {
    const auto origin = world.meshOrigin(chunk.key);
    return chunk.hasMesh && chunk.state == world::ChunkState::Meshed && chunk.version == chunk.meshVersion &&
        resident.owner == world.identityToken() && resident.source == chunk.handle &&
        resident.version == chunk.meshVersion && resident.regionRevision == chunk.meshRegionRevision &&
        resident.geometry && resident.origin.x == origin.x && resident.origin.y == origin.y && resident.origin.z == origin.z;
}

auto StreamingDemoView::boundaryUploadCurrent(const world::WorldStreamer& world) const -> bool {
    if (!pendingBoundary_ || world.hasPendingWork() || world.residentCount() != pendingBoundary_->desired.size() ||
        world.identityToken() != pendingBoundary_->owner ||
        world.regions().revision() != pendingBoundary_->regionRevision ||
        (!pendingBoundary_->plain && !world.boundaryCurrent(pendingBoundary_->boundary.composition))) return false;
    for (const auto& expected : pendingBoundary_->desired) {
        const auto current = world.chunk(expected.key);
        if (!current || !current->hasMesh || current->state != world::ChunkState::Meshed ||
            current->handle != expected.handle || current->version != expected.version ||
            current->meshVersion != expected.meshVersion || current->meshRegionRevision != expected.meshRegionRevision) return false;
    }
    return true;
}

auto StreamingDemoView::syncBoundaryComposition(world::WorldStreamer& world, world::BoundaryCompositionJob& job,
                                                BoundaryUploadBudget budget) -> bool {
    uploads_ = {};
    retireBoundaryMeshes(budget);
    const auto status = job.update(world);
    const auto* result = job.result(world);
    const bool plain = status.state == world::BoundaryCompositionState::NoBoundaries;
    if (pendingBoundary_ && (pendingBoundary_->plain != plain ||
                            (!plain && (!result || pendingBoundary_->boundary.composition.geometry() != result->geometry())) ||
                            !boundaryUploadCurrent(world))) cancelBoundaryUpload();
    retireBoundaryMeshes(budget);
    if (status.state == world::BoundaryCompositionState::Failed) return false;
    if (!boundaryRetired_.isEmpty() || !reloadMeshes_.isEmpty()) return true;
    if ((!result && !plain) || world.hasPendingWork()) return true;
    if (!pendingBoundary_ && caughtUp(world) && (plain ? (!boundary_ && plainOwner_ == world.identityToken()) :
        (boundary_ && boundary_->composition.geometry() == result->geometry()))) return true;
    const auto admitted = [&] {
        return uploads_.uploaded < budget.uploads &&
            (!budget.deadline || std::chrono::steady_clock::now() < *budget.deadline);
    };
    if (!admitted()) return true;
    if (!pendingBoundary_) {
        if (world.residentCount() > 25) return false;
        // Reserve before any handles transfer, so cancellation/commit do not allocate.
        boundaryRetired_.reserve(28);
        BoundaryUpload pending;
        pending.plain = plain; pending.regionRevision = world.regions().revision();
        pending.owner = world.identityToken();
        if (result) pending.boundary.composition = *result;
        if (plain) pending.nextPolicy = 3;
        pending.desired = world.residentChunks();
        pendingBoundary_ = std::move(pending);
    }
    const auto fail = [&] { cancelBoundaryUpload(); retireBoundaryMeshes(budget); return false; };
    if (!boundaryUploadCurrent(world)) return fail();
    try {
        auto& pending = *pendingBoundary_;
        while (pending.nextResident < pending.desired.size() && admitted()) {
            const auto& chunk = pending.desired[pending.nextResident];
            const auto old = chunks_.find(chunk.key);
            if (budget.reuseUnchanged && old != chunks_.end() && residentCurrent(old->second, world, chunk)) {
                auto resident = old->second; resident.borrowed = true;
                pending.chunks.emplace(chunk.key, std::move(resident));
                ++pending.nextResident; ++uploads_.reused;
                continue;
            }
            const auto* source = world.mesh(chunk.key);
            if (!source || source->vertices.isEmpty() != source->indices.isEmpty()) return fail();
            auto& resident = pending.chunks[chunk.key];
            resident.source = chunk.handle; resident.version = chunk.meshVersion;
            resident.origin = world.meshOrigin(chunk.key); resident.regionRevision = chunk.meshRegionRevision;
            resident.owner = world.identityToken();
            resident.geometry = std::make_shared<const world::MeshData>(*source);
            if (!source->indices.isEmpty()) {
                auto resource = world::toResourceMesh(*source);
                resource.submeshes.clear(); resource.submeshes.push({0, source->indices.size()});
                resident.mesh = renderer_.createMesh(resource);
                if (!resident.mesh.isValid()) return fail();
                resident.entity = registry_.create();
                if (!resident.entity.isValid()) return fail();
                registry_.emplace<ecs::LocalTransform>(resident.entity).position = resident.origin;
                // Deliberately invisible until the entire view is ready.
                registry_.emplace<engine::MeshRenderer>(resident.entity,
                    engine::MeshRenderer{{}, materials_[static_cast<std::size_t>(chunk.key.policy)], 0, false, false});
                uploads_.bytes += resource.indices.size();
                for (const auto& stream : resource.streams) uploads_.bytes += stream.data.size();
            }
            ++pending.nextResident; ++uploads_.uploaded;
        }
        const auto* geometry = pending.boundary.composition.geometry();
        while (pending.nextResident == pending.desired.size() && pending.nextPolicy < 3 && admitted()) {
            const auto policy = pending.nextPolicy;
            world::MeshData group;
            for (std::size_t t = 0; t < geometry->triangles.size(); ++t) {
                const auto& stamp = geometry->triangles[t];
                if (static_cast<std::size_t>(result->sources()[stamp.sourceIndex].key.policy) != policy) continue;
                for (unsigned v = 0; v < 3; ++v) {
                    group.indices.push(static_cast<std::uint32_t>(group.vertices.size()));
                    group.vertices.push(geometry->mesh.vertices[geometry->mesh.indices[t * 3 + v]]);
                }
            }
            if (!group.indices.isEmpty()) {
                auto resource = world::toResourceMesh(group);
                resource.submeshes.clear(); resource.submeshes.push({0, group.indices.size()});
                auto& mesh = pending.boundary.meshes[policy]; mesh = renderer_.createMesh(resource);
                if (!mesh.isValid() || renderer_.ensureMaterialPipeline(materials_[policy], mesh) != gfx::GfxError::None) return fail();
                auto& entity = pending.boundary.entities[policy]; entity = registry_.create();
                if (!entity.isValid()) return fail();
                registry_.emplace<ecs::LocalTransform>(entity);
                registry_.emplace<engine::MeshRenderer>(entity, engine::MeshRenderer{{}, materials_[policy], 0, false, false});
                uploads_.bytes += resource.indices.size();
                for (const auto& stream : resource.streams) uploads_.bytes += stream.data.size();
            }
            ++pending.nextPolicy; ++uploads_.uploaded;
        }
        if (pending.nextResident != pending.desired.size() || pending.nextPolicy != 3) {
            uploads_.deferred = static_cast<std::uint32_t>(pending.desired.size() - pending.nextResident + 3 - pending.nextPolicy);
            return true;
        }
        ecs::updateWorldTransforms(registry_);
        if (!boundaryUploadCurrent(world)) return fail();
        const auto acceptedSurface = makeSurfaceView(pending.chunks,
            plain ? nullptr : &pending.boundary.composition);
        if (!pending.chunks.empty() && !acceptedSurface) return fail();
        // Main-thread commit. Only handle assignments, ownership swaps and queued
        // retirement follow; no draw/query runs between these operations.
        for (auto& [key, resident] : pending.chunks) {
            const bool represented = !plain && std::any_of(result->sources().begin(), result->sources().end(),
                [&](const auto& source) { return source.key == key; });
            if (resident.entity.isValid()) registry_.get<engine::MeshRenderer>(resident.entity)->mesh = represented ? resources::MeshHandle{} : resident.mesh;
        }
        for (std::size_t policy = 0; policy < 3; ++policy)
            if (pending.boundary.entities[policy].isValid())
                registry_.get<engine::MeshRenderer>(pending.boundary.entities[policy])->mesh = pending.boundary.meshes[policy];
        for (const auto& [key, resident] : chunks_) {
            const auto next = pending.chunks.find(key);
            if (next != pending.chunks.end() && next->second.borrowed) continue;
            if (resident.entity.isValid()) registry_.destroy(resident.entity);
            if (resident.mesh.isValid()) boundaryRetired_.push(resident.mesh);
        }
        if (boundary_) {
            for (const auto entity : boundary_->entities) if (entity.isValid()) registry_.destroy(entity);
            for (const auto mesh : boundary_->meshes) if (mesh.isValid()) boundaryRetired_.push(mesh);
        }
        chunks_.swap(pending.chunks);
        for (auto& [key, resident] : chunks_) { (void)key; resident.borrowed = false; }
        if (plain) {
            boundary_.reset(); plainOwner_ = pending.owner; plainRevision_ = pending.regionRevision;
        } else {
            boundary_ = std::move(pending.boundary); plainOwner_.reset();
        }
        surfaceView_ = acceptedSurface;
        if (surfaceView_ && !world.publishSurfaceView(*surfaceView_)) return false;
        pendingBoundary_.reset();
    } catch (...) { cancelBoundaryUpload(); throw; }
    retireBoundaryMeshes(budget);
    return true;
}

auto StreamingDemoView::publishBoundaryComposition(world::WorldStreamer& world,
    const world::StreamingBoundaryComposition& composition, bool replace) -> bool {
    const auto* geometry = composition.geometry();
    if (pendingBoundary_ || !boundaryRetired_.isEmpty()) return false;
    if (!geometry || !world.boundaryCurrent(composition)) return false;
    if (replace) {
        if (world.hasPendingWork() || world.residentCount() > 25 || !reloadMeshes_.isEmpty()) return false;
    } else if (boundary_ || !caughtUp(world)) return false;
    for (const auto& source : composition.sources()) {
        if (!replace) {
            const auto found = chunks_.find(source.key);
            if (found == chunks_.end() || found->second.source != source.handle || found->second.version != source.meshVersion) return false;
        }
    }
    Boundary pending;
    pending.composition = composition;
    decltype(chunks_) pendingChunks;
    const auto rollback = [&] {
        for (const auto entity : pending.entities) if (entity.isValid()) registry_.destroy(entity);
        for (const auto mesh : pending.meshes) if (mesh.isValid()) renderer_.destroyMesh(mesh);
        for (const auto& [key, resident] : pendingChunks) {
            (void)key;
            if (resident.entity.isValid()) registry_.destroy(resident.entity);
            if (resident.mesh.isValid()) renderer_.destroyMesh(resident.mesh);
        }
    };
    try {
        if (replace) {
            for (const auto& chunk : world.residentChunks()) {
                const auto* source = world.mesh(chunk.key);
                if (!chunk.hasMesh || chunk.state != world::ChunkState::Meshed || chunk.meshVersion != chunk.version ||
                    !source || source->vertices.isEmpty() != source->indices.isEmpty()) { rollback(); return false; }
                auto& resident = pendingChunks[chunk.key]; // Own every staged handle before subsequent allocations.
                resident.source = chunk.handle; resident.version = chunk.meshVersion;
                resident.origin = world.meshOrigin(chunk.key); resident.regionRevision = chunk.meshRegionRevision;
                resident.owner = world.identityToken();
                resident.geometry = std::make_shared<const world::MeshData>(*source);
                if (source->indices.isEmpty()) continue;
                auto resource = world::toResourceMesh(*source);
                resource.submeshes.clear(); resource.submeshes.push({0, source->indices.size()});
                resident.mesh = renderer_.createMesh(resource);
                if (!resident.mesh.isValid()) { rollback(); return false; }
                resident.entity = registry_.create();
                if (!resident.entity.isValid()) { rollback(); return false; }
                registry_.emplace<ecs::LocalTransform>(resident.entity).position = resident.origin;
                registry_.emplace<engine::MeshRenderer>(resident.entity,
                    engine::MeshRenderer{resident.mesh, materials_[static_cast<std::size_t>(chunk.key.policy)], 0, false, false});
            }
        }
        std::array<world::MeshData, 3> groups;
        for (std::size_t t = 0; t < geometry->triangles.size(); ++t) {
            const auto& stamp = geometry->triangles[t];
            const auto policy = static_cast<std::size_t>(composition.sources()[stamp.sourceIndex].key.policy);
            if (policy >= groups.size()) { rollback(); return false; }
            auto& group = groups[policy];
            for (unsigned v = 0; v < 3; ++v) {
                group.indices.push(static_cast<std::uint32_t>(group.vertices.size()));
                group.vertices.push(geometry->mesh.vertices[geometry->mesh.indices[t * 3 + v]]);
            }
        }
        for (std::size_t policy = 0; policy < groups.size(); ++policy) {
            if (groups[policy].indices.isEmpty()) continue;
            auto resource = world::toResourceMesh(groups[policy]);
            resource.submeshes.clear(); resource.submeshes.push({0, groups[policy].indices.size()});
            pending.meshes[policy] = renderer_.createMesh(resource);
            if (!pending.meshes[policy].isValid() ||
                renderer_.ensureMaterialPipeline(materials_[policy], pending.meshes[policy]) != gfx::GfxError::None) {
                rollback(); return false;
            }
            pending.entities[policy] = registry_.create();
            if (!pending.entities[policy].isValid()) { rollback(); return false; }
            registry_.emplace<ecs::LocalTransform>(pending.entities[policy]); // Common zero-origin transform for tops AND walls.
            registry_.emplace<engine::MeshRenderer>(pending.entities[policy],
                engine::MeshRenderer{pending.meshes[policy], materials_[policy], 0, false, false});
        }
        ecs::updateWorldTransforms(registry_);
        if (!world.boundaryCurrent(composition)) { rollback(); return false; }
        const auto& acceptedResidents = replace ? pendingChunks : chunks_;
        const auto acceptedSurface = makeSurfaceView(acceptedResidents, &composition);
        if (!acceptedSurface || !acceptedSurface->current(world)) { rollback(); return false; }
        // The immutable surface package is prepared before any accepted-view
        // handles are switched, so render and query cannot diverge on commit.
        if (replace) {
            clear();
            chunks_.swap(pendingChunks);
        }
        for (const auto& source : composition.sources()) {
            const auto& resident = chunks_.at(source.key);
            if (resident.entity.isValid()) registry_.get<engine::MeshRenderer>(resident.entity)->mesh = {};
        }
        boundary_ = std::move(pending);
        surfaceView_ = *acceptedSurface;
        if (!world.publishSurfaceView(*surfaceView_)) return false;
        return true;
    } catch (...) { rollback(); throw; }
    return false;
}

auto StreamingDemoView::raycastDisplayedTerrain(const world::WorldStreamer& world, math::Vec3 origin,
                                                math::Vec3 direction, float maxDistance,
                                                bool cullBackFaces) const -> std::optional<world::StreamingBoundaryHit> {
    return world.raycastAcceptedTerrain(origin, direction, maxDistance, cullBackFaces);
}

auto StreamingDemoView::render(gfx::FramebufferHandle target, const engine::Camera& camera,
                               float aspect) -> gfx::GfxError {
    *registry_.get<engine::Camera>(camera_) = camera;
    return renderer_.render(target, camera_, aspect);
}

} // namespace f4::demo
