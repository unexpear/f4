#include <doctest/doctest.h>

#include <cstdint>
#include <new>
#include <utility>

#include "core/memory/arena.h"
#include "core/memory/memory.h"
#include "core/memory/pool.h"

using namespace f4::memory;

TEST_CASE("alignUp and alignment helpers") {
    CHECK(alignUp(0, 4) == 0);
    CHECK(alignUp(1, 4) == 4);
    CHECK(alignUp(4, 4) == 4);
    CHECK(alignUp(5, 8) == 8);
    CHECK(alignUp(17, 16) == 32);

    CHECK(isPowerOfTwo(1));
    CHECK(isPowerOfTwo(64));
    CHECK(!isPowerOfTwo(0));
    CHECK(!isPowerOfTwo(96));

    CHECK(isAlignedTo(64, 16));
    CHECK(!isAlignedTo(66, 16));
}

TEST_CASE("heap seam returns aligned memory that frees cleanly") {
    void* p = heapAllocate(100, 64);
    REQUIRE(p != nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(p);
    CHECK(address % 64 == 0);
    heapFree(p, 64);

    void* q = heapAllocate(8);
    REQUIRE(q != nullptr);
    const auto qAddress = reinterpret_cast<std::uintptr_t>(q);
    CHECK(qAddress % kDefaultAlignment == 0);
    heapFree(q);
}

TEST_CASE("arena bumps, aligns, reports, and exhausts") {
    Arena arena(256);

    SUBCASE("fresh state") {
        CHECK(arena.capacityBytes() == 256);
        CHECK(arena.usedBytes() == 0);
        CHECK(arena.remainingBytes() == 256);
    }

    SUBCASE("allocations are aligned and packed") {
        auto* a = static_cast<std::byte*>(arena.allocate(10));
        REQUIRE(a != nullptr);
        auto* b = static_cast<std::byte*>(arena.allocate(1, 64));
        REQUIRE(b != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
        CHECK(arena.usedBytes() >= 11);
        CHECK(arena.usedBytes() <= 74); // worst-case padding bounded
    }

    SUBCASE("zero-size request is null") {
        CHECK(arena.allocate(0) == nullptr);
    }

    SUBCASE("exhaustion yields nullptr with exact accounting") {
        void* first = arena.allocate(200);
        REQUIRE(first != nullptr);
        // 56 bytes left; this must not fit after padding.
        CHECK(arena.allocate(60, 16) == nullptr);
        CHECK(arena.allocate(48) != nullptr);   // fits unaligned
        CHECK(arena.remainingBytes() < 16);
    }

    SUBCASE("reset rewinds without freeing the buffer") {
        REQUIRE(arena.allocate(100) != nullptr);
        arena.reset();
        CHECK(arena.usedBytes() == 0);
        REQUIRE(arena.allocate(256) != nullptr); // full capacity again
        CHECK(arena.allocate(1) == nullptr);
    }
}

namespace {
struct Tracked {
    int value;
    explicit Tracked(int v) : value(v) {}
};
struct alignas(64) OverAligned {
    double x = 1.0;
};
} // namespace

TEST_CASE("arena create placement-constructs with correct alignment") {
    Arena arena(512);
    auto* t = arena.create<Tracked>(42);
    REQUIRE(t != nullptr);
    CHECK(t->value == 42);
    CHECK(reinterpret_cast<std::uintptr_t>(t) % alignof(Tracked) == 0);

    auto* o = arena.create<OverAligned>();
    REQUIRE(o != nullptr);
    CHECK(o->x == 1.0);
    CHECK(reinterpret_cast<std::uintptr_t>(o) % 64 == 0);
}

TEST_CASE("arena moves own the buffer exactly once") {
    Arena source(128);
    REQUIRE(source.allocate(64) != nullptr);
    Arena target(std::move(source));
    CHECK(target.usedBytes() == 64);
    CHECK(source.capacityBytes() == 0);
    CHECK(source.allocate(1) == nullptr);

    Arena second = std::move(target);
    CHECK(second.usedBytes() == 64);
}

TEST_CASE("pool recycles chunks LIFO and accounts capacity") {
    Pool pool(24, 8, 4);
    CHECK(pool.capacityChunks() == 4);
    CHECK(pool.freeChunks() == 4);
    // stride rounded up to sizeof(void*) minimum
    CHECK(pool.strideBytes() >= 24);

    void* a = pool.allocate();
    void* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(pool.freeChunks() == 2);
    CHECK(a != b);

    pool.deallocate(a);
    pool.deallocate(b);
    CHECK(pool.freeChunks() == 4);
    // LIFO: last freed comes back first.
    void* c = pool.allocate();
    CHECK(c == b);
    void* d = pool.allocate();
    CHECK(d == a);

    // Drain completely.
    while (pool.allocate() != nullptr) {
    }
    CHECK(pool.allocate() == nullptr);
    CHECK(pool.freeChunks() == 0);
    CHECK(pool.contains(c));
}

TEST_CASE("pool contains rejects foreign pointers") {
    Pool pool(32, 16, 2);
    int local = 7;
    CHECK(!pool.contains(&local));
    CHECK(!pool.contains(nullptr));
    void* mid = pool.allocate(); // first chunk
    CHECK(pool.contains(mid));
    // Interior pointer inside chunk 1 is still rejected (not chunk-aligned).
    auto* interior = static_cast<std::byte*>(mid) + pool.strideBytes() / 2;
    CHECK(!pool.contains(interior));
}

namespace {
struct PooledWidget {
    int id;
    bool destroyed = false;
    explicit PooledWidget(int i) : id(i) {}
    ~PooledWidget() { destroyed = true; }
};
} // namespace

TEST_CASE("typed pool constructs and destroys") {
    TypedPool<PooledWidget> pool(3);

    auto* w1 = pool.acquire(1);
    auto* w2 = pool.acquire(2);
    auto* w3 = pool.acquire(3);
    REQUIRE(w1 != nullptr);
    CHECK(w3->id == 3);
    CHECK(pool.acquire(4) == nullptr); // exhausted
    CHECK(w2->id == 2);

    pool.release(w2);
    auto* w5 = pool.acquire(5);
    REQUIRE(w5 != nullptr);
    CHECK(w5 == w2); // recycled slot
    CHECK(w5->id == 5);
    CHECK(pool.contains(w1));
    CHECK(pool.contains(w5));
}

TEST_CASE("heap tracker reports per-frame allocation accounting") {
    HeapTracker& t = heapTracker();
    enableHeapTracking(true);
    t.beginFrame();

    void* a = heapAllocate(100);
    void* b = heapAllocate(64);
    void* c = heapAllocate(16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    heapFree(b);

    t.endFrame();
    HeapFrameStats s = t.frame();
    CHECK(s.allocations == 3);
    CHECK(s.frees == 1);
    CHECK(s.bytesAllocated == 100u + 64u + 16u);
    CHECK(s.bytesFreed == 64u);
    CHECK(s.liveGroups == 2); // a and c still live
    CHECK(s.peakLiveBytes >= 100u + 64u);

    CHECK(t.liveBytes() == 100u + 16u);
    CHECK(t.liveGroups() == 2);

    heapFree(a);
    heapFree(c);
    t.endFrame();
    s = t.frame();
    CHECK(s.allocations == 0);
    CHECK(s.frees == 2);
    CHECK(s.bytesFreed == 100u + 16u);
    CHECK(t.liveBytes() == 0);
    CHECK(t.liveGroups() == 0);

    enableHeapTracking(false);
}
