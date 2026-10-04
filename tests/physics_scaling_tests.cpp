// Pins how ending a 3D body's contacts grows with the contacts of other bodies, through the bytes it
// allocates. This executable links allocation_counter.cpp, which replaces the global allocation functions,
// so it counts every allocation the engine makes during a measured operation. Jolt allocates through its
// own hooks, which call malloc, so its bookkeeping is not counted.
#include "allocation_counter.hpp"
#include <anima/physics.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace anima;
using namespace anima::physics;
namespace {
constexpr double tick = 1. / 60;       // One 60 Hz fixed step.
constexpr float box_half_extent = .5F; // Unit boxes resting on a floor whose top is at y = 0.
constexpr float spacing = 2;           // Grid spacing that keeps the boxes apart.

// Doubling checks end the contacts of this many bodies and of twice as many. Doubling them doubles
// the bytes of work linear in them but quadruples those of work that rebuilds every remaining contact
// for each body, so a check allows three times the bytes. The counts are smaller than scene_scaling's
// because the step that makes the contacts dominates the run, and grows faster than the bodies.
constexpr std::size_t doubled_bodies = 1024;
constexpr std::size_t doubling_limit = 3;

// Bytes allocated while @p operation runs.
template <class Operation> std::size_t allocated_by(Operation &&operation) {
    allocation_counter::bytes = 0;
    allocation_counter::counting = true;
    operation();
    allocation_counter::counting = false;
    return allocation_counter::bytes;
}

BodySettings box(Vec3 position, Vec3 half_extent, Motion motion) {
    BodySettings s;
    s.pose.position = position;
    s.collider.half_extent = half_extent;
    s.motion = motion;
    return s;
}

// Bytes allocated by ending the contacts of @p count dynamic boxes that each rest on one floor, by
// removing every box or, when @p disable is set, by disabling it.
std::size_t ending_bytes(std::size_t count, bool disable) {
    const auto side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const auto floor_half_extent = static_cast<float>(side) * spacing / 2 + spacing;
    World world({{0, -9.81F, 0}, static_cast<std::uint32_t>(count + 1)});
    (void)world.create(box({0, -.5F, 0}, {floor_half_extent, .5F, floor_half_extent}, Motion::stationary));
    std::vector<Body> boxes;
    boxes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = static_cast<float>(i % side) * spacing - floor_half_extent + spacing;
        const auto z = static_cast<float>(i / side) * spacing - floor_half_extent + spacing;
        boxes.push_back(world.create(
            box({x, box_half_extent, z}, {box_half_extent, box_half_extent, box_half_extent}, Motion::dynamic)));
    }
    world.step(tick);
    REQUIRE(world.take_events().size() == count);
    const auto bytes = allocated_by([&] {
        for (auto &body : boxes)
            if (disable)
                body.set_enabled(false);
            else
                body.remove();
    });
    REQUIRE(world.take_events().size() == count);
    return bytes;
}
} // namespace

TEST_CASE("Removing bodies allocates no more per body as their contacts grow") {
    const auto small = ending_bytes(doubled_bodies, false), large = ending_bytes(2 * doubled_bodies, false);
    CHECK(large <= doubling_limit * small);
}

TEST_CASE("Disabling bodies allocates no more per body as their contacts grow") {
    const auto small = ending_bytes(doubled_bodies, true), large = ending_bytes(2 * doubled_bodies, true);
    CHECK(large <= doubling_limit * small);
}
