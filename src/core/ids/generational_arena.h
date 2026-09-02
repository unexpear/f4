#pragma once

#include <cstddef>
#include <utility>

#include "core/assert.h"
#include "core/containers/array.h"
#include "core/ids/handle.h"

namespace f4::ids {

// Fixed-capacity slot storage for payload T with per-slot generations.
//
// acquire() returns a Handle<Tag> stamped with the slot's current generation;
// release() destroys the object, stamps the slot forward, and recycles the
// index. Any handle copy made before the release now fails contains()/get()
// - stale-handle detection by construction (see M2 plan notes).
//
// Tag distinguishes handle families so two arenas can never trade handles,
// even when their payload types match.
template <typename T, typename Tag>
class GenerationalArena {
public:
    using Id = Handle<Tag>;

    explicit GenerationalArena(std::size_t capacity) {
        F4_ASSERT(capacity > 0, "generational arena needs non-zero capacity");
        // resize() value-initializes every slot: generation 0, not live.
        slots_.resize(capacity);
        freeIndices_.reserve(capacity);
        for (std::uint32_t i = 0; i < capacity; ++i) {
            // Build the free stack in reverse so acquire order is ascending.
            freeIndices_.push(static_cast<std::uint32_t>(capacity - 1 - i));
        }
    }

    GenerationalArena(const GenerationalArena&) = delete;
    auto operator=(const GenerationalArena&) -> GenerationalArena& = delete;

    ~GenerationalArena() {
        for (Slot& slot : slots_) {
            if (slot.live) {
                slot.object()->~T();
            }
        }
    }

    template <typename... Args>
    auto acquire(Args&&... args) -> Id {
        F4_ASSERT(!freeIndices_.isEmpty(), "generational arena exhausted");
        const std::uint32_t index = freeIndices_.back();
        freeIndices_.pop();
        Slot& slot = slots_[index];
        new (slot.storage) T(static_cast<Args&&>(args)...);
        slot.live = true;
        return Id{index, slot.generation};
    }

    void release(Id id) {
        Slot& slot = checkedSlot(id);
        slot.object()->~T();
        slot.live = false;
        ++slot.generation;
        freeIndices_.push(id.index);
    }

    [[nodiscard]] auto contains(Id id) const -> bool {
        if (!id.isValid() || id.index >= slots_.size()) {
            return false;
        }
        const Slot& slot = slots_[id.index];
        return slot.live && slot.generation == id.generation;
    }

    [[nodiscard]] auto get(Id id) -> T* { return contains(id) ? slots_[id.index].object() : nullptr; }

    [[nodiscard]] auto get(Id id) const -> const T* {
        return contains(id) ? slots_[id.index].object() : nullptr;
    }

    void clear() {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.live) {
                slot.object()->~T();
                slot.live = false;
                ++slot.generation;
            }
        }
        freeIndices_.clear();
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            freeIndices_.push(static_cast<std::uint32_t>(slots_.size() - 1 - i));
        }
    }

    template <typename Fn>
    void forEach(Fn&& fn) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.live) {
                fn(slot.object());
            }
        }
    }

    [[nodiscard]] auto liveCount() const -> std::size_t {
        std::size_t count = 0;
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            count += slots_[i].live ? 1 : 0;
        }
        return count;
    }

private:
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        std::uint32_t generation = 0;
        bool live = false;

        [[nodiscard]] auto object() -> T* { return reinterpret_cast<T*>(storage); }
        [[nodiscard]] auto object() const -> const T* { return reinterpret_cast<const T*>(storage); }
    };

    [[nodiscard]] auto checkedSlot(Id id) -> Slot& {
        F4_ASSERT(contains(id), "release on stale or invalid handle");
        return slots_[id.index];
    }

    f4::containers::Array<Slot> slots_;
    f4::containers::Array<std::uint32_t> freeIndices_;
};

} // namespace f4::ids
