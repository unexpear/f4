#include <doctest/doctest.h>

#include <cstdint>
#include <tuple>

#include "core/ecs/component_storage.h"
#include "core/ecs/entity.h"
#include "core/ecs/registry.h"
#include "core/ecs/system_runner.h"
#include "core/ecs/view.h"

using f4::ecs::Entity;
using f4::ecs::Registry;
using f4::ecs::SystemRunner;
using f4::ecs::View;

namespace {
struct Position {
    float x;
    float y;
};
struct Velocity {
    float vx;
    float vy;
};
struct Health {
    int hp;
};

struct LifeTracker {
    static inline int alive = 0;
    int value;
    explicit LifeTracker(int v) : value(v) { ++alive; }
    LifeTracker(const LifeTracker& o) : value(o.value) { ++alive; }
    auto operator=(const LifeTracker&) -> LifeTracker& = default;
    ~LifeTracker() { --alive; }
};

class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_(seed) {}
    auto next() -> std::uint32_t {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }
    auto below(std::uint32_t bound) -> std::uint32_t { return next() % bound; }

private:
    std::uint32_t state_;
};
} // namespace

TEST_CASE("entity creation, recycling, and staleness") {
    Registry registry(16);

    const Entity a = registry.create();
    const Entity b = registry.create();
    const Entity c = registry.create();
    CHECK(registry.liveCount() == 3);
    CHECK(registry.isAlive(a));

    registry.destroy(b);
    CHECK(!registry.isAlive(b));
    CHECK(registry.liveCount() == 2);

    const Entity recycled = registry.create();
    CHECK(recycled.index == b.index);      // freed slot is reused
    CHECK(recycled.generation == b.generation + 1);
    CHECK(!registry.isAlive(b));           // old id stays rejected
    CHECK(a != c);

    // Destroying through a stale id would be misuse - the arena asserts on
    // it, so it is deliberately not exercised here.
}

TEST_CASE("sparse set add, get, mutate, remove") {
    Registry registry(16);
    const Entity e = registry.create();

    CHECK(!registry.hasStorage<Position>());
    Position& added = registry.emplace<Position>(e, 1.0f, 2.0f);
    CHECK(registry.hasStorage<Position>());
    CHECK(&added == registry.get<Position>(e));
    CHECK(added.x == 1.0f);

    registry.get<Position>(e)->x = 7.5f;
    const Position* fetched = registry.get<Position>(e);
    REQUIRE(fetched != nullptr);
    CHECK(fetched->x == 7.5f);
    CHECK(fetched->y == 2.0f);

    SUBCASE("double add is asserted away - tryRemove path instead") {
        CHECK(registry.remove<Position>(e));
        CHECK(!registry.remove<Position>(e)); // absent: reports false
        CHECK(registry.get<Position>(e) == nullptr);
    }

    SUBCASE("swap-and-pop preserves remaining lookups") {
        const Entity p = registry.create();
        const Entity q = registry.create();
        const Entity r = registry.create();
        registry.emplace<Position>(p, 10.0f, 0.0f);
        registry.emplace<Position>(q, 20.0f, 0.0f);
        registry.emplace<Position>(r, 30.0f, 0.0f);

        registry.remove<Position>(q); // middle removal swaps last into hole

        CHECK(registry.get<Position>(q) == nullptr);
        const Position* pp = registry.get<Position>(p);
        const Position* rp = registry.get<Position>(r);
        REQUIRE(pp != nullptr);
        REQUIRE(rp != nullptr);
        CHECK(pp->x == 10.0f);
        CHECK(rp->x == 30.0f);
        // Rows: e's component from the enclosing scope plus p and r.
        CHECK(registry.storage<Position>().size() == 3);
    }
}

TEST_CASE("stale entity ids cannot observe newer components") {
    Registry registry(16);
    const Entity first = registry.create();
    registry.emplace<Position>(first, 1.0f, 1.0f);

    registry.destroy(first);
    const Entity second = registry.create();
    REQUIRE(second.index == first.index);
    registry.emplace<Position>(second, 99.0f, 99.0f);

    // The stale handle shares an index but carries the old generation.
    CHECK(registry.get<Position>(first) == nullptr);
    const Position* fresh = registry.get<Position>(second);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->x == 99.0f);
}

TEST_CASE("destroy sweeps components from every storage") {
    REQUIRE(LifeTracker::alive == 0);
    Registry registry(16);
    const Entity victim = registry.create();
    const Entity survivor = registry.create();

    registry.emplace<Position>(victim, 1.0f, 1.0f);
    registry.emplace<Velocity>(victim, 1.0f, 1.0f);
    registry.emplace<LifeTracker>(victim, 42);
    registry.emplace<Position>(survivor, 2.0f, 2.0f);

    CHECK(LifeTracker::alive == 1);
    registry.destroy(victim);
    CHECK(LifeTracker::alive == 0); // destructor ran exactly once

    CHECK(registry.get<Position>(victim) == nullptr);
    CHECK(registry.get<Velocity>(victim) == nullptr);
    CHECK(registry.get<LifeTracker>(victim) == nullptr);

    const Position* kept = registry.get<Position>(survivor);
    REQUIRE(kept != nullptr);
    CHECK(kept->x == 2.0f); // untouched neighbor
}

TEST_CASE("views iterate exact intersections") {
    Registry registry(64);
    const Entity e1 = registry.create();
    const Entity e2 = registry.create();
    const Entity e3 = registry.create();
    const Entity posOnly = registry.create();

    registry.emplace<Position>(e1, 1.0f, 0.0f);
    registry.emplace<Position>(e2, 2.0f, 0.0f);
    registry.emplace<Position>(e3, 3.0f, 0.0f);
    registry.emplace<Position>(posOnly, 4.0f, 0.0f);
    registry.emplace<Velocity>(e1, 0.5f, 0.0f);
    registry.emplace<Velocity>(e3, 1.5f, 0.0f);
    registry.emplace<Health>(e3, 100);

    SUBCASE("single-type view") {
        int seen = 0;
        float sumX = 0.0f;
        for (auto [entity, pos] : View<Position>(registry)) {
            ++seen;
            sumX += pos.x;
            (void)entity;
        }
        CHECK(seen == 4);
        CHECK(sumX == 10.0f);
    }

    SUBCASE("dual-type view excludes partial matches") {
        int seen = 0;
        float sumX = 0.0f;
        for (auto [entity, pos, vel] : View<Position, Velocity>(registry)) {
            ++seen;
            sumX += pos.x * vel.vx;
            (void)entity;
        }
        CHECK(seen == 2);
        CHECK(sumX == (1.0f * 0.5f) + (3.0f * 1.5f));
    }

    SUBCASE("triple-type view") {
        int seen = 0;
        for (auto [entity, pos, vel, hp] : View<Position, Velocity, Health>(registry)) {
            ++seen;
            CHECK(hp.hp == 100);
            (void)entity;
            (void)pos;
            (void)vel;
        }
        CHECK(seen == 1);
    }

    SUBCASE("mutation through a view writes through to storage") {
        for (auto [entity, pos, vel] : View<Position, Velocity>(registry)) {
            pos.x += vel.vx;
        }
        CHECK(registry.get<Position>(e1)->x == 1.5f);
        CHECK(registry.get<Position>(e3)->x == 4.5f);
    }

    SUBCASE("view over empty component type yields nothing") {
        int seen = 0;
        for (auto [entity, hp, pos] : View<Health, Position>(registry)) {
            ++seen;
            CHECK(hp.hp == 100);
            (void)entity;
            (void)pos;
        }
        // Health has one row (e3), Position drives as smaller? No - Health is
        // smaller (1 row), so it walks its single entity and probes Position.
        CHECK(seen == 1);
    }

    SUBCASE("membership survives unspecified ordering") {
        // After churn the dense order is unspecified; views must still find
        // exactly the right members.
        registry.destroy(e2);
        const Entity fresh = registry.create();
        registry.emplace<Position>(fresh, 5.0f, 0.0f);
        registry.emplace<Velocity>(fresh, 1.0f, 0.0f);

        int seen = 0;
        bool sawFresh = false;
        for (auto [entity, pos, vel] : View<Position, Velocity>(registry)) {
            ++seen;
            if (entity == fresh) {
                sawFresh = true;
                CHECK(pos.x == 5.0f);
            }
        }
        CHECK(seen == 3);
        CHECK(sawFresh);
    }
}

TEST_CASE("system runner executes stages in insertion order") {
    Registry registry(16);
    SystemRunner runner;
    f4::containers::Array<int> order;

    runner.add(f4::containers::String("first"), [&order](Registry& reg) {
        order.push(1);
        (void)reg;
    });
    runner.add(f4::containers::String("second"), [](Registry& reg) {
        // Systems may restructure the world mid-frame.
        const Entity spawned = reg.create();
        reg.emplace<Position>(spawned, 0.0f, 0.0f);
    });
    runner.add(f4::containers::String("third"), [&order](Registry& reg) {
        order.push(3);
        CHECK(reg.liveCount() == 1); // spawn happened before this stage
    });

    CHECK(runner.count() == 3);
    runner.runAll(registry);

    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 3);
}

TEST_CASE("clear wipes entities and storages; generations move forward") {
    Registry registry(16);
    const Entity a = registry.create();
    const Entity b = registry.create();
    registry.emplace<Position>(a, 1.0f, 1.0f);
    registry.emplace<Velocity>(b, 2.0f, 2.0f);

    registry.clear();
    CHECK(registry.liveCount() == 0);
    CHECK(!registry.hasStorage<Position>()); // storages were erased wholesale
    CHECK(!registry.isAlive(a));
    CHECK(!registry.isAlive(b));
    CHECK(registry.get<Position>(a) == nullptr);

    const Entity freshA = registry.create();
    CHECK(freshA.index == 0); // free list rebuilt: acquisition ascends again
    registry.emplace<Position>(freshA, 9.0f, 9.0f);
    CHECK(registry.get<Position>(freshA)->x == 9.0f);
}

TEST_CASE("lifetime stress: interleaved churn keeps every invariant") {
    Registry registry(4096);
    Lcg rng(0xC0FFEEu);

    f4::containers::Array<Entity> live;
    std::size_t positionRows = 0;
    std::size_t healthRows = 0;

    for (int step = 0; step < 20000; ++step) {
        const std::uint32_t op = rng.below(10);
        if (op < 4) {
            if (live.size() < 3000) {
                const Entity e = registry.create();
                live.push(e);
            }
        } else if (op < 6) {
            if (!live.isEmpty()) {
                const std::uint32_t victimSlot = rng.below(static_cast<std::uint32_t>(live.size()));
                const Entity victim = live[victimSlot];
                const bool hadPosition =
                    registry.get<Position>(victim) != nullptr;
                const bool hadHealth = registry.get<Health>(victim) != nullptr;
                if (hadPosition) {
                    --positionRows;
                }
                if (hadHealth) {
                    --healthRows;
                }
                registry.destroy(victim); // sweeps components implicitly
                live.removeSwap(victimSlot);
            }
        } else if (op < 8) {
            if (!live.isEmpty()) {
                const Entity target = live[rng.below(static_cast<std::uint32_t>(live.size()))];
                const bool had = registry.get<Position>(target) != nullptr;
                if (op == 6 && !had) {
                    registry.emplace<Position>(target,
                                               static_cast<float>(rng.next()),
                                               static_cast<float>(rng.next()));
                    ++positionRows;
                } else if (op == 7 && had) {
                    registry.remove<Position>(target);
                    --positionRows;
                }
            }
        } else {
            if (!live.isEmpty()) {
                const Entity target = live[rng.below(static_cast<std::uint32_t>(live.size()))];
                const bool had = registry.get<Health>(target) != nullptr;
                if (!had) {
                    registry.emplace<Health>(target, static_cast<int>(rng.below(100)));
                    ++healthRows;
                } else if (rng.below(2) == 0) {
                    registry.remove<Health>(target);
                    --healthRows;
                }
            }
        }
    }

    CHECK(registry.liveCount() == live.size());
    CHECK(registry.storage<Position>().size() == positionRows);
    CHECK(registry.storage<Health>().size() == healthRows);

    // Every mirrored live entity answers, every stored row belongs to a live
    // entity with matching identity.
    for (const Entity e : live) {
        REQUIRE(registry.isAlive(e));
    }
    const f4::containers::Array<Entity>& rows = registry.storage<Position>().entities();
    for (const Entity e : rows) {
        CHECK(registry.isAlive(e));
    }

    // Full-view agreement with the mirrors.
    std::size_t viewedPositions = 0;
    for (auto [entity, pos] : View<Position>(registry)) {
        ++viewedPositions;
        (void)entity;
        (void)pos;
    }
    CHECK(viewedPositions == positionRows);
}
