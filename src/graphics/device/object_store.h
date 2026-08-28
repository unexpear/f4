#pragma once

#include <cstddef>

#include "core/ids/generational_arena.h"
#include "graphics/handle.h"

namespace f4::gfx {

// Thin type-safe wrapper over ids::GenerationalArena for a gfx object family.
// Each Gpu* record type binds to one Handle<Tag>. The capacity is a shared
// per-family cap (see handle.cpp) — exhaustion asserts per the arena contract.
template <typename Record, typename Tag>
class ObjectStore {
public:
    static constexpr std::size_t kCapacity = 1024;

    ObjectStore() = default;
    ObjectStore(const ObjectStore&) = delete;
    auto operator=(const ObjectStore&) -> ObjectStore& = delete;

    template <typename... Args>
    auto create(Args&&... args) -> ids::Handle<Tag> {
        return arena_.acquire(static_cast<Args&&>(args)...);
    }

    auto destroy(ids::Handle<Tag> id) -> bool {
        if (!arena_.contains(id)) {
            return false;
        }
        arena_.release(id);
        return true;
    }

    [[nodiscard]] auto get(ids::Handle<Tag> id) -> Record* { return arena_.get(id); }
    [[nodiscard]] auto contains(ids::Handle<Tag> id) const -> bool { return arena_.contains(id); }
    [[nodiscard]] auto liveCount() const -> std::size_t { return arena_.liveCount(); }

    // Visit every live record (used by backends to release GPU objects on
    // device teardown).
    template <typename Fn>
    void forEach(Fn&& fn) {
        arena_.forEach(static_cast<Fn&&>(fn));
    }

private:
    ids::GenerationalArena<Record, Tag> arena_{kCapacity};
};

} // namespace f4::gfx
