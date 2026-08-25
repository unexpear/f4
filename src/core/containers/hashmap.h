#pragma once

#include <cstddef>
#include <unordered_map>
#include <utility>

#include "core/assert.h"

namespace f4::containers {

// Engine-facing hash map over std::unordered_map (per plan: wrappers).
//
// Semantics:
//  - insert(k, v): fails (returns false) if the key exists - existing value
//    untouched. set(k, v): upserts unconditionally.
//  - find(k): pointer-or-null, mirroring GenerationalArena::get. Pointers and
//    references to values stay valid across insert/erase of OTHER keys
//    (node-based backing); erasing a key invalidates that key only.
//  - Iteration order is UNSPECIFIED and may vary between runs, compilers, or
//    insertion histories. Any logic requiring determinism must sort keys
//    explicitly before consuming.
//  - Keys need std::hash + operator==: Handle<Tag>, String/StringView are
//    already wired.
template <typename K, typename V>
class HashMap {
public:
    HashMap() = default;

    explicit HashMap(std::size_t initialBuckets) { map_.reserve(initialBuckets); }

    auto insert(const K& key, const V& value) -> bool {
        return map_.emplace(key, value).second;
    }

    auto insert(const K& key, V&& value) -> bool {
        return map_.emplace(key, static_cast<V&&>(value)).second;
    }

    void set(const K& key, const V& value) { map_[key] = value; }

    [[nodiscard]] auto find(const K& key) -> V* {
        const auto it = map_.find(key);
        return it != map_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] auto find(const K& key) const -> const V* {
        const auto it = map_.find(key);
        return it != map_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] auto contains(const K& key) const -> bool { return map_.find(key) != map_.end(); }

    auto erase(const K& key) -> bool { return map_.erase(key) > 0; }

    void reserve(std::size_t elements) { map_.reserve(elements); }
    void clear() { map_.clear(); }

    [[nodiscard]] auto size() const -> std::size_t { return map_.size(); }
    [[nodiscard]] auto isEmpty() const -> bool { return map_.empty(); }

    [[nodiscard]] auto begin() { return map_.begin(); }
    [[nodiscard]] auto end() { return map_.end(); }
    [[nodiscard]] auto begin() const { return map_.begin(); }
    [[nodiscard]] auto end() const { return map_.end(); }

private:
    std::unordered_map<K, V> map_;
};

} // namespace f4::containers