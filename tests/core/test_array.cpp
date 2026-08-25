#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>

#include "core/containers/array.h"

using f4::containers::Array;

TEST_CASE("array pushes, grows, and reports sizes") {
    Array<int> a;
    CHECK(a.isEmpty());
    CHECK(a.size() == 0);
    CHECK(a.capacity() == 0);

    for (int i = 0; i < 10; ++i) {
        a.push(i);
    }
    CHECK(a.size() == 10);
    CHECK(a.capacity() >= 10);
    for (int i = 0; i < 10; ++i) {
        CHECK(a[i] == i);
    }
}

TEST_CASE("array reserve avoids reallocation") {
    Array<int> a(32);
    CHECK(a.capacity() >= 32);
    CHECK(a.size() == 0);

    const int* dataBefore = a.data();
    for (int i = 0; i < 32; ++i) {
        a.push(i);
    }
    CHECK(a.data() == dataBefore); // no growth happened
}

namespace {
struct LifetimeTracker {
    static inline int liveCount = 0;
    int value;
    explicit LifetimeTracker(int v) : value(v) { ++liveCount; }
    LifetimeTracker(const LifetimeTracker& o) : value(o.value) { ++liveCount; }
    LifetimeTracker(LifetimeTracker&& o) noexcept : value(o.value) { ++liveCount; }
    auto operator=(const LifetimeTracker&) -> LifetimeTracker& = default;
    ~LifetimeTracker() { --liveCount; }
};
} // namespace

TEST_CASE("array destroys exactly the elements it owns") {
    REQUIRE(LifetimeTracker::liveCount == 0);
    {
        Array<LifetimeTracker> a;
        for (int i = 0; i < 20; ++i) {
            a.emplaceBack(i);
        }
        CHECK(LifetimeTracker::liveCount == 20);

        a.removeSwap(5);
        CHECK(LifetimeTracker::liveCount == 19);
        CHECK(a.size() == 19);

        a.clear();
        CHECK(LifetimeTracker::liveCount == 0);
        CHECK(a.size() == 0);

        for (int i = 0; i < 3; ++i) {
            a.emplaceBack(i);
        }
        CHECK(LifetimeTracker::liveCount == 3);
    }
    CHECK(LifetimeTracker::liveCount == 0);
}

TEST_CASE("array copy is deep, move steals storage") {
    Array<std::string> original;
    original.push("alpha");
    original.push("beta");

    Array<std::string> copy = original;
    CHECK(copy.size() == 2);
    CHECK(copy[0] == "alpha");

    original[0] = "changed";
    CHECK(copy[0] == "alpha"); // deep: no aliasing

    const std::string* movedData = copy.data();
    Array<std::string> stolen = std::move(copy);
    CHECK(stolen.data() == movedData);
    CHECK(copy.size() == 0);
    CHECK(copy.data() == nullptr);
}

TEST_CASE("array supports move-only element types") {
    Array<std::unique_ptr<int>> a;
    a.push(std::make_unique<int>(7));
    a.push(std::make_unique<int>(9));

    // Growth path moves elements between buffers.
    for (int i = 0; i < 8; ++i) {
        a.push(std::make_unique<int>(i));
    }
    REQUIRE(a.size() == 10);
    CHECK(*a[0] == 7);
    CHECK(*a[1] == 9);

    a.removeSwap(0);
    REQUIRE(a.size() == 9);
    // Index 0 now holds the element swapped in from the tail (the last
    // pushed value, 7); 9 stays untouched at index 1.
    CHECK(*a[0] == 7);
    CHECK(*a[1] == 9);

    // pop destroys the moved-out tail correctly.
    a.pop();
    CHECK(a.size() == 8);
}

TEST_CASE("array removeSwap preserves all other elements") {
    Array<int> a;
    for (int i = 0; i < 6; ++i) {
        a.push(i * 11);
    }
    a.removeSwap(2); // removes 22, swaps in last (55)
    CHECK(a.size() == 5);
    bool found22 = false;
    bool sawAllOthers = true;
    for (const int v : a) {
        if (v == 22) {
            found22 = true;
        }
    }
    CHECK(!found22);
    for (const int expected : {0, 11, 33, 44}) {
        bool seen = false;
        for (const int v : a) {
            seen = seen || v == expected;
        }
        sawAllOthers = sawAllOthers && seen;
    }
    CHECK(sawAllOthers);
}

TEST_CASE("array resize value-initializes and shrinks") {
    Array<double> a;
    a.resize(4);
    CHECK(a.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(a[i] == 0.0); // value-initialized
    }
    a[2] = 5.0;
    a.resize(2);
    CHECK(a.size() == 2);
}
