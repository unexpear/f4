#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "core/ids/generational_arena.h"
#include "core/ids/handle.h"

using f4::ids::GenerationalArena;
using f4::ids::Handle;
using f4::ids::kInvalidHandleIndex;

namespace {
struct WidgetTag {};
struct ComponentTag {};
struct Counter {
    int value = 0;
};
using WidgetId = Handle<WidgetTag>;
using ComponentId = Handle<ComponentTag>;
using WidgetArena = GenerationalArena<Counter, WidgetTag>;
} // namespace

TEST_CASE("handles are null by default and typed by tag") {
    const WidgetId null{};
    CHECK(!null.isValid());
    CHECK(null.index == kInvalidHandleIndex);
    CHECK(null.generation == 0);

    const WidgetId a{3, 1};
    const WidgetId b{3, 1};
    const WidgetId c{3, 2};
    CHECK(a == b);
    CHECK(a != c);
    CHECK(b.isValid());

    // Cross-family handles share layout but not type:
    //   ComponentId wrong = a;   // must not compile
    (void)sizeof(ComponentId);
}

TEST_CASE("generational arena acquires ascending indices") {
    WidgetArena arena(4);

    const auto first = arena.acquire();
    const auto second = arena.acquire();
    REQUIRE(first.isValid());
    REQUIRE(second.isValid());
    CHECK(first.index == 0);
    CHECK(second.index == 1);
    CHECK(arena.liveCount() == 2);
    CHECK(arena.get(first) != nullptr);
}

TEST_CASE("generational arena recycles indices and invalidates stale handles") {
    WidgetArena arena(2);

    const auto original = arena.acquire(); // slot 0, generation 0
    REQUIRE(original.index == 0);
    CHECK(arena.contains(original));

    arena.release(original);

    // Stale handle: index exists but generation moved forward.
    CHECK(!arena.contains(original));
    CHECK(arena.get(original) == nullptr);

    // Recycled slot: same index, bumped generation.
    const auto recycled = arena.acquire();
    CHECK(recycled.index == 0);
    CHECK(recycled.generation == original.generation + 1);
    CHECK(arena.contains(recycled));
    CHECK(!arena.contains(original)); // ABA check
}

TEST_CASE("generational arena stores payloads correctly through cycles") {
    GenerationalArena<std::string, ComponentTag> arena(3);

    const auto a = arena.acquire("alpha");
    const auto b = arena.acquire("beta");
    const auto c = arena.acquire("gamma");
    REQUIRE(arena.get(a) != nullptr);
    CHECK(*arena.get(a) == "alpha");

    arena.release(b);
    const auto d = arena.acquire("delta"); // reuses b's slot
    CHECK(*arena.get(d) == "delta");
    CHECK(*arena.get(a) == "alpha"); // untouched neighbors intact
    CHECK(*arena.get(c) == "gamma");
    CHECK(!arena.contains(b));
}

TEST_CASE("released slot returns to the free stack exactly once") {
    WidgetArena arena(2);
    const auto h0 = arena.acquire();
    const auto h1 = arena.acquire();
    CHECK(h0.index == 0);
    CHECK(h1.index == 1);

    arena.release(h1);
    const auto h2 = arena.acquire();
    CHECK(h2.index == h1.index);
    CHECK(h2.generation > h1.generation);
    CHECK(arena.liveCount() == 2);
}

TEST_CASE("generational arena forEach visits only live slots") {
    WidgetArena arena(5);
    const auto a = arena.acquire();
    const auto b = arena.acquire();
    const auto c = arena.acquire();
    arena.release(b);

    int visited = 0;
    int totalValue = 0;
    arena.forEach([&visited, &totalValue](Counter* widget) {
        ++visited;
        totalValue += widget->value;
    });
    CHECK(visited == 2);
    CHECK(totalValue == 0); // default-constructed payloads
    CHECK(a != c);
}

TEST_CASE("generational arena clear kills every outstanding handle") {
    WidgetArena arena(4);
    const auto a = arena.acquire();
    const auto b = arena.acquire();
    const auto c = arena.acquire();
    arena.release(b);

    arena.clear();
    CHECK(arena.liveCount() == 0);
    CHECK(!arena.contains(a));
    CHECK(!arena.contains(c));

    const auto fresh = arena.acquire();
    const bool staleAfterClear = !arena.contains(a) && !arena.contains(c);
    CHECK(staleAfterClear);
    CHECK(arena.liveCount() == 1);
    CHECK(fresh.isValid());
}
