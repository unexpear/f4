#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>

#include "core/diagnostics/cpu_profiler.h"
#include "core/diagnostics/diagnostics.h"
#include "core/diagnostics/metric_registry.h"

using namespace f4::diag;

TEST_CASE("metric registry declares, dedups, and stores typed values") {
    MetricRegistry reg;
    const std::uint32_t fps = reg.declare({"fps", MetricType::Double, "gpu", "fps"});
    const std::uint32_t draws = reg.declare({"draw_calls", MetricType::UInt, "gpu", "count"});
    const std::uint32_t allocs = reg.declare({"heap_allocs", MetricType::UInt, "memory", "count"});
    CHECK(reg.count() == 3);

    // Declaring the same name again returns the existing id (no duplicate).
    const std::uint32_t fpsAgain = reg.declare({"fps", MetricType::Double, "gpu", "fps"});
    CHECK(fpsAgain == fps);
    CHECK(reg.count() == 3);

    reg[fps].set(59.7);
    reg[draws].set(std::uint64_t(42));
    reg[allocs].set(std::uint64_t(123));
    reg.declare({"name", MetricType::String, "telemetry"});
    reg[3].set("player_1");
    CHECK(reg.count() == 4);

    CHECK(reg[fps].asDouble() == doctest::Approx(59.7));
    CHECK(reg[fps].type() == MetricType::Double);
    CHECK(reg[draws].asUInt() == std::uint64_t(42));
    CHECK(reg[allocs].asUInt() == std::uint64_t(123));
    CHECK(reg[3].asString() == "player_1");

    // Wrong-typed setters are ignored (value unchanged).
    reg[draws].set(7.0);
    CHECK(reg[draws].asUInt() == std::uint64_t(42));

    // Iteration visits every metric.
    std::size_t visited = 0;
    for (const auto& m : reg) {
        (void)m;
        ++visited;
    }
    CHECK(visited == reg.count());

    // find() locates by name.
    CHECK(reg.find("fps") != nullptr);
    CHECK(reg.find("nope") == nullptr);

    reg.clear();
    CHECK(reg.count() == 0);
}

TEST_CASE("metric registry typing is enforced by declaration") {
    MetricRegistry reg;
    reg.declare({"count", MetricType::Int});
    reg.declare({"bytes", MetricType::UInt});
    reg.declare({"ratio", MetricType::Double});
    CHECK(reg[0].asInt() == 0);
    CHECK(reg[1].asUInt() == 0);
    CHECK(reg[2].asDouble() == doctest::Approx(0.0));

    reg[0].set(std::int64_t(-5));
    reg[1].set(std::uint64_t(1) << 40);
    reg[2].set(0.25);
    CHECK(reg[0].asInt() == -5);
    CHECK(reg[1].asUInt() == (1ull << 40));
    CHECK(reg[2].asDouble() == doctest::Approx(0.25));
}

namespace {
void spinSome() {
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 1000000; ++i) {
        sink += i;
    }
    (void)sink;
}
} // namespace

TEST_CASE("cpu profiler records zone call counts and a frame histogram") {
    CpuProfiler profiler;
    profiler.beginFrame();
    {
        ScopedZone zp(profiler, "update");
        spinSome();
    }
    {
        ScopedZone zp(profiler, "render");
        spinSome();
    }
    {
        ScopedZone outer(profiler, "outer");
        {
            ScopedZone inner(profiler, "inner");
            spinSome();
        }
    }
    profiler.endFrame();

    const auto& zones = profiler.zones();
    auto findByName = [&](const char* name) -> const ZoneSample* {
        for (const ZoneSample& z : zones) {
            if (std::string_view(z.name) == name) {
                return &z;
            }
        }
        return nullptr;
    };

    REQUIRE(zones.size() == 4);
    const ZoneSample* update = findByName("update");
    const ZoneSample* render = findByName("render");
    const ZoneSample* outer = findByName("outer");
    const ZoneSample* inner = findByName("inner");
    REQUIRE(update != nullptr);
    REQUIRE(render != nullptr);
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    CHECK(update->callCount == 1);
    CHECK(update->totalNanos > 0);
    CHECK(update->maxNanos <= update->totalNanos);
    CHECK(render->callCount == 1);
    CHECK(outer->callCount == 1);
    CHECK(inner->callCount == 1);

    const FrameHistogram& hist = profiler.histogram();
    CHECK(hist.count == 1);
    CHECK(hist.maxMs >= hist.minMs);
}

TEST_CASE("diagnostics samples on an interval and routes to sinks") {
    Diagnostics diag;
    diag.setConsoleEnabled(true);
    diag.setHudEnabled(true);
    diag.setSamplingInterval(0.0); // sample every update

    const std::uint32_t id =
        diag.metrics().declare({"frame_index", MetricType::UInt, "gpu", "count"});
    diag.metrics()[id].set(std::uint64_t(7));

    struct HudCount {
        int calls = 0;
    } hud;
    auto hudSink = [](const MetricRegistry&, void* userData) {
        static_cast<HudCount*>(userData)->calls++;
    };
    diag.setSampleSink(hudSink, &hud);

    diag.update(0.0);
    diag.update(0.5);   // another sample period
    CHECK(hud.calls == 2);

    diag.setHudEnabled(false);
    diag.update(1.0);
    CHECK(hud.calls == 2); // HUD disabled: sink not notified

    diag.setConsoleEnabled(false);
    diag.update(1.5); // no crash, no output

    // Sampling interval gates output frequency.
    Diagnostics gated;
    gated.setSamplingInterval(1.0);
    HudCount gatedHud{};
    gated.setSampleSink(hudSink, &gatedHud);
    gated.update(0.0);
    gated.update(0.4); // not yet due
    gated.update(1.1); // due
    CHECK(gatedHud.calls == 2);
}
