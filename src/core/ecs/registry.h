#pragma once

#include <cstddef>
#include <memory>
#include <typeindex>
#include <utility>

#include "core/containers/hashmap.h"
#include "core/ecs/component_storage.h"
#include "core/ecs/entity.h"
#include "core/ids/generational_arena.h"

namespace f4::ecs {

using containers::HashMap;
using ids::GenerationalArena;

class Registry;

// Owns entity lifetimes and all component storages.
//
// Contract (locked in plan §M3):
//  - Entity ids are Handle<EntityTag> minted by a GenerationalArena of tiny
//    slot records; stale-id rejection and recycling semantics are inherited
//    from the tested M2 machinery. Capacity is fixed at construction -
//    acquisition asserts when exhausted (sizing bugs surface in tests).
//  - destroy(entity) removes the entity from EVERY registered component
//    storage before releasing its handle - components never outlive their
//    entity.
//  - clear() wipes all entities and storages in one step (generations bump,
//    outstanding handles go stale, exactly like GenerationalArena::clear).
class Registry {
public:
    explicit Registry(std::size_t maxEntities) : slots_(maxEntities) {}

    auto create() -> Entity { return slots_.acquire(); }

    void destroy(Entity entity) {
        F4_ASSERT(slots_.contains(entity), "destroy on stale or invalid entity");
        for (auto& entry : storages_) {
            entry.second->removeEntity(entity);
        }
        slots_.release(entity);
    }

    [[nodiscard]] auto isAlive(Entity entity) const -> bool { return slots_.contains(entity); }

    template <typename T>
    auto storage() -> ComponentStorage<T>& {
        ComponentStorage<T>* found = findStorage<T>();
        if (found != nullptr) {
            return *found;
        }
        auto created = std::make_unique<ComponentStorage<T>>();
        ComponentStorage<T>& reference = *created;
        storages_.set(std::type_index(typeid(T)), std::move(created));
        return reference;
    }

    template <typename T>
    [[nodiscard]] auto hasStorage() const -> bool {
        return constFindStorage<T>() != nullptr;
    }

    template <typename T, typename... Args>
    auto emplace(Entity entity, Args&&... args) -> T& {
        return storage<T>().emplace(entity, static_cast<Args&&>(args)...);
    }

    template <typename T>
    [[nodiscard]] auto get(Entity entity) -> T* {
        return storage<T>().get(entity);
    }

    template <typename T>
    [[nodiscard]] auto get(Entity entity) const -> const T* {
        const ComponentStorage<T>* found = constFindStorage<T>();
        return found != nullptr ? found->get(entity) : nullptr;
    }

    template <typename T>
    auto remove(Entity entity) -> bool {
        ComponentStorage<T>* found = findStorage<T>();
        return found != nullptr ? found->tryRemove(entity) : false;
    }

    // Wipes everything: components first, then all entity handles.
    void clear() {
        storages_.clear();
        slots_.clear();
    }

    [[nodiscard]] auto liveCount() const -> std::size_t { return slots_.liveCount(); }

private:
    struct EntitySlot {};

    template <typename T>
    auto findStorage() -> ComponentStorage<T>* {
        auto* found = storages_.find(std::type_index(typeid(T)));
        return found != nullptr ? static_cast<ComponentStorage<T>*>(found->get()) : nullptr;
    }

    template <typename T>
    [[nodiscard]] auto constFindStorage() const -> const ComponentStorage<T>* {
        const auto* found = storages_.find(std::type_index(typeid(T)));
        return found != nullptr ? static_cast<const ComponentStorage<T>*>(found->get()) : nullptr;
    }

    GenerationalArena<EntitySlot, EntityTag> slots_;
    HashMap<std::type_index, std::unique_ptr<IStorage>> storages_;
};

} // namespace f4::ecs