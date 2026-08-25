#pragma once

#include <cstddef>
#include <tuple>
#include <utility>

#include "core/assert.h"
#include "core/containers/array.h"
#include "core/ecs/component_storage.h"
#include "core/ecs/entity.h"
#include "core/ecs/registry.h"

namespace f4::ecs {

using containers::Array;

// Multi-component iteration: walks the DENSE ENTITY LIST of the smallest
// requested storage and probes every requested storage with O(1) contains
// checks (the base storage's probe is redundant-but-harmless and keeps the
// implementation simple).
//
// Contract:
//  - Views are TRANSIENT: they hold raw storage pointers and are invalid once
//    the registry is destroyed or clear()ed. Structural changes to iterated
//    component types during iteration invalidate the view - collect first,
//    mutate after.
//  - Iteration order follows the smallest storage's dense order, which is
//    unspecified after removals. Never rely on ordering across runs.
template <typename... Ts>
class View {
public:
    static_assert(sizeof...(Ts) >= 1, "a view needs at least one component type");

    explicit View(Registry& registry)
        : storages_(std::make_tuple(&registry.storage<Ts>()...)) {
        // Implicit derived-to-base conversion makes this an IStorage* array.
        const IStorage* storages[] = {&registry.storage<Ts>()...};
        std::size_t smallest = 0;
        for (std::size_t i = 1; i < sizeof...(Ts); ++i) {
            if (storages[i]->entityList().size() < storages[smallest]->entityList().size()) {
                smallest = i;
            }
        }
        dense_ = &storages[smallest]->entityList();
    }

    class Iterator {
    public:
        auto operator*() const -> std::tuple<Entity, Ts&...> {
            const Entity entity = (*view_->dense_)[position_];
            return std::tuple<Entity, Ts&...>(
                entity, *std::get<ComponentStorage<Ts>*>(view_->storages_)->get(entity)...);
        }

        auto operator++() -> Iterator& {
            ++position_;
            skipToMatch();
            return *this;
        }

        [[nodiscard]] auto operator!=(const Iterator& other) const -> bool {
            return position_ != other.position_;
        }

    private:
        friend class View;

        Iterator(View* view, std::size_t position) : view_(view), position_(position) {
            skipToMatch();
        }

        void skipToMatch() {
            const std::size_t count = view_->dense_->size();
            while (position_ < count && !view_->matches((*view_->dense_)[position_])) {
                ++position_;
            }
        }

        View* view_;
        std::size_t position_;
    };

    [[nodiscard]] auto begin() -> Iterator { return Iterator(this, 0); }
    [[nodiscard]] auto end() -> Iterator { return Iterator(this, dense_->size()); }

private:
    [[nodiscard]] auto matches(Entity entity) const -> bool {
        return (std::get<ComponentStorage<Ts>*>(storages_)->contains(entity) && ...);
    }

    std::tuple<ComponentStorage<Ts>*...> storages_;
    const Array<Entity>* dense_ = nullptr;
};

} // namespace f4::ecs