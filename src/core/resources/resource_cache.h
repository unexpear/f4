#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <typeindex>
#include <utility>

#include "core/assert.h"
#include "core/containers/hashmap.h"
#include "core/containers/string.h"
#include "core/containers/string_hash.h"
#include "core/ids/generational_arena.h"
#include "core/resources/buffer.h"
#include "core/resources/mesh.h"
#include "core/resources/resource_ids.h"
#include "core/resources/shader.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

using containers::HashMap;
using containers::String;
using containers::StringView;
using ids::GenerationalArena;
using ids::Handle;

template <typename Tag>
[[nodiscard]] inline auto handleKey(ids::Handle<Tag> h) -> std::uint64_t {
    return (static_cast<std::uint64_t>(h.generation) << 32) | static_cast<std::uint64_t>(h.index);
}

// Type-erased base so clear() can tear down every family without knowing T.
class IFamily {
public:
    virtual ~IFamily() = default;
    virtual void destroyAll() = 0;
};

// One family per tag: a fixed-capacity arena (stale-handle safety inherited
// from M2) plus bidirectional path registration. Path policy is EXACT match,
// case-sensitive, documented in plan §M5.
template <typename Tag>
class ResourceFamily final : public IFamily {
public:
    static constexpr std::size_t kCapacity = 1024;

    ResourceFamily() : arena(kCapacity) {}

    HashMap<String, Handle<Tag>> byPath;
    HashMap<std::uint64_t, String> pathOf;
    HashMap<std::uint64_t, std::uint32_t> references;
    GenerationalArena<typename ResourceTrait<Tag>::type, Tag> arena;

    void destroyAll() override {
        arena.clear();
        byPath.clear();
        pathOf.clear();
        references.clear();
    }
};

// Central resource registry. Opaque handles only above this line; records are
// plain data owned by the arenas.
class ResourceCache {
public:
    // Creates an untracked record. Exhaustion asserts (arena contract).
    template <typename Tag, typename... Args>
    auto create(Args&&... args) -> Handle<Tag> {
        ResourceFamily<Tag>& f = family<Tag>();
        const Handle<Tag> handle = f.arena.acquire(static_cast<Args&&>(args)...);
        const bool inserted = f.references.insert(handleKey(handle), 1);
        F4_ASSERT(inserted, "resource reference bookkeeping invariant broken");
        return handle;
    }

    // Creates AND registers a canonical path. Returns a null handle if the
    // path is already taken - callers decide whether that means getOrLoad.
    template <typename Tag, typename... Args>
    auto createNamed(StringView path, Args&&... args) -> Handle<Tag> {
        ResourceFamily<Tag>& f = family<Tag>();
        const String key(StringView(path.data(), path.size()));
        if (f.byPath.find(key) != nullptr) {
            return Handle<Tag>{};
        }
        String owned(key);
        const Handle<Tag> handle = f.arena.acquire(static_cast<Args&&>(args)...);
        const bool insertedPath = f.byPath.insert(owned, handle);
        const bool insertedReverse = f.pathOf.insert(handleKey(handle), static_cast<String&&>(owned));
        const bool insertedReference = f.references.insert(handleKey(handle), 1);
        F4_ASSERT(insertedPath && insertedReverse && insertedReference,
                  "path bookkeeping invariant broken");
        return handle;
    }

    // Returns the existing named resource, or invokes loader once and stores
    // its successful value. Loader errors pass through without mutating the
    // cache. The loader must return Result<ResourceTrait<Tag>::type, E>.
    template <typename Tag, typename Loader>
    auto getOrLoad(StringView path, Loader&& loader)
        -> Result<Handle<Tag>, decltype(std::declval<Loader&&>()().error())> {
        const String ownedPath(path);
        if (const Handle<Tag> existing = find<Tag>(ownedPath.view()); existing.isValid()) {
            const bool retained = retain(existing);
            F4_ASSERT(retained, "resource reference bookkeeping invariant broken");
            return existing;
        }

        auto loaded = static_cast<Loader&&>(loader)();
        if (loaded.hasError()) {
            return loaded.error();
        }

        const Handle<Tag> created = createNamed<Tag>(
            ownedPath.view(), static_cast<typename ResourceTrait<Tag>::type&&>(loaded.value()));
        if (created.isValid()) {
            return created;
        }

        const Handle<Tag> existing = find<Tag>(ownedPath.view());
        F4_ASSERT(existing.isValid(), "resource path bookkeeping invariant broken");
        const bool retained = retain(existing);
        F4_ASSERT(retained, "resource reference bookkeeping invariant broken");
        return existing;
    }

    template <typename Tag>
    [[nodiscard]] auto get(Handle<Tag> handle) -> typename ResourceTrait<Tag>::type* {
        return family<Tag>().arena.get(handle);
    }

    template <typename Tag>
    [[nodiscard]] auto contains(Handle<Tag> handle) -> bool {
        return family<Tag>().arena.contains(handle);
    }

    // Pointer-or-null handle for a registered path; null when unregistered OR
    // when the registered handle has since been destroyed (destroy() removes
    // the mapping, so in practice null simply means absent).
    template <typename Tag>
    [[nodiscard]] auto find(StringView path) -> Handle<Tag> {
        const String key(StringView(path.data(), path.size()));
        const Handle<Tag>* found = family<Tag>().byPath.find(key);
        return found == nullptr ? Handle<Tag>{} : *found;
    }

    template <typename Tag>
    auto retain(Handle<Tag> handle) -> bool {
        ResourceFamily<Tag>& f = family<Tag>();
        std::uint32_t* count = f.references.find(handleKey(handle));
        if (!f.arena.contains(handle) || count == nullptr ||
            *count == std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        ++*count;
        return true;
    }

    template <typename Tag>
    [[nodiscard]] auto referenceCount(Handle<Tag> handle) -> std::uint32_t {
        ResourceFamily<Tag>& f = family<Tag>();
        const std::uint32_t* count = f.references.find(handleKey(handle));
        return f.arena.contains(handle) && count != nullptr ? *count : 0;
    }

    // Drops one ownership reference. The record and canonical path are
    // removed only when the final reference is released.
    template <typename Tag>
    auto release(Handle<Tag> handle) -> bool {
        ResourceFamily<Tag>& f = family<Tag>();
        std::uint32_t* count = f.references.find(handleKey(handle));
        if (!f.arena.contains(handle) || count == nullptr || *count == 0) return false;
        if (*count > 1) {
            --*count;
            return true;
        }
        if (const String* path = f.pathOf.find(handleKey(handle))) {
            f.byPath.erase(*path);
            f.pathOf.erase(handleKey(handle));
        }
        f.references.erase(handleKey(handle));
        f.arena.release(handle);
        return true;
    }

    // Backward-compatible name for releasing an ownership reference.
    template <typename Tag>
    auto destroy(Handle<Tag> handle) -> bool {
        return release(handle);
    }

    template <typename Tag>
    [[nodiscard]] auto liveCount() -> std::size_t {
        return family<Tag>().arena.liveCount();
    }

    void clear() {
        for (auto& [typeIndex, fam] : families_) {
            fam->destroyAll();
        }
    }

private:
    template <typename Tag>
    [[nodiscard]] auto family() -> ResourceFamily<Tag>& {
        const std::type_index key(typeid(ResourceFamily<Tag>));
        if (auto* existing = families_.find(key)) {
            return *static_cast<ResourceFamily<Tag>*>(existing->get());
        }
        auto created = std::make_unique<ResourceFamily<Tag>>();
        ResourceFamily<Tag>& ref = *created;
        families_.set(key, std::unique_ptr<IFamily>(std::move(created)));
        return ref;
    }

    HashMap<std::type_index, std::unique_ptr<IFamily>> families_;
};

} // namespace f4::resources
