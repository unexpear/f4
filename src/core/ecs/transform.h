#pragma once

#include <utility>

#include "core/assert.h"
#include "core/containers/array.h"
#include "core/containers/hashmap.h"
#include "core/ecs/registry.h"
#include "core/ecs/view.h"
#include "core/math/mat4.h"
#include "core/math/transform.h"

namespace f4::ecs {

using containers::Array;
using containers::HashMap;

// Transform hierarchy built on the M3 ECS and the M1 math layer
// (math::Transform / composeTRS / composeHierarchy - nothing duplicated).
//
// Contracts (locked at M4):
//  - LocalTransform IS math::Transform (position/rotation/scale TRS).
//  - PARENT PRESENCE marks non-root: an entity without a Parent component is
//    a root. No sentinel handles. A Parent pointing at a dead entity - or at
//    an entity without its own LocalTransform - detaches that child to root
//    on the next pass (orphan policy), so destroying a parent mid-frame can
//    never corrupt the next pass.
//  - WorldMatrix components are OWNED BY THE PASS: systems read them, never
//    write them. The pass creates missing ones and updates dirty ones.
//  - DIRTY PROPAGATION: mutators below stamp TransformDirty onto the edited
//    entity only. The pass carries an inherited-dirty flag down the DFS, so
//    tagging an ancestor recomputes its whole subtree without touching the
//    descendants' tags. The pass CLEARS every tag it consumes; after it
//    returns, no TransformDirty remains anywhere.
//  - REPARENTING WORLD-JUMPS by design: locals are kept as-is and the world
//    changes on the next pass. Preserve-world reparenting
//    (inverse(newParentWorld) * oldWorld) is a separate future contract -
//    deliberately NOT part of this one (it couples reparenting to matrix
//    inversion and non-uniform-scale edge cases).
using LocalTransform = math::Transform;

struct Parent {
    Entity entity;
};

struct WorldMatrix {
    math::Mat4 matrix;
};

struct TransformDirty {};

// --- Mutators (stamp TransformDirty) -----------------------------------------

inline void tagTransformDirty(Registry& registry, Entity entity) {
    F4_ASSERT(registry.isAlive(entity), "cannot dirty a dead entity");
    if (registry.get<TransformDirty>(entity) == nullptr) {
        registry.emplace<TransformDirty>(entity);
    }
}

inline void setLocalPosition(Registry& registry, Entity entity, const math::Vec3& position) {
    auto* local = registry.get<LocalTransform>(entity);
    F4_ASSERT(local != nullptr, "entity has no LocalTransform");
    local->position = position;
    tagTransformDirty(registry, entity);
}

inline void setLocalRotation(Registry& registry, Entity entity, const math::Quat& rotation) {
    auto* local = registry.get<LocalTransform>(entity);
    F4_ASSERT(local != nullptr, "entity has no LocalTransform");
    local->rotation = rotation;
    tagTransformDirty(registry, entity);
}

inline void setLocalScale(Registry& registry, Entity entity, const math::Vec3& scale) {
    auto* local = registry.get<LocalTransform>(entity);
    F4_ASSERT(local != nullptr, "entity has no LocalTransform");
    local->scale = scale;
    tagTransformDirty(registry, entity);
}

// --- Hierarchy edits ----------------------------------------------------------

// Reassigns the child's parent. Returns false (no side effects) if this
// would create a cycle, including child == parent.
//
// Cycle-walk termination: edges are created ONLY here, and creation that
// would close a cycle is rejected before the edge exists - so the live
// parent graph is always acyclic and walking ancestors terminates.
[[nodiscard]] inline auto trySetParent(Registry& registry, Entity child, Entity parent) -> bool {
    F4_ASSERT(registry.isAlive(child), "cannot reparent a dead child");
    F4_ASSERT(registry.isAlive(parent), "cannot reparent under a dead parent");
    if (child == parent) {
        return false;
    }

    Entity ancestor = parent;
    while (true) {
        if (ancestor == child) {
            return false; // parent sits in child's subtree
        }
        const Parent* up = registry.get<Parent>(ancestor);
        if (up == nullptr || !registry.isAlive(up->entity)) {
            break;
        }
        ancestor = up->entity;
    }

    if (auto* existing = registry.get<Parent>(child)) {
        existing->entity = parent;
    } else {
        registry.emplace<Parent>(child, parent);
    }
    tagTransformDirty(registry, child);
    return true;
}

inline void setParent(Registry& registry, Entity child, Entity parent) {
    F4_VERIFY(trySetParent(registry, child, parent), "setParent on rejected reparent");
}

// Detaches to root. No-op if the entity had no parent.
inline void clearParent(Registry& registry, Entity child) {
    if (registry.remove<Parent>(child)) {
        tagTransformDirty(registry, child);
    }
}

// --- World-matrix recomputation pass ------------------------------------------

// Recomputes world matrices where needed: entities tagged directly, entities
// under a dirty ancestor, and any entity whose WorldMatrix does not exist
// yet. Clears consumed tags. O(n) over transformable entities.
inline void updateWorldTransforms(Registry& registry) {
    // Collect roots and parent->children buckets in one sweep. A parent
    // counts as "attached" only if it is alive AND itself has a
    // LocalTransform; a DEAD parent link is PRUNED here (orphan policy):
    // the child detaches to root on this very pass, so it can never keep a
    // stale pre-death world matrix. A live-but-transformless parent merely
    // parks the child at root until the parent gains a LocalTransform.
    HashMap<Entity, Array<Entity>> childBuckets;
    Array<Entity> roots;
    for (auto [entity, local] : View<LocalTransform>(registry)) {
        (void)local;
        const Parent* parent = registry.get<Parent>(entity);
        const bool parentAlive = parent != nullptr && registry.isAlive(parent->entity);
        if (parent != nullptr && !parentAlive) {
            registry.remove<Parent>(entity);
            tagTransformDirty(registry, entity);
            roots.push(entity);
            continue;
        }
        const bool attached = parentAlive &&
                              registry.get<LocalTransform>(parent->entity) != nullptr;
        if (!attached) {
            roots.push(entity);
            continue;
        }
        if (Array<Entity>* bucket = childBuckets.find(parent->entity)) {
            bucket->push(entity);
        } else {
            Array<Entity> fresh;
            fresh.push(entity);
            childBuckets.set(parent->entity, static_cast<Array<Entity>&&>(fresh));
        }
    }

    struct Frame {
        Entity entity;
        math::Mat4 parentWorld;
        bool inheritedDirty;
    };

    Array<Frame> stack;
    stack.reserve(roots.size());
    for (const Entity root : roots) {
        stack.push(Frame{root, math::Mat4::identity(), false});
    }

    while (!stack.isEmpty()) {
        Frame frame = stack.back();
        stack.pop();

        const bool selfDirty = registry.get<TransformDirty>(frame.entity) != nullptr;
        const bool dirty = selfDirty || frame.inheritedDirty;

        auto* worldMatrix = registry.get<WorldMatrix>(frame.entity);
        if (dirty || worldMatrix == nullptr) {
            // Guaranteed present: frames only enter through the sweep above.
            const auto* local = registry.get<LocalTransform>(frame.entity);
            const math::Mat4 world =
                math::composeHierarchy(frame.parentWorld, math::composeTRS(*local));
            if (worldMatrix != nullptr) {
                worldMatrix->matrix = world;
            } else {
                registry.emplace<WorldMatrix>(frame.entity, world);
            }
            if (selfDirty) {
                registry.remove<TransformDirty>(frame.entity);
            }
        }

        if (const Array<Entity>* bucket = childBuckets.find(frame.entity)) {
            const math::Mat4 worldForChildren = registry.get<WorldMatrix>(frame.entity)->matrix;
            for (const Entity child : *bucket) {
                stack.push(Frame{child, worldForChildren, dirty});
            }
        }
    }
}

// True when the pass left nothing pending - used by tests to pin the dirty
// state-machine contract.
[[nodiscard]] inline auto transformsAreClean(const Registry& registry) -> bool {
    if (!registry.hasStorage<TransformDirty>()) {
        return true;
    }
    return registry.storage<TransformDirty>().isEmpty();
}

} // namespace f4::ecs