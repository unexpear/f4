#include "engine/rendering/render_queue.h"

#include <algorithm>
#include <cmath>

namespace f4::engine {

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

    // Opaque: pipeline key asc, depth asc
    std::sort(items_.data(), items_.data() + split,
              [](const RenderItem& a, const RenderItem& b) {
                  if (a.pipelineSortKey != b.pipelineSortKey) {
                      return a.pipelineSortKey < b.pipelineSortKey;
                  }
                  return a.depth < b.depth;
              });

    // Transparent: depth desc (far to near), then pipeline
    if (split < items_.size()) {
        std::sort(items_.data() + split, items_.data() + items_.size(),
                  [](const RenderItem& a, const RenderItem& b) {
                      if (std::abs(a.depth - b.depth) > 1e-5f) {
                          return a.depth > b.depth;
                      }
                      return a.pipelineSortKey < b.pipelineSortKey;
                  });
    }
}

} // namespace f4::engine
