#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "core/containers/hashmap.h"
#include "core/containers/string.h"
#include "core/containers/string_hash.h"
#include "core/ids/handle.h"

using f4::containers::HashMap;
using f4::containers::String;
using f4::ids::Handle;

namespace {
struct MeshTag {};
using MeshId = Handle<MeshTag>;
} // namespace

TEST_CASE("insert never overwrites, set upserts") {
    HashMap<int, std::string> map;

    CHECK(map.insert(1, "one"));
    CHECK(!map.insert(1, "uno")); // rejected: key exists
    const auto* v = map.find(1);
    REQUIRE(v != nullptr);
    CHECK(*v == "one"); // original untouched

    map.set(1, "ONE");
    CHECK(*map.find(1) == "ONE");

    map.set(2, "two");
    CHECK(map.size() == 2);
}

TEST_CASE("find returns null for missing keys") {
    HashMap<int, int> map;
    CHECK(map.find(42) == nullptr);
    CHECK(!map.contains(42));

    map.set(42, 7);
    CHECK(map.contains(42));
    REQUIRE(map.find(42) != nullptr);
    CHECK(*map.find(42) == 7);

    CHECK(map.erase(42));
    CHECK(!map.erase(42)); // second erase reports absence
    CHECK(map.find(42) == nullptr);
}

TEST_CASE("handle keys work through the shared hash specialization") {
    HashMap<MeshId, int> map;

    const MeshId a{5, 1};
    const MeshId b{6, 0};

    CHECK(map.insert(a, 100));
    CHECK(map.insert(b, 200));
    CHECK(map.size() == 2);

    CHECK(*map.find(a) == 100);
    CHECK(*map.find(b) == 200);
    CHECK(map.find(MeshId{5, 2}) == nullptr);  // same slot, wrong generation
    CHECK(map.find(MeshId{}) == nullptr);      // null handle hashes but misses

    CHECK(map.erase(a));
    CHECK(map.find(a) == nullptr);
}

TEST_CASE("string keys hash consistently with their views") {
    HashMap<String, int> map;
    const String key("texture/albedo.png");
    CHECK(map.insert(key, 3));

    CHECK(*map.find(String("texture/albedo.png")) == 3);
}

TEST_CASE("values are reference-stable across further inserts and erases") {
    HashMap<int, std::unique_ptr<int>> map; // heap payload: address is identity
    map.reserve(4);

    int* firstAddress = nullptr;
    for (int i = 0; i < 64; ++i) {
        map.insert(i, std::make_unique<int>(i * 3));
        if (i == 0) {
            firstAddress = map.find(0)->get();
        }
    }
    // Massive growth + churn happened; the first value's address must not move.
    CHECK(map.find(0)->get() == firstAddress);

    for (int i = 10; i < 60; ++i) {
        map.erase(i);
    }
    CHECK(map.find(0)->get() == firstAddress);
    CHECK(**map.find(0) == 0);
}

namespace {
struct TrackedValue {
    static inline int instances = 0;
    int v;
    explicit TrackedValue(int x) : v(x) { ++instances; }
    TrackedValue(const TrackedValue& o) : v(o.v) { ++instances; }
    ~TrackedValue() { --instances; }
    auto operator=(const TrackedValue&) -> TrackedValue& = default;
};
} // namespace

TEST_CASE("clear destroys every stored element exactly once") {
    REQUIRE(TrackedValue::instances == 0);
    {
        HashMap<int, TrackedValue> map;
        for (int i = 0; i < 32; ++i) {
            map.insert(i, TrackedValue(i));
        }
        CHECK(TrackedValue::instances == 32);
        map.erase(7);
        CHECK(TrackedValue::instances == 31);
        map.clear();
        CHECK(TrackedValue::instances == 0);
        CHECK(map.isEmpty());
    }
    CHECK(TrackedValue::instances == 0);
}
