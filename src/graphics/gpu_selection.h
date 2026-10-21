#pragma once

// Adapter selection shared by every backend that can enumerate GPUs.
//
// Default ("auto"): pick the strongest GPU. Discrete beats integrated beats
// virtual/other; software rasterizers (WARP / Microsoft Basic Render Driver,
// llvmpipe, SwiftShader) are only used when nothing else exists. Ties are
// broken by dedicated video memory, then by the API's enumeration order.
//
// Override with the F4_GPU environment variable:
//   F4_GPU=auto                         default ranking
//   F4_GPU=discrete | high-performance  best discrete GPU
//   F4_GPU=integrated | low-power       best integrated GPU
//   F4_GPU=<n>                          adapter n in the backend's own
//                                       enumeration order (logged at startup)
// OpenGL cannot enumerate adapters; it honours discrete/integrated through the
// NVIDIA Optimus / AMD PowerXpress executable exports (see
// platform/high_performance_gpu.cpp). Index overrides do not apply to OpenGL.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace f4::gfx {

enum class GpuClass : std::uint8_t { Discrete, Integrated, Virtual, Other, Software };

struct GpuCandidate {
    std::string name;
    GpuClass gpuClass = GpuClass::Other;
    std::uint64_t dedicatedMemoryBytes = 0;
    std::uint32_t apiIndex = 0; // position in the backend API's enumeration
};

enum class GpuPreferenceMode : std::uint8_t { Auto, Discrete, Integrated, Index };

struct GpuPreference {
    GpuPreferenceMode mode = GpuPreferenceMode::Auto;
    std::uint32_t index = 0;
    bool recognized = true; // false: unparseable text, treated as Auto
};

// Header-only so the platform layer can read the same setting without linking
// the graphics library.
[[nodiscard]] inline auto parseGpuPreference(const char* text) -> GpuPreference {
    GpuPreference preference;
    if (text == nullptr) return preference;
    std::string value;
    for (const char* c = text; *c != '\0'; ++c) {
        if (std::isspace(static_cast<unsigned char>(*c)) == 0) {
            value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*c))));
        }
    }
    if (value.empty() || value == "auto" || value == "best" || value == "default") {
        return preference;
    }
    if (value == "discrete" || value == "dgpu" || value == "high-performance" ||
        value == "highperformance" || value == "high_performance") {
        preference.mode = GpuPreferenceMode::Discrete;
        return preference;
    }
    if (value == "integrated" || value == "igpu" || value == "low-power" ||
        value == "lowpower" || value == "low_power") {
        preference.mode = GpuPreferenceMode::Integrated;
        return preference;
    }
    bool digits = value.size() <= 9;
    for (const char c : value) digits = digits && std::isdigit(static_cast<unsigned char>(c)) != 0;
    if (digits) {
        preference.mode = GpuPreferenceMode::Index;
        preference.index = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        return preference;
    }
    preference.recognized = false;
    return preference;
}

[[nodiscard]] inline auto gpuPreferenceFromEnvironment() -> GpuPreference {
    return parseGpuPreference(std::getenv("F4_GPU"));
}

[[nodiscard]] auto gpuClassName(GpuClass gpuClass) -> const char*;
[[nodiscard]] inline auto gpuPreferenceName(const GpuPreference& preference) -> std::string {
    switch (preference.mode) {
    case GpuPreferenceMode::Auto: return preference.recognized ? "auto" : "auto (unrecognized)";
    case GpuPreferenceMode::Discrete: return "discrete";
    case GpuPreferenceMode::Integrated: return "integrated";
    case GpuPreferenceMode::Index: return std::to_string(preference.index);
    }
    return "auto";
}

// Indices into `candidates`, strongest first (the auto order).
[[nodiscard]] auto rankGpuCandidates(std::span<const GpuCandidate> candidates)
    -> std::vector<std::size_t>;

struct GpuSelection {
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::size_t chosen = kNone;      // index into candidates
    std::vector<std::size_t> ranking; // auto order, strongest first
    bool preferenceHonored = true;   // false: override matched nothing, used auto
};

// Applies `preference` on top of the auto ranking. Discrete/Integrated pick the
// highest-ranked adapter of that class; Index picks the adapter whose apiIndex
// matches. When the override matches nothing the auto pick is used.
[[nodiscard]] auto selectGpu(std::span<const GpuCandidate> candidates,
                             const GpuPreference& preference) -> GpuSelection;

// Logs the ranking and the pick, e.g.
//   DX12 adapters (F4_GPU=auto), strongest first:
//     #1 NVIDIA GeForce RTX 3070 Ti Laptop GPU [discrete, 8018 MiB, index 1] <- selected
//     #2 Intel(R) Iris(R) Xe Graphics [integrated, 128 MiB, index 0]
void logGpuSelection(const char* api, std::span<const GpuCandidate> candidates,
                     const GpuSelection& selection, const GpuPreference& preference);

} // namespace f4::gfx
