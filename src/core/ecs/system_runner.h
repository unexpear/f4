#pragma once

#include <cstddef>
#include <functional>
#include <utility>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "core/ecs/registry.h"

namespace f4::ecs {

using containers::Array;
using containers::String;

// Ordered system execution: named {name, fn(Registry&)} stages run strictly
// in insertion order - deterministic by construction. No scheduling, no
// parallelism; M12 revisits if profiling demands it.
class SystemRunner {
public:
    using SystemFn = std::function<void(Registry&)>;

    void add(String name, SystemFn fn) {
        systems_.push(System{std::move(name), std::move(fn)});
    }

    void runAll(Registry& registry) const {
        for (const System& system : systems_) {
            system.fn(registry);
        }
    }

    [[nodiscard]] auto count() const -> std::size_t { return systems_.size(); }

    void clear() { systems_.clear(); }

private:
    struct System {
        String name;
        SystemFn fn;
    };

    Array<System> systems_;
};

} // namespace f4::ecs