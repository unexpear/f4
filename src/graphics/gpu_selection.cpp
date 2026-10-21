#include "graphics/gpu_selection.h"

#include <algorithm>
#include <cstdio>

#include "core/logging/logger.h"

namespace f4::gfx {

namespace {

[[nodiscard]] auto classRank(GpuClass gpuClass) -> int {
    switch (gpuClass) {
    case GpuClass::Discrete: return 0;
    case GpuClass::Integrated: return 1;
    case GpuClass::Virtual: return 2;
    case GpuClass::Other: return 3;
    case GpuClass::Software: return 4;
    }
    return 5;
}

} // namespace

auto gpuClassName(GpuClass gpuClass) -> const char* {
    switch (gpuClass) {
    case GpuClass::Discrete: return "discrete";
    case GpuClass::Integrated: return "integrated";
    case GpuClass::Virtual: return "virtual";
    case GpuClass::Other: return "other";
    case GpuClass::Software: return "software";
    }
    return "unknown";
}

auto rankGpuCandidates(std::span<const GpuCandidate> candidates) -> std::vector<std::size_t> {
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const GpuCandidate& left = candidates[a];
        const GpuCandidate& right = candidates[b];
        if (classRank(left.gpuClass) != classRank(right.gpuClass)) {
            return classRank(left.gpuClass) < classRank(right.gpuClass);
        }
        if (left.dedicatedMemoryBytes != right.dedicatedMemoryBytes) {
            return left.dedicatedMemoryBytes > right.dedicatedMemoryBytes;
        }
        return left.apiIndex < right.apiIndex;
    });
    return order;
}

auto selectGpu(std::span<const GpuCandidate> candidates, const GpuPreference& preference)
    -> GpuSelection {
    GpuSelection selection;
    selection.ranking = rankGpuCandidates(candidates);
    if (selection.ranking.empty()) {
        selection.preferenceHonored = preference.mode == GpuPreferenceMode::Auto;
        return selection;
    }
    selection.chosen = selection.ranking.front();
    const auto pickClass = [&](GpuClass wanted) {
        for (const std::size_t index : selection.ranking) {
            if (candidates[index].gpuClass == wanted) {
                selection.chosen = index;
                return;
            }
        }
        selection.preferenceHonored = false;
    };
    switch (preference.mode) {
    case GpuPreferenceMode::Auto:
        selection.preferenceHonored = preference.recognized;
        break;
    case GpuPreferenceMode::Discrete: pickClass(GpuClass::Discrete); break;
    case GpuPreferenceMode::Integrated: pickClass(GpuClass::Integrated); break;
    case GpuPreferenceMode::Index: {
        const auto found = std::find_if(
            selection.ranking.begin(), selection.ranking.end(),
            [&](std::size_t index) { return candidates[index].apiIndex == preference.index; });
        if (found != selection.ranking.end()) selection.chosen = *found;
        else selection.preferenceHonored = false;
        break;
    }
    }
    return selection;
}

void logGpuSelection(const char* api, std::span<const GpuCandidate> candidates,
                     const GpuSelection& selection, const GpuPreference& preference) {
    F4_LOG_INFO() << api << " adapters (F4_GPU=" << gpuPreferenceName(preference)
                  << "), strongest first:";
    std::size_t rank = 1;
    for (const std::size_t index : selection.ranking) {
        const GpuCandidate& candidate = candidates[index];
        F4_LOG_INFO() << "  #" << rank++ << ' ' << candidate.name << " ["
                      << gpuClassName(candidate.gpuClass) << ", "
                      << candidate.dedicatedMemoryBytes / (1024u * 1024u) << " MiB, index "
                      << candidate.apiIndex << ']'
                      << (index == selection.chosen ? " <- selected" : "");
    }
    if (!selection.preferenceHonored) {
        F4_LOG_WARNING() << api << ": F4_GPU=" << gpuPreferenceName(preference)
                         << " matched no adapter; using the automatic pick";
    }
}

} // namespace f4::gfx
