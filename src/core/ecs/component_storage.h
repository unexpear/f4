#pragma once

#include <cstddef>
#include <utility>

#include "core/assert.h"
#include "core/containers/array.h"
#include "core/ecs/entity.h"

namespace f4::ecs {

using containers::Array;

// Type-erased view over any ComponentStorage<T>: lets Registry sweep
// components on destroy and lets View pick iteration roots without knowing T.
class IStorage {
public:
    virtual ~IStorage() = default;
    virtual void removeEntity(Entity entity) = 0;
    [[nodiscard]] virtual auto entityList() const -> const Array<Entity>& = 0;
};

// Sparse-set component storage: the engine's core data structure.
//
// Contract (locked in plan §M3):
//  - Dense parallel arrays hold components and their owning entities; a
//    sparse array maps entity INDEX to dense position.
//  - Removal is swap-and-pop: dense ORDER changes on erase. Pointers and
//    references into dense storage are invalidated by any removal. Sort
//    explicitly if a consumer needs order.
//  - Lookup validates the FULL entity handle against the dense entity list,
//    so a stale recycled id can never observe or fetch a newer entity's
//    component.
//  - get() returns pointer-or-null, matching the house convention
//    (GenerationalArena::get, HashMap::find).
template <typename T>
class ComponentStorage final : public IStorage {
public:
    template <typename... Args>
    auto emplace(Entity entity, Args&&... args) -> T& {
        F4_ASSERT(!contains(entity), "component already present for entity");
        const std::uint32_t index = entity.index;
        ensureSparse(index);
        T& slot = components_.emplaceBack(static_cast<Args&&>(args)...);
        entities_.push(entity);
        sparse_[index] = static_cast<std::uint32_t>(entities_.size() - 1);
        return slot;
    }

    auto add(Entity entity, const T& value) -> T& { return emplace(entity, value); }

    [[nodiscard]] auto contains(Entity entity) const -> bool {
        const std::uint32_t index = entity.index;
        if (index >= sparse_.size()) {
            return false;
        }
        const std::uint32_t densePosition = sparse_[index];
        return densePosition < entities_.size() && entities_[densePosition] == entity;
    }

    [[nodiscard]] auto get(Entity entity) -> T* {
        if (!contains(entity)) {
            return nullptr;
        }
        return &components_[sparse_[entity.index]];
    }

    [[nodiscard]] auto get(Entity entity) const -> const T* {
        if (!contains(entity)) {
            return nullptr;
        }
        return &components_[sparse_[entity.index]];
    }

    // Removes if present, reporting whether anything was removed. Never
    // asserts - used by Registry's destroy sweep across all storages.
    auto tryRemove(Entity entity) -> bool {
        if (!contains(entity)) {
            return false;
        }
        const std::uint32_t position = sparse_[entity.index];
        const std::uint32_t last = static_cast<std::uint32_t>(entities_.size() - 1);
        if (position != last) {
            // Move the final pair into the hole and fix its sparse entry.
            components_[position] = static_cast<T&&>(components_[last]);
            entities_[position] = entities_[last];
            sparse_[entities_[position].index] = position;
        }
        components_.pop();
        entities_.pop();
        return true;
    }

    void remove(Entity entity) {
        F4_ASSERT(tryRemove(entity), "remove on entity without this component");
    }

    [[nodiscard]] auto size() const -> std::size_t { return entities_.size(); }
    [[nodiscard]] auto isEmpty() const -> bool { return entities_.isEmpty(); }

    void removeEntity(Entity entity) override { tryRemove(entity); }

    [[nodiscard]] auto entityList() const -> const Array<Entity>& override { return entities_; }

    [[nodiscard]] auto entities() const -> const Array<Entity>& { return entities_; }
    [[nodiscard]] auto components() -> Array<T>& { return components_; }
    [[nodiscard]] auto components() const -> const Array<T>& { return components_; }

private:
    // Sparse entries are value-initialized to 0; garbage zeros are harmless
    // because every lookup re-validates identity through the dense arrays.
    void ensureSparse(std::uint32_t index) {
        if (index >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(index) + 1);
        }
    }

    Array<T> components_;
    Array<Entity> entities_;
    Array<std::uint32_t> sparse_;
};

} // namespace f4::ecs