#include "engine/rendering/render_queue.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f4::engine {

namespace {

template <typename Tag>
[[nodiscard]] auto compareHandle(ids::Handle<Tag> a, ids::Handle<Tag> b) -> int {
    if (a.index != b.index) return a.index < b.index ? -1 : 1;
    if (a.generation != b.generation) return a.generation < b.generation ? -1 : 1;
    return 0;
}

[[nodiscard]] auto compareBatchState(const RenderItem& a, const RenderItem& b) -> int {
    if (a.mirrored != b.mirrored) return a.mirrored ? 1 : -1;
    if (a.pipelineSortKey != b.pipelineSortKey) {
        return a.pipelineSortKey < b.pipelineSortKey ? -1 : 1;
    }
    if (const int material = compareHandle(a.material, b.material); material != 0) {
        return material;
    }
    if (const int mesh = compareHandle(a.meshRenderer.mesh, b.meshRenderer.mesh); mesh != 0) {
        return mesh;
    }
    if (a.meshRenderer.subMeshIndex != b.meshRenderer.subMeshIndex) {
        return a.meshRenderer.subMeshIndex < b.meshRenderer.subMeshIndex ? -1 : 1;
    }
    return 0;
}

[[nodiscard]] auto compareEntity(ecs::Entity a, ecs::Entity b) -> int {
    return compareHandle(a, b);
}

[[nodiscard]] auto canInstanceTogether(const RenderItem& a, const RenderItem& b) -> bool {
    return !a.transparent && !b.transparent && compareBatchState(a, b) == 0;
}

[[nodiscard]] auto orderedDepth(float depth) -> float {
    return std::isfinite(depth) ? depth : std::numeric_limits<float>::max();
}

} // namespace

void RenderQueue::sort() {
    // Partition: stable sort with custom comparator so opaque group stays in
    // pipeline/depth order while transparent group is reversed depth.
    // We partition into two subranges, sort each, then merge by splicing.
    if (items_.isEmpty()) {
        return;
    }
    // First, partition opaque before transparent (stable).
    std::stable_sort(items_.data(), items_.data() + items_.size(),
                     [](const RenderItem& a, const RenderItem& b) {
                         if (a.transparent != b.transparent) {
                             return !a.transparent && b.transparent;
                         }
                         return false;
                     });

    // Find split point
    std::size_t split = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].transparent) {
            split = i;
            break;
        }
        if (i + 1 == items_.size()) {
            split = items_.size();
        }
    }

    // Opaque: complete batch key asc, then front-to-back. Entity identity is
    // the final tie-break so equal-depth input never depends on storage order.
    std::sort(items_.data(), items_.data() + split,
              [](const RenderItem& a, const RenderItem& b) {
                  if (const int state = compareBatchState(a, b); state != 0) {
                      return state < 0;
                  }
                  const float aDepth = orderedDepth(a.depth);
                  const float bDepth = orderedDepth(b.depth);
                  if (aDepth != bDepth) return aDepth < bDepth;
                  return compareEntity(a.entity, b.entity) < 0;
              });

    // Transparent: exact depth order first. An epsilon comparator is not a
    // strict weak ordering and can make std::sort undefined for chained near-
    // equal values, so ties use state and entity identity instead.
    if (split < items_.size()) {
        std::sort(items_.data() + split, items_.data() + items_.size(),
                  [](const RenderItem& a, const RenderItem& b) {
                      const float aDepth = orderedDepth(a.depth);
                      const float bDepth = orderedDepth(b.depth);
                      if (aDepth != bDepth) return aDepth > bDepth;
                      if (const int state = compareBatchState(a, b); state != 0) {
                          return state < 0;
                      }
                      return compareEntity(a.entity, b.entity) < 0;
                  });
    }
}

void RenderQueue::buildBatches(Array<RenderBatch>& output, bool batchingEnabled) const {
    output.clear();
    std::size_t first = 0;
    while (first < items_.size()) {
        std::size_t count = 1;
        if (batchingEnabled) {
            while (first + count < items_.size() &&
                   count < std::numeric_limits<std::uint32_t>::max() &&
                   canInstanceTogether(items_[first], items_[first + count])) {
                ++count;
            }
        }
        output.push({static_cast<std::uint32_t>(first),
                     static_cast<std::uint32_t>(count)});
        first += count;
    }
}

} // namespace f4::engine
