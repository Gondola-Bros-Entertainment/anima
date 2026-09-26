#include <anima/physics2d.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace anima::physics2d;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "Expected explicit rejection");
}
BodySettings box(Vec2 position, Vec2 extent, Motion motion = Motion::stationary) {
    BodySettings s;
    s.pose.position = position;
    s.collider.half_extent = extent;
    s.motion = motion;
    return s;
}
void run() {
    Body stale;
    {
        World world;
        auto floor = world.create(box({0, -.5F}, {10, .5F}));
        auto s = box({0, 4}, {.5F, .5F}, Motion::dynamic);
        s.collider.shape = Shape::circle;
        auto ball = world.create(s);
        stale = ball;
        for (int i = 0; i < 240; ++i)
            world.step(1. / 60);
        check(std::abs(ball.pose().position.y - .5F) < .03F, "Ball did not land on floor");
        check(!ball.awake(), "Resting body failed to sleep");
        auto events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::begin && !events[0].sensor,
              "Landing/sleep should retain one contact begin");
        for (int i = 0; i < 30; ++i)
            world.step(1. / 60);
        check(world.take_events().empty(), "Sleeping contact flickered");
        ball.add_impulse({0, 3});
        for (int i = 0; i < 10; ++i)
            world.step(1. / 60);
        events = world.take_events();
        check(ball.awake() && events.size() == 1 && events[0].phase == ContactPhase::end,
              "Impulse did not wake/leave contact");
        World foreign;
        auto other = foreign.create({});
        check(!world.owns(other) && !(other == ball), "Foreign body identity alias");
        QueryFilter filter;
        filter.ignore = other;
        rejects([&] { (void)world.raycast({0, 10}, {0, -20}, filter); });
        rejects([&] { world.set_layer_collision(0, 1, false); });
        floor.remove();
        ball.remove();
        ball.remove();
        check(!stale.valid(), "Removed identity remains valid");
        const auto replacement = world.create({});
        check(!(replacement == stale) && !stale.valid(), "Backend slot reuse resurrected body");
        rejects([&] { (void)stale.pose(); });
        stale = replacement;
    }
    check(!stale.valid(), "Destroyed world retained body");
    stale.remove();
    {
        World world({{0, 0}, 16});
        world.set_layer_collision(0, 0, false); // Query filtering must not depend on simulation mask.
        auto ground = world.create(box({0, -.5F}, {4, .5F}));
        auto bridge = world.create(box({0, 3}, {2, .2F}));
        auto ray = world.raycast({0, 6}, {0, -10});
        check(ray && ray->body == bridge && std::abs(ray->fraction - .28F) < .001F && ray->normal.y > .99F,
              "Ray top surface/fraction failed");
        ray = world.raycast({0, 1}, {0, 4});
        check(ray && ray->body == bridge && ray->normal.y < -.99F, "Bridge underside missed");
        Collider query;
        query.shape = Shape::circle;
        query.radius = .5F;
        auto sweep = world.sweep(query, {{0, 100}}, {0, -200});
        check(sweep && sweep->body == bridge && std::abs(sweep->fraction - .4815F) < .001F,
              "Large displacement sweep tunneled through bridge");
        auto overlaps = world.overlap(query, {{0, .25F}});
        check(overlaps.size() == 1 && overlaps[0] == ground, "Clearance overlap failed");
        sweep = world.sweep(query, {{0, .25F}}, {0, 4});
        check(sweep && sweep->body == ground && sweep->initial_overlap && sweep->fraction == 0,
              "Initially penetrating sweep did not fail closed");
        ray = world.raycast({0, -.25F}, {0, 4});
        check(ray && ray->initial_overlap && ray->body == ground, "Initially penetrating ray missed");
        QueryFilter filter;
        filter.ignore = bridge;
        ray = world.raycast({0, 6}, {0, -10}, filter);
        check(ray && ray->body == ground, "Ignore-body filter failed");
        filter.layers = 2;
        check(!world.raycast({0, 6}, {0, -10}, filter), "Layer query filter failed");
        bridge.set_enabled(false);
        ray = world.raycast({0, 6}, {0, -10});
        check(ray && ray->body == ground, "Disabled body remains queryable");
        bridge.set_enabled(true);
        bridge.teleport({{0, 3}, 1.57079632679F});
        ray = world.raycast({-4, 3}, {8, 0});
        check(ray && ray->body == bridge && ray->normal.x < -.99F, "Rotated shape query failed");
        query.shape = Shape::capsule;
        sweep = world.sweep(query, {{3, 5}, .5F}, {0, -10});
        check(sweep && sweep->body == ground, "Rotated capsule sweep failed");
        query.shape = Shape::box;
        sweep = world.sweep(query, {{3, 5}, .5F}, {0, -10});
        check(sweep && sweep->body == ground, "Rotated box sweep failed");
        rejects([&] { (void)world.raycast({}, {}); });
        rejects([&] { (void)world.sweep(query, {}, {}); });
        rejects([&] { (void)world.overlap(query, {{}, std::numeric_limits<float>::quiet_NaN()}); });
    }
    {
        World world({{0, 0}, 8});
        world.set_layer_collision(0, 1, false);
        auto settings = box({}, {2, 2});
        settings.sensor = true;
        settings.layer = 2;
        auto sensor = world.create(settings);
        auto visitor = world.create(box({}, {.2F, .2F}, Motion::dynamic));
        auto ignored = box({}, {1, 1});
        ignored.layer = 1;
        auto blocker = world.create(ignored);
        world.step(1. / 60);
        auto events = world.take_events();
        // The stationary sensor also overlaps the stationary blocker, which it must not report.
        check(events.size() == 1, "Two stationary bodies reported a sensor overlap");
        unsigned starts = 0;
        for (auto e : events)
            if (e.first == visitor || e.second == visitor) {
                check(e.sensor && e.phase == ContactPhase::begin && (e.first == sensor || e.second == sensor),
                      "Layer response leaked");
                ++starts;
            }
        check(starts == 1, "Missing sensor enter");
        QueryFilter f;
        f.layers = 1u << 2;
        check(!world.raycast({0, 3}, {0, -6}, f), "Sensor query defaults incorrect");
        f.sensors = true;
        check(world.raycast({0, 3}, {0, -6}, f)->body == sensor, "Explicit sensor query missing");
        visitor.set_enabled(false);
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::end, "Disable did not close contact once");
        visitor.set_enabled(true);
        world.step(1. / 60);
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::begin, "Reenable did not reopen contact");
        visitor.remove();
        auto replacement = world.create(box({10, 0}, {.2F, .2F}, Motion::dynamic));
        world.step(1. / 60);
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::end &&
                  (events[0].first == visitor || events[0].second == visitor),
              "Removal or buffered stale event alias");
        replacement.remove();
        blocker.remove();
        sensor.remove();
        (void)world.take_events();
        auto kinematic = world.create(box({10, 0}, {.5F, .5F}, Motion::kinematic));
        kinematic.move_kinematic({{11, 0}, .1F}, .1);
        world.step(.1);
        check(std::abs(kinematic.pose().position.x - 11) < .01F && std::abs(kinematic.pose().angle - .1F) < .001F,
              "Kinematic target failed");
        const auto stopped = kinematic.pose();
        kinematic.move_kinematic(stopped, .1);
        world.step(.1);
        check(std::abs(kinematic.pose().position.x - stopped.position.x) < .001F && kinematic.velocity().x == 0,
              "Stationary kinematic target retained old velocity");
        rejects([&] { kinematic.add_impulse({1, 0}); });
    }
    {
        World world({{0, 0}, 16});
        const auto wall = world.create(box({}, {.05F, 5}));
        auto s = box({-5, 0}, {.2F, .2F}, Motion::dynamic);
        s.collider.shape = Shape::capsule;
        s.collider.radius = .2F;
        s.collider.half_height = .3F;
        s.continuous = true;
        s.velocity = {90, 0};
        auto fast = world.create(s);
        world.step(.1);
        check(fast.pose().position.x < 0, "Continuous capsule tunneled through thin wall");
        fast.set_enabled(false);
        fast.teleport({{-4, 0}, 0});
        fast.set_velocity({});
        fast.set_angular_velocity(1);
        fast.set_enabled(true);
        world.step(1. / 60);
        check(fast.pose().angle > 0 && fast.angular_velocity() > 0, "Angular state lost after disable");
        (void)wall;
        for (float bad : {-1.F, 0.F, .001F, std::numeric_limits<float>::infinity()}) {
            s.collider.radius = bad;
            rejects([&] { (void)world.create(s); });
        }
        s = {};
        s.motion = static_cast<Motion>(9);
        rejects([&] { (void)world.create(s); });
        s = {};
        s.layer = 16;
        rejects([&] { (void)world.create(s); });
        s = {};
        s.velocity.x = 1;
        rejects([&] { (void)world.create(s); });
        s = {};
        s.fixed_rotation = true;
        s.motion = Motion::dynamic;
        s.angular_velocity = 1;
        rejects([&] { (void)world.create(s); });
        s.angular_velocity = 0;
        auto fixed = world.create(s);
        rejects([&] { fixed.set_angular_velocity(1); });
        rejects([&] { world.step(0); });
        rejects([&] { world.step(.11); });
    }
    for (int i = 0; i < 3; ++i) {
        World world({{}, 1});
        auto body = world.create({});
        rejects([&] { (void)world.create({}); });
        body.remove();
        check(world.size() == 0, "Capacity/removal leaked body");
    }
}
// A fixed-rotation kinematic body keeps exactly the angle it was given, so moving it without
// turning is accepted.
void run_fixed_rotation_angle() {
    constexpr float angle = .3F;
    constexpr float angle_tolerance = 1e-6F;
    World world;
    auto s = box({1, 2}, {.5F, .5F}, Motion::kinematic);
    s.fixed_rotation = true;
    s.pose.angle = angle;
    auto body = world.create(s);
    check(std::abs(body.pose().angle - angle) < angle_tolerance, "Stored angle differs from the requested angle");
    body.move_kinematic({{2, 2}, angle}, 1. / 60);
}
// Box2D asserts that every bounding box stays within 100,000 meters of the origin. The largest collider,
// placed at the furthest accepted origin, must stay inside that limit; any further origin is rejected.
void run_world_limit() {
    constexpr float maximum_position = 79'999;  // The documented 2D position bound.
    constexpr float maximum_dimension = 10'000; // The documented 2D collider dimension bound.
    constexpr float beyond_world_limit = 2e5F;
    World world;
    auto largest = box({maximum_position, -maximum_position}, {1, 1}, Motion::dynamic);
    largest.collider.shape = Shape::capsule;
    largest.collider.radius = maximum_dimension;
    largest.collider.half_height = maximum_dimension;
    (void)world.create(largest);
    world.step(1. / 60);
    rejects([&] { (void)world.create(box({beyond_world_limit, 0}, {.5F, .5F})); });
    rejects([&] { (void)world.create(box({maximum_position + 1, 0}, {.5F, .5F})); });
    check(world.size() == 1, "Out-of-range body was created");
}
// A sensor pair is reported when either body can move, as in the 3D module: a dynamic sensor detects a
// stationary sensor and a stationary body, but that stationary sensor and body never report each other.
void run_sensor_motion_rule() {
    World world({{0, 0}, 8});
    auto zone_settings = box({}, {1, 1});
    zone_settings.sensor = true;
    auto zone = world.create(zone_settings);
    auto wall = world.create(box({}, {1, 1}));
    auto probe_settings = box({}, {.2F, .2F}, Motion::dynamic);
    probe_settings.sensor = true;
    auto probe = world.create(probe_settings);
    world.step(1. / 60);
    const auto events = world.take_events();
    const auto reported = [&](const Body &a, const Body &b) {
        for (const auto &e : events)
            if (e.sensor && e.phase == ContactPhase::begin &&
                ((e.first == a && e.second == b) || (e.first == b && e.second == a)))
                return true;
        return false;
    };
    check(reported(probe, zone), "A dynamic sensor did not detect a stationary sensor");
    check(reported(probe, wall), "A dynamic sensor did not detect a stationary body");
    check(!reported(zone, wall), "Two stationary bodies reported a sensor overlap");
    check(events.size() == 2, "Unexpected sensor events");
}
} // namespace
int main() {
    try {
        run();
        run_fixed_rotation_angle();
        run_world_limit();
        run_sensor_motion_rule();
        std::cout << "PASS 2D physics lifetime, queries, sensors, sleeping and motion\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
