#include "doctest/doctest.h"

#include <vector>

#include "graphics/gpu_selection.h"

using namespace f4::gfx;

namespace {

constexpr std::uint64_t kMiB = 1024u * 1024u;

[[nodiscard]] auto laptopAdapters() -> std::vector<GpuCandidate> {
    // DXGI order on a typical Optimus laptop: iGPU first, software last.
    return {
        {"Intel(R) Iris(R) Xe Graphics", GpuClass::Integrated, 128 * kMiB, 0},
        {"NVIDIA GeForce RTX 3070 Ti Laptop GPU", GpuClass::Discrete, 8018 * kMiB, 1},
        {"Microsoft Basic Render Driver", GpuClass::Software, 0, 2},
    };
}

} // namespace

TEST_CASE("GPU selection: F4_GPU parsing") {
    CHECK(parseGpuPreference(nullptr).mode == GpuPreferenceMode::Auto);
    CHECK(parseGpuPreference("").mode == GpuPreferenceMode::Auto);
    CHECK(parseGpuPreference("Auto").mode == GpuPreferenceMode::Auto);
    CHECK(parseGpuPreference("discrete").mode == GpuPreferenceMode::Discrete);
    CHECK(parseGpuPreference("High-Performance").mode == GpuPreferenceMode::Discrete);
    CHECK(parseGpuPreference(" integrated ").mode == GpuPreferenceMode::Integrated);
    CHECK(parseGpuPreference("low-power").mode == GpuPreferenceMode::Integrated);
    const GpuPreference index = parseGpuPreference("2");
    CHECK(index.mode == GpuPreferenceMode::Index);
    CHECK(index.index == 2u);
    const GpuPreference junk = parseGpuPreference("fastest-please");
    CHECK(junk.mode == GpuPreferenceMode::Auto);
    CHECK_FALSE(junk.recognized);
    CHECK_FALSE(parseGpuPreference("-1").recognized);
}

TEST_CASE("GPU selection: auto ranks discrete, integrated, then software by VRAM") {
    const auto adapters = laptopAdapters();
    const GpuSelection selection = selectGpu(adapters, GpuPreference{});
    REQUIRE(selection.ranking.size() == 3u);
    CHECK(selection.ranking[0] == 1u);
    CHECK(selection.ranking[1] == 0u);
    CHECK(selection.ranking[2] == 2u);
    CHECK(selection.chosen == 1u);
    CHECK(selection.preferenceHonored);

    // Among equals, more dedicated memory wins; then enumeration order.
    const std::vector<GpuCandidate> twoDiscrete = {
        {"small", GpuClass::Discrete, 4096 * kMiB, 0},
        {"big", GpuClass::Discrete, 16384 * kMiB, 1},
        {"big twin", GpuClass::Discrete, 16384 * kMiB, 2},
    };
    const auto ranking = rankGpuCandidates(twoDiscrete);
    CHECK(ranking == std::vector<std::size_t>{1u, 2u, 0u});

    // Software is only chosen when it is the only adapter.
    const std::vector<GpuCandidate> onlySoftware = {{"llvmpipe", GpuClass::Software, 0, 0}};
    CHECK(selectGpu(onlySoftware, GpuPreference{}).chosen == 0u);
    CHECK(selectGpu(std::vector<GpuCandidate>{}, GpuPreference{}).chosen ==
          GpuSelection::kNone);
}

TEST_CASE("GPU selection: overrides pick by class or API index, else fall back") {
    const auto adapters = laptopAdapters();
    CHECK(selectGpu(adapters, parseGpuPreference("integrated")).chosen == 0u);
    CHECK(selectGpu(adapters, parseGpuPreference("discrete")).chosen == 1u);
    CHECK(selectGpu(adapters, parseGpuPreference("2")).chosen == 2u);
    CHECK(selectGpu(adapters, parseGpuPreference("0")).chosen == 0u);

    const GpuSelection missing = selectGpu(adapters, parseGpuPreference("7"));
    CHECK(missing.chosen == 1u);
    CHECK_FALSE(missing.preferenceHonored);

    const std::vector<GpuCandidate> desktop = {{"dGPU", GpuClass::Discrete, 8192 * kMiB, 0}};
    const GpuSelection noIntegrated = selectGpu(desktop, parseGpuPreference("integrated"));
    CHECK(noIntegrated.chosen == 0u);
    CHECK_FALSE(noIntegrated.preferenceHonored);
}
