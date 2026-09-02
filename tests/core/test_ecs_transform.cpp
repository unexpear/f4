#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/ecs/view.h"
#include "approx.hpp"

using namespace f4;
using ecs::Entity;
using ecs::LocalTransform;
using ecs::Parent;
using ecs::Registry;
using ecs::TransformDirty;
using ecs::WorldMatrix;
using math::Mat4;
using math::Quat;
using math::Vec3;

namespace {
using DoubleMatrix = std::array<double, 16>;

auto countDirty(const Registry& registry) -> int {
    if (!registry.hasStorage<TransformDirty>()) {
        return 0;
    }
    return static_cast<int>(registry.storage<TransformDirty>().size());
}

auto toDoubleMatrix(const Mat4& matrix) -> DoubleMatrix {
    DoubleMatrix result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result[static_cast<std::size_t>(row * 4 + column)] = matrix(row, column);
        }
    }
    return result;
}

auto multiply(const DoubleMatrix& a, const DoubleMatrix& b) -> DoubleMatrix {
    DoubleMatrix result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double value = 0.0;
            for (int k = 0; k < 4; ++k) {
                value += a[static_cast<std::size_t>(row * 4 + k)] *
                         b[static_cast<std::size_t>(k * 4 + column)];
            }
            result[static_cast<std::size_t>(row * 4 + column)] = value;
        }
    }
    return result;
}
} // namespace

TEST_CASE("unparented entity gets identity world") {
    Registry registry(16);
    const Entity e = registry.create();
    registry.emplace<LocalTransform>(e);

    ecs::updateWorldTransforms(registry);

    const WorldMatrix* wm = registry.get<WorldMatrix>(e);
    REQUIRE(wm != nullptr);
    CHECK(f4test::meq(wm->matrix, Mat4::identity()));
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("two-level chain: dirty inheritance from parent-only tag") {
    Registry registry(16);
    const Entity parent = registry.create();
    const Entity child = registry.create();
    registry.emplace<LocalTransform>(parent, Vec3(1.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(child, Vec3(0.0f, 2.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    REQUIRE(ecs::trySetParent(registry, child, parent));

    SUBCASE("initial pass computes both") {
        ecs::updateWorldTransforms(registry);
        CHECK(f4test::veq(registry.get<WorldMatrix>(child)->matrix.transformPoint(Vec3()),
                          Vec3(1.0f, 2.0f, 0.0f)));
        CHECK(countDirty(registry) == 0);
    }

    SUBCASE("moving only the parent updates the child") {
        ecs::updateWorldTransforms(registry); // establish baseline
        ecs::setLocalPosition(registry, parent, Vec3(5.0f, 0.0f, 0.0f));

        CHECK(countDirty(registry) == 1); // child was never tagged

        ecs::updateWorldTransforms(registry);

        CHECK(f4test::veq(registry.get<WorldMatrix>(child)->matrix.transformPoint(Vec3()),
                          Vec3(5.0f, 2.0f, 0.0f)));
        // The state machine must have consumed every tag, including the
        // inherited-dirty path.
        CHECK(ecs::transformsAreClean(registry));
    }
}

TEST_CASE("three-level chain matches manual composition") {
    Registry registry(16);
    const Entity gp = registry.create();
    const Entity p = registry.create();
    const Entity c = registry.create();

    const Quat yaw90 = math::toQuat(Mat4::rotationY(1.57079637f));
    registry.emplace<LocalTransform>(gp, Vec3(10.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(p, Vec3(0.0f, 1.0f, 0.0f), yaw90,
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(c, Vec3(2.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(2.0f, 2.0f, 2.0f));
    REQUIRE(ecs::trySetParent(registry, p, gp));
    REQUIRE(ecs::trySetParent(registry, c, p));

    ecs::updateWorldTransforms(registry);

    // Brute-force expectation from the math layer directly.
    const Mat4 expected =
        math::composeHierarchy(math::composeHierarchy(math::composeTRS(*registry.get<LocalTransform>(gp)),
                                                      math::composeTRS(*registry.get<LocalTransform>(p))),
                               math::composeTRS(*registry.get<LocalTransform>(c)));

    const WorldMatrix* wm = registry.get<WorldMatrix>(c);
    REQUIRE(wm != nullptr);
    F4_CHECK_NEAR(wm->matrix, expected);

    // The child carries the middle node's yaw AND its own x2 scale: this
    // engine's +90 deg yaw about Y sends local +X to world -Z (column basis
    // (c, 0, -s)), and the uniform scale doubles its length.
    CHECK(f4test::veq(wm->matrix.transformVector(Vec3(1.0f, 0.0f, 0.0f)),
                      Vec3(0.0f, 0.0f, -2.0f)));
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("deep hierarchy drift stays bounded against double accumulation") {
    constexpr int depth = 512;
    Registry registry(depth + 8);
    const LocalTransform local{
        Vec3(0.125f, -0.0625f, 0.03125f),
        Quat::fromAxisAngle(math::normalized(Vec3(1.0f, 2.0f, -0.5f)), 0.013f),
        Vec3(1.0001f, 0.9999f, 1.00005f)};
    Entity parent{};
    Entity deepest{};
    for (int i = 0; i < depth; ++i) {
        const Entity entity = registry.create();
        registry.emplace<LocalTransform>(entity, local);
        if (i != 0) REQUIRE(ecs::trySetParent(registry, entity, parent));
        parent = entity;
        deepest = entity;
    }

    ecs::updateWorldTransforms(registry);
    const WorldMatrix* actual = registry.get<WorldMatrix>(deepest);
    REQUIRE(actual != nullptr);

    DoubleMatrix expected = toDoubleMatrix(Mat4::identity());
    const DoubleMatrix localDouble = toDoubleMatrix(math::composeTRS(local));
    for (int i = 0; i < depth; ++i) expected = multiply(expected, localDouble);

    double maxAbsoluteError = 0.0;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const double observed = actual->matrix(row, column);
            REQUIRE(std::isfinite(observed));
            maxAbsoluteError = std::max(
                maxAbsoluteError,
                std::fabs(observed - expected[static_cast<std::size_t>(row * 4 + column)]));
        }
    }
    INFO("512-level max absolute drift: " << maxAbsoluteError);
    // GCC and MSVC both measured 1.528e-5; pin a four-times regression ceiling.
    CHECK(maxAbsoluteError < 6.2e-5);
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("cycle rejection has no side effects") {
    Registry registry(16);
    const Entity a = registry.create();
    const Entity b = registry.create();
    const Entity c = registry.create();
    for (const Entity e : {a, b, c}) {
        registry.emplace<LocalTransform>(e);
    }

    CHECK(ecs::trySetParent(registry, b, a));
    CHECK(ecs::trySetParent(registry, c, b));

    SUBCASE("self parent rejected") {
        CHECK(!ecs::trySetParent(registry, a, a));
    }

    SUBCASE("deep cycle rejected") {
        CHECK(!ecs::trySetParent(registry, a, c)); // a->b->c; putting c under a closes it
    }

    // Rejections add no tags - but the two ACCEPTED reparents above each
    // left one.
    CHECK(countDirty(registry) == 2);
    ecs::updateWorldTransforms(registry);
    CHECK(f4test::meq(registry.get<WorldMatrix>(a)->matrix, Mat4::identity()));
    CHECK(f4test::veq(registry.get<WorldMatrix>(c)->matrix.transformPoint(Vec3()), Vec3()));
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("orphaned children become roots when their parent dies") {
    Registry registry(16);
    const Entity parent = registry.create();
    const Entity child = registry.create();
    registry.emplace<LocalTransform>(parent, Vec3(9.0f, 9.0f, 9.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(child, Vec3(1.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    REQUIRE(ecs::trySetParent(registry, child, parent));
    ecs::updateWorldTransforms(registry);
    CHECK(f4test::veq(registry.get<WorldMatrix>(child)->matrix.transformPoint(Vec3()),
                      Vec3(10.0f, 9.0f, 9.0f)));

    registry.destroy(parent); // child's Parent handle now dangles

    ecs::updateWorldTransforms(registry); // must not crash; child is a root now
    CHECK(f4test::veq(registry.get<WorldMatrix>(child)->matrix.transformPoint(Vec3()),
                      Vec3(1.0f, 0.0f, 0.0f)));
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("cross-subtree reparent world-jumps to new ancestry") {
    Registry registry(16);
    const Entity rootA = registry.create();
    const Entity nodeA = registry.create();
    const Entity rootB = registry.create();

    registry.emplace<LocalTransform>(rootA, Vec3(100.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(nodeA, Vec3(1.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(rootB, Vec3(0.0f, -50.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    REQUIRE(ecs::trySetParent(registry, nodeA, rootA));
    ecs::updateWorldTransforms(registry);
    CHECK(f4test::veq(registry.get<WorldMatrix>(nodeA)->matrix.transformPoint(Vec3()),
                      Vec3(101.0f, 0.0f, 0.0f)));

    REQUIRE(ecs::trySetParent(registry, nodeA, rootB)); // world-jump by contract
    ecs::updateWorldTransforms(registry);
    CHECK(f4test::veq(registry.get<WorldMatrix>(nodeA)->matrix.transformPoint(Vec3()),
                      Vec3(1.0f, -50.0f, 0.0f)));
    CHECK(ecs::transformsAreClean(registry));
}

TEST_CASE("clearParent detaches to root") {
    Registry registry(16);
    const Entity parent = registry.create();
    const Entity child = registry.create();
    registry.emplace<LocalTransform>(parent, Vec3(7.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    registry.emplace<LocalTransform>(child, Vec3(1.0f, 0.0f, 0.0f), Quat::identity(),
                                     Vec3(1.0f, 1.0f, 1.0f));
    REQUIRE(ecs::trySetParent(registry, child, parent));

    ecs::clearParent(registry, child);
    ecs::updateWorldTransforms(registry);
    CHECK(f4test::veq(registry.get<WorldMatrix>(child)->matrix.transformPoint(Vec3()),
                      Vec3(1.0f, 0.0f, 0.0f)));
    CHECK(ecs::transformsAreClean(registry));
}

namespace {
// Mirror model for the stress test: plain recursion over parent links,
// fully independent of the pass under test.
struct MirrorNode {
    std::optional<std::uint64_t> parentKey;
    LocalTransform local;
};

auto keyOf(Entity e) -> std::uint64_t {
    return (static_cast<std::uint64_t>(e.index) << 32) | e.generation;
}

auto mirrorWorld(const std::unordered_map<std::uint64_t, MirrorNode>& nodes,
                 std::uint64_t key) -> Mat4 {
    const MirrorNode& node = nodes.at(key);
    if (!node.parentKey.has_value()) {
        return math::composeTRS(node.local);
    }
    return mirrorWorld(nodes, *node.parentKey).mul(math::composeTRS(node.local));
}
} // namespace

TEST_CASE("stress: hierarchy churn matches brute-force mirror") {
    Registry registry(512);
    std::unordered_map<std::uint64_t, MirrorNode> mirror;
    std::vector<Entity> live;
    std::uint32_t rngState = 0xBEEF1234u;
    auto nextRandom = [&rngState]() {
        rngState = rngState * 1664525u + 1013904223u;
        return rngState;
    };
    auto pickLive = [&]() -> std::optional<Entity> {
        if (live.empty()) {
            return std::nullopt;
        }
        return live[nextRandom() % live.size()];
    };

    for (int step = 0; step < 8000; ++step) {
        const std::uint32_t op = nextRandom() % 10;
        if (op < 3 && live.size() < 400) {
            const Entity e = registry.create();
            const LocalTransform local(
                Vec3(static_cast<float>(nextRandom() % 20) - 10.0f, 0.0f, 0.0f),
                Quat::identity(), Vec3(1.0f, 1.0f, 1.0f));
            registry.emplace<LocalTransform>(e, local.position, local.rotation, local.scale);
            mirror[keyOf(e)] = MirrorNode{std::nullopt, local};
            live.push_back(e);
        } else if (op == 3 && !live.empty()) {
            const std::optional<Entity> target = pickLive();
            const Vec3 pos(static_cast<float>(nextRandom() % 30) - 15.0f,
                           static_cast<float>(nextRandom() % 30) - 15.0f, 0.0f);
            ecs::setLocalPosition(registry, *target, pos);
            mirror[keyOf(*target)].local.position = pos;
        } else if (op < 6 && !live.empty()) {
            // Reparent attempt - may be rejected as a cycle; mirror agrees.
            const std::optional<Entity> child = pickLive();
            const std::optional<Entity> parent = pickLive();
            if (ecs::trySetParent(registry, *child, *parent)) {
                mirror[keyOf(*child)].parentKey = keyOf(*parent);
            }
        } else if (op == 6 && !live.empty()) {
            const std::optional<Entity> target = pickLive();
            ecs::clearParent(registry, *target); // no-op when already root
            mirror[keyOf(*target)].parentKey.reset();
        } else if (op == 7 && !live.empty()) {
            // Destroy only an entity nobody parents to (keeps mirror valid).
            const std::optional<Entity> target = pickLive();
            bool referenced = false;
            for (const auto& [key, node] : mirror) {
                if (node.parentKey.has_value() && *node.parentKey == keyOf(*target)) {
                    referenced = true;
                    break;
                }
            }
            if (!referenced) {
                const auto it = std::find(live.begin(), live.end(), *target);
                live.erase(it);
                mirror.erase(keyOf(*target));
                registry.destroy(*target); // sweeps its components implicitly
            }
        } else if (step % 97 == 0 || step > 7900) {
            // Periodic verification against brute force.
            ecs::updateWorldTransforms(registry);
            for (const Entity e : live) {
                const WorldMatrix* wm = registry.get<WorldMatrix>(e);
                REQUIRE(wm != nullptr);
                const Mat4 expected = mirrorWorld(mirror, keyOf(e));
                CHECK(f4test::meq(wm->matrix, expected, f4test::kLooseTolerance));
            }
            CHECK(ecs::transformsAreClean(registry));
        }
    }
}
