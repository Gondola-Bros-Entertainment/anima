#include <anima/physics2d.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <compare>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace anima::physics2d;
namespace {
constexpr auto motion_layer = "Invalid 2D body motion/layer";
constexpr auto step_range = "2D physics step outside [0.000001, 0.1]";
constexpr auto zero_cast = "Zero 2D cast displacement; use overlap";
constexpr auto position_range = "2D physics position outside supported range";
BodySettings box(Vec2 position, Vec2 extent, Motion motion = Motion::stationary) {
    BodySettings s;
    s.pose.position = position;
    s.collider.half_extent = extent;
    s.motion = motion;
    return s;
}
constexpr double frame = 1. / 60;           // One 60 Hz fixed step.
constexpr float velocity_tolerance = 1e-4F; // Velocities computed from a mass and an inertia.
bool near(float a, float b) { return std::abs(a - b) < velocity_tolerance; }
bool near(Vec2 a, Vec2 b) { return near(a.x, b.x) && near(a.y, b.y); }
// A box's rotational inertia about its center: m / 3 (h_x^2 + h_y^2).
float box_inertia(float mass, Vec2 half) { return mass / 3 * (half.x * half.x + half.y * half.y); }
} // namespace

TEST_CASE("Bodies land, sleep, wake and expire with their world") {
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
        CHECK_MESSAGE(std::abs(ball.pose().position.y - .5F) < .03F, "Ball did not land on floor");
        CHECK_FALSE_MESSAGE(ball.awake(), "Resting body failed to sleep");
        auto events = world.take_events();
        REQUIRE_MESSAGE(events.size() == 1u, "Landing/sleep should retain one contact begin");
        CHECK(events[0].phase == ContactPhase::begin);
        CHECK_FALSE(events[0].sensor);
        for (int i = 0; i < 30; ++i)
            world.step(1. / 60);
        CHECK_MESSAGE(world.take_events().empty(), "Sleeping contact flickered");
        ball.add_impulse({0, 3});
        for (int i = 0; i < 10; ++i)
            world.step(1. / 60);
        events = world.take_events();
        CHECK(ball.awake());
        REQUIRE(events.size() == 1u);
        CHECK_MESSAGE(events[0].phase == ContactPhase::end, "Impulse did not leave contact");
        World foreign;
        auto other = foreign.create({});
        CHECK_FALSE(world.owns(other));
        CHECK_MESSAGE(other != ball, "Foreign body identity alias");
        QueryFilter filter;
        filter.ignore = other;
        CHECK_THROWS_WITH_AS(world.raycast({0, 10}, {0, -20}, filter), "Foreign 2D query body", std::invalid_argument);
        CHECK_THROWS_WITH_AS(world.set_layer_collision(0, 1, false), "Configure 2D layers before creating bodies",
                             std::logic_error);
        floor.remove();
        ball.remove();
        ball.remove();
        CHECK_FALSE_MESSAGE(stale.valid(), "Removed identity remains valid");
        const auto replacement = world.create({});
        CHECK_MESSAGE(replacement != stale, "Backend slot reuse resurrected body");
        CHECK_FALSE(stale.valid());
        CHECK_THROWS_WITH_AS(stale.pose(), "Expired 2D physics body", std::out_of_range);
        stale = replacement;
    }
    CHECK_FALSE_MESSAGE(stale.valid(), "Destroyed world retained body");
    stale.remove();
}

TEST_CASE("Rays, sweeps and overlaps honor surfaces, filters, enablement and rotation") {
    World world({{0, 0}, 16});
    world.set_layer_collision(0, 0, false); // Query filtering must not depend on simulation mask.
    auto ground = world.create(box({0, -.5F}, {4, .5F}));
    auto bridge = world.create(box({0, 3}, {2, .2F}));
    auto ray = world.raycast({0, 6}, {0, -10});
    REQUIRE(ray);
    CHECK(ray->body == bridge);
    CHECK(std::abs(ray->fraction - .28F) < .001F);
    CHECK(ray->normal.y > .99F);
    ray = world.raycast({0, 1}, {0, 4});
    REQUIRE_MESSAGE(ray, "Bridge underside missed");
    CHECK(ray->body == bridge);
    CHECK(ray->normal.y < -.99F);
    Collider query;
    query.shape = Shape::circle;
    query.radius = .5F;
    auto sweep = world.sweep(query, {{0, 100}}, {0, -200});
    REQUIRE_MESSAGE(sweep, "Large displacement sweep tunneled through bridge");
    CHECK(sweep->body == bridge);
    CHECK(std::abs(sweep->fraction - .4815F) < .001F);
    auto overlaps = world.overlap(query, {{0, .25F}});
    REQUIRE_MESSAGE(overlaps.size() == 1u, "Clearance overlap failed");
    CHECK(overlaps[0] == ground);
    sweep = world.sweep(query, {{0, .25F}}, {0, 4});
    REQUIRE(sweep);
    CHECK(sweep->body == ground);
    CHECK_MESSAGE(sweep->initial_overlap, "Initially penetrating sweep did not fail closed");
    CHECK(sweep->fraction == 0);
    ray = world.raycast({0, -.25F}, {0, 4});
    REQUIRE(ray);
    CHECK_MESSAGE(ray->initial_overlap, "Initially penetrating ray missed");
    CHECK(ray->body == ground);
    QueryFilter filter;
    filter.ignore = bridge;
    ray = world.raycast({0, 6}, {0, -10}, filter);
    REQUIRE(ray);
    CHECK_MESSAGE(ray->body == ground, "Ignore-body filter failed");
    filter.layers = 2;
    CHECK_FALSE_MESSAGE(world.raycast({0, 6}, {0, -10}, filter), "Layer query filter failed");
    bridge.set_enabled(false);
    ray = world.raycast({0, 6}, {0, -10});
    REQUIRE(ray);
    CHECK_MESSAGE(ray->body == ground, "Disabled body remains queryable");
    bridge.set_enabled(true);
    bridge.teleport({{0, 3}, 1.57079632679F});
    ray = world.raycast({-4, 3}, {8, 0});
    REQUIRE_MESSAGE(ray, "Rotated shape query failed");
    CHECK(ray->body == bridge);
    CHECK(ray->normal.x < -.99F);
    query.shape = Shape::capsule;
    sweep = world.sweep(query, {{3, 5}, .5F}, {0, -10});
    REQUIRE_MESSAGE(sweep, "Rotated capsule sweep failed");
    CHECK(sweep->body == ground);
    query.shape = Shape::box;
    sweep = world.sweep(query, {{3, 5}, .5F}, {0, -10});
    REQUIRE_MESSAGE(sweep, "Rotated box sweep failed");
    CHECK(sweep->body == ground);
    CHECK_THROWS_WITH_AS(world.raycast({}, {}), zero_cast, std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.sweep(query, {}, {}), zero_cast, std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.overlap(query, {{}, std::numeric_limits<float>::quiet_NaN()}),
                         "2D physics value outside finite supported range", std::invalid_argument);
}

TEST_CASE("Collider factories build bodies of their shapes and leave other fields at their defaults") {
    constexpr float surface_tolerance = .001F;
    constexpr auto dimensions = "2D collider dimensions outside [0.01, 10000]";
    const Collider defaults;
    const auto box_collider = Collider::box({1, .25F});
    CHECK((box_collider.shape == Shape::box && box_collider.half_extent.x == 1 && box_collider.half_extent.y == .25F &&
           box_collider.radius == defaults.radius && box_collider.half_height == defaults.half_height));
    const auto circle = Collider::circle(.25F);
    CHECK((circle.shape == Shape::circle && circle.radius == .25F && circle.half_extent.x == defaults.half_extent.x &&
           circle.half_extent.y == defaults.half_extent.y && circle.half_height == defaults.half_height));
    const auto capsule = Collider::capsule(.25F, 1);
    CHECK((capsule.shape == Shape::capsule && capsule.radius == .25F && capsule.half_height == 1 &&
           capsule.half_extent.x == defaults.half_extent.x && capsule.half_extent.y == defaults.half_extent.y));

    World world({{0, 0}, 8});
    // Each body stands at x = 10 i; a vertical ray from y = 5 finds its top surface, or nothing.
    const auto top = [&](float x) -> std::optional<float> {
        const auto hit = world.raycast({x, 5}, {0, -10});
        if (!hit)
            return std::nullopt;
        return hit->point.y;
    };
    const auto at = [&](const Collider &collider, float x) {
        BodySettings settings;
        settings.collider = collider;
        settings.pose.position = {x, 0};
        return world.create(settings);
    };
    const auto on = [](std::optional<float> y, float expected) {
        return y && std::abs(*y - expected) < surface_tolerance;
    };
    (void)at(box_collider, 0);
    CHECK_MESSAGE((on(top(.9F), .25F) && !top(1.1F)), "Box factory built the wrong extent");
    (void)at(circle, 10);
    // A box of half size 0.25 would be flat at x = 10.2; the circle's top there is at 0.15.
    CHECK_MESSAGE((on(top(10), .25F) && on(top(10.2F), .15F)), "Circle factory built the wrong shape");
    (void)at(capsule, 20);
    CHECK_MESSAGE((on(top(20), 1.25F) && on(top(20.2F), 1.15F) && !top(20.3F)),
                  "Capsule factory built the wrong shape");
    CHECK(world.size() == 3u);
    // The factories validate nothing; World::create rejects their invalid dimensions.
    BodySettings invalid;
    invalid.collider = Collider::circle(0);
    CHECK_THROWS_WITH_AS(world.create(invalid), dimensions, std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.overlap(Collider::capsule(.25F, 0), {}), dimensions, std::invalid_argument);
    CHECK(world.size() == 3u);
}

TEST_CASE("Collision layer masks build from layers and the 2D layer matrix reads back") {
    static_assert(anima::collision_layer_count == 16u);
    static_assert(anima::all_collision_layers == 0xffff);
    static_assert(anima::collision_layer_mask({2, 5}) == 0x24);
    CHECK_THROWS_WITH_AS((void)anima::collision_layer_mask({16}), "Collision layer must be in [0,15]",
                         std::invalid_argument);
    CHECK(QueryFilter{}.layers == anima::all_collision_layers);
    World world;
    CHECK(world.layers_collide(1, 2));
    world.set_layer_collision(1, 2, false);
    CHECK_FALSE(world.layers_collide(2, 1));
    CHECK_FALSE(world.layers_collide(1, 2));
    CHECK(world.layers_collide(1, 1));
    CHECK(world.layers_collide(2, 3));
    world.set_layer_collision(2, 1, true);
    CHECK(world.layers_collide(1, 2));
    world.set_layer_collision(4, 4, false);
    [[maybe_unused]] const auto body = world.create(box({}, {1, 1}));
    CHECK_FALSE_MESSAGE(world.layers_collide(4, 4), "The matrix is not readable once bodies exist");
    CHECK_THROWS_WITH_AS((void)world.layers_collide(16, 0), "2D physics layer must be in [0,15]",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)world.layers_collide(0, 16), "2D physics layer must be in [0,15]",
                         std::invalid_argument);
}

TEST_CASE("Sensors honor layers, report each transition once and kinematic bodies reach their targets") {
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
    CHECK_MESSAGE(events.size() == 1u, "Two stationary bodies reported a sensor overlap");
    unsigned starts = 0;
    for (auto e : events)
        if (e.first == visitor || e.second == visitor) {
            CHECK(e.sensor);
            CHECK(e.phase == ContactPhase::begin);
            CHECK_MESSAGE((e.first == sensor || e.second == sensor), "Layer response leaked");
            ++starts;
        }
    CHECK_MESSAGE(starts == 1u, "Missing sensor enter");
    QueryFilter f;
    f.layers = 1u << 2;
    CHECK_FALSE_MESSAGE(world.raycast({0, 3}, {0, -6}, f), "Sensor query defaults incorrect");
    f.sensors = true;
    const auto hit = world.raycast({0, 3}, {0, -6}, f);
    REQUIRE_MESSAGE(hit, "Explicit sensor query missing");
    CHECK(hit->body == sensor);
    visitor.set_enabled(false);
    events = world.take_events();
    REQUIRE_MESSAGE(events.size() == 1u, "Disable did not close contact once");
    CHECK(events[0].phase == ContactPhase::end);
    visitor.set_enabled(true);
    world.step(1. / 60);
    events = world.take_events();
    REQUIRE_MESSAGE(events.size() == 1u, "Reenable did not reopen contact");
    CHECK(events[0].phase == ContactPhase::begin);
    visitor.remove();
    auto replacement = world.create(box({10, 0}, {.2F, .2F}, Motion::dynamic));
    world.step(1. / 60);
    events = world.take_events();
    REQUIRE(events.size() == 1u);
    CHECK(events[0].phase == ContactPhase::end);
    CHECK_MESSAGE((events[0].first == visitor || events[0].second == visitor), "Removal or buffered stale event alias");
    replacement.remove();
    blocker.remove();
    sensor.remove();
    (void)world.take_events();
    auto kinematic = world.create(box({10, 0}, {.5F, .5F}, Motion::kinematic));
    kinematic.move_kinematic({{11, 0}, .1F}, .1);
    world.step(.1);
    CHECK(std::abs(kinematic.pose().position.x - 11) < .01F);
    CHECK(std::abs(kinematic.pose().angle - .1F) < .001F);
    const auto stopped = kinematic.pose();
    kinematic.move_kinematic(stopped, .1);
    world.step(.1);
    CHECK(std::abs(kinematic.pose().position.x - stopped.position.x) < .001F);
    CHECK_MESSAGE(kinematic.velocity().x == 0, "Stationary kinematic target retained old velocity");
    CHECK_THROWS_WITH_AS(kinematic.add_impulse({1, 0}), "Only enabled dynamic 2D bodies accept impulses",
                         std::invalid_argument);
}

TEST_CASE("A continuous capsule stops at a thin wall and invalid settings are rejected") {
    World world({{0, 0}, 16});
    auto wall = world.create(box({}, {.05F, 5}));
    auto s = box({-5, 0}, {.2F, .2F}, Motion::dynamic);
    s.collider.shape = Shape::capsule;
    s.collider.radius = .2F;
    s.collider.half_height = .3F;
    s.continuous = true;
    s.velocity = {90, 0};
    auto fast = world.create(s);
    world.step(.1);
    CHECK_MESSAGE(fast.pose().position.x < 0, "Continuous capsule tunneled through thin wall");
    fast.set_enabled(false);
    fast.teleport({{-4, 0}, 0});
    fast.set_velocity({});
    fast.set_angular_velocity(1);
    fast.set_enabled(true);
    world.step(1. / 60);
    CHECK_MESSAGE(fast.pose().angle > 0, "Angular state lost after disable");
    CHECK(fast.angular_velocity() > 0);
    CHECK_THROWS_WITH_AS(wall.set_velocity({1, 0}), "Stationary 2D bodies cannot have velocity", std::invalid_argument);
    for (float bad : {-1.F, 0.F, .001F, std::numeric_limits<float>::infinity()}) {
        CAPTURE(bad);
        s.collider.radius = bad;
        CHECK_THROWS_WITH_AS(world.create(s), "2D collider dimensions outside [0.01, 10000]", std::invalid_argument);
    }
    s = {};
    s.motion = static_cast<Motion>(9);
    CHECK_THROWS_WITH_AS(world.create(s), motion_layer, std::invalid_argument);
    s = {};
    s.layer = 16;
    CHECK_THROWS_WITH_AS(world.create(s), motion_layer, std::invalid_argument);
    s = {};
    s.velocity.x = 1;
    CHECK_THROWS_WITH_AS(world.create(s), "Stationary 2D body has velocity", std::invalid_argument);
    s = {};
    s.fixed_rotation = true;
    s.motion = Motion::dynamic;
    s.angular_velocity = 1;
    CHECK_THROWS_WITH_AS(world.create(s), "Fixed-rotation 2D body has angular velocity", std::invalid_argument);
    s.angular_velocity = 0;
    auto fixed = world.create(s);
    CHECK_THROWS_WITH_AS(fixed.set_angular_velocity(1), "2D body cannot rotate", std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.step(0), step_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.step(.11), step_range, std::invalid_argument);
}

TEST_CASE("Body capacity is enforced across repeated world lifetimes") {
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        World world({{}, 1});
        auto body = world.create({});
        CHECK_THROWS_WITH_AS(world.create({}), "2D physics body capacity exhausted", std::length_error);
        body.remove();
        CHECK_MESSAGE(world.size() == 0u, "Capacity/removal leaked body");
    }
}

// A fixed-rotation kinematic body keeps exactly the angle it was given, so moving it without
// turning is accepted.
TEST_CASE("A fixed-rotation kinematic body keeps its angle and moves without turning") {
    constexpr float angle = .3F;
    constexpr float angle_tolerance = 1e-6F;
    World world;
    auto s = box({1, 2}, {.5F, .5F}, Motion::kinematic);
    s.fixed_rotation = true;
    s.pose.angle = angle;
    auto body = world.create(s);
    CHECK_MESSAGE(std::abs(body.pose().angle - angle) < angle_tolerance,
                  "Stored angle differs from the requested angle");
    CHECK_NOTHROW(body.move_kinematic({{2, 2}, angle}, 1. / 60));
}

// Box2D asserts that every bounding box stays within 100,000 meters of the origin. The largest collider,
// placed at the furthest accepted origin, must stay inside that limit; any further origin is rejected.
TEST_CASE("Bodies stay inside Box2D's world limit") {
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
    CHECK_THROWS_WITH_AS(world.create(box({beyond_world_limit, 0}, {.5F, .5F})), position_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(world.create(box({maximum_position + 1, 0}, {.5F, .5F})), position_range,
                         std::invalid_argument);
    CHECK_MESSAGE(world.size() == 1u, "Out-of-range body was created");
}

// A sensor pair is reported when either body can move, as in the 3D module: a dynamic sensor detects a
// stationary sensor and a stationary body, but that stationary sensor and body never report each other.
TEST_CASE("A sensor pair is reported only when either body can move") {
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
    CHECK_MESSAGE(reported(probe, zone), "A dynamic sensor did not detect a stationary sensor");
    CHECK_MESSAGE(reported(probe, wall), "A dynamic sensor did not detect a stationary body");
    CHECK_FALSE_MESSAGE(reported(zone, wall), "Two stationary bodies reported a sensor overlap");
    CHECK(events.size() == 2u);
    for (const auto &event : events)
        CHECK_FALSE_MESSAGE(event.contact, "A sensor overlap reported a contact point");
}

TEST_CASE("A solid contact's begin event reports its point, normal and approach speed") {
    constexpr double tick = 1. / 60;
    constexpr float impact_speed = 5; // Without gravity the crate meets the floor at its launch speed.
    constexpr float tolerance = .002F;
    // Creating the crate first names it first, while Box2D orders the pair by its broad-phase proxies, which puts
    // the floor first either way.
    for (const bool floor_first : {true, false}) {
        CAPTURE(floor_first);
        World world({{0, 0}, 8});
        const auto floor_settings = box({0, -.5F}, {10, .5F});
        auto crate_settings = box({0, 1}, {.5F, .5F}, Motion::dynamic);
        crate_settings.velocity = {0, -impact_speed};
        Body floor, crate;
        if (floor_first) {
            floor = world.create(floor_settings);
            crate = world.create(crate_settings);
        } else {
            crate = world.create(crate_settings);
            floor = world.create(floor_settings);
        }
        std::vector<ContactEvent> events;
        for (int i = 0; i < 240 && events.empty(); ++i) {
            world.step(tick);
            events = world.take_events();
        }
        REQUIRE(events.size() == 1u);
        const auto &begin = events[0];
        REQUIRE((begin.phase == ContactPhase::begin && !begin.sensor));
        REQUIRE_MESSAGE(begin.contact, "A solid begin event has no contact point");
        CHECK((begin.first == floor) == floor_first);
        const float up = floor_first ? 1.F : -1.F;
        CHECK_MESSAGE(
            (std::abs(begin.contact->normal.x) < tolerance && std::abs(begin.contact->normal.y - up) < tolerance),
            "The normal does not point from the first body toward the second");
        // The crate lands flat, centered on the origin, and the surfaces were at most one step's travel apart.
        CHECK(std::abs(begin.contact->point.x) < tolerance);
        CHECK(std::abs(begin.contact->point.y) <= impact_speed * tick);
        CHECK(std::abs(begin.contact->approach_speed - impact_speed) < tolerance);
        crate.teleport({{0, 10}});
        world.step(tick);
        events = world.take_events();
        REQUIRE(events.size() == 1u);
        CHECK(events[0].phase == ContactPhase::end);
        CHECK_FALSE_MESSAGE(events[0].contact, "An end event reported a contact point");
    }
}

TEST_CASE("Body vectors combine with core's Vec2 arithmetic") {
    World world;
    auto body = world.create(box({0, 0}, {.5F, .5F}, Motion::kinematic));
    const Vec2 direction = anima::normalized(Vec2{3, 4});
    body.set_velocity({1, 0});
    body.set_velocity(body.velocity() + direction * 2.F);
    CHECK(body.velocity().x == doctest::Approx(2.2F));
    CHECK(body.velocity().y == doctest::Approx(1.6F));
    CHECK(length(body.velocity() - Vec2{2.2F, 1.6F}) < 1e-5F);
}

TEST_CASE("Damping slows only dynamic bodies, by the documented factor per substep") {
    constexpr int ticks = 60; // One second.
    constexpr double tick = 1. / 60;
    constexpr unsigned substeps = 4;
    constexpr float damping = 1;
    constexpr float decay_tolerance = 1e-4F; // Float rounding over the substeps.
    constexpr auto invalid_damping = "Invalid 2D body damping";
    World world({{0, 0}, 8, substeps});
    const Vec2 velocity{1, 0};
    const float spin = 1;
    auto s = box({}, {.5F, .5F}, Motion::dynamic);
    s.velocity = velocity;
    s.angular_velocity = spin;
    auto undamped = world.create(s);
    s.pose.position = {0, 5};
    s.linear_damping = damping;
    s.angular_damping = damping;
    auto damped = world.create(s);
    s.pose.position = {0, 10};
    s.motion = Motion::kinematic;
    auto kinematic = world.create(s);
    for (int i = 0; i < ticks; ++i)
        world.step(tick);
    CHECK_MESSAGE((undamped.velocity().x == velocity.x && undamped.velocity().y == velocity.y &&
                   undamped.angular_velocity() == spin),
                  "An undamped body lost speed");
    const auto factor = static_cast<float>(std::pow(1 / (1 + damping * tick / substeps), ticks * substeps));
    CHECK_MESSAGE((std::abs(damped.velocity().x - velocity.x * factor) < decay_tolerance &&
                   std::abs(damped.velocity().y) < decay_tolerance),
                  "Linear damping did not scale the velocity by 1 / (1 + c h) per substep");
    CHECK_MESSAGE(std::abs(damped.angular_velocity() - spin * factor) < decay_tolerance,
                  "Angular damping did not scale the angular velocity by 1 / (1 + c h) per substep");
    CHECK_MESSAGE((kinematic.velocity().x == velocity.x && kinematic.velocity().y == velocity.y &&
                   kinematic.angular_velocity() == spin),
                  "Damping slowed a kinematic body");
    for (const float bad :
         {-1.F, 61.F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        CAPTURE(bad);
        auto linear = box({}, {.5F, .5F}, Motion::dynamic);
        linear.linear_damping = bad;
        CHECK_THROWS_WITH_AS(world.create(linear), invalid_damping, std::invalid_argument);
        auto angular = box({}, {.5F, .5F}, Motion::dynamic);
        angular.angular_damping = bad;
        CHECK_THROWS_WITH_AS(world.create(angular), invalid_damping, std::invalid_argument);
    }
    s.linear_damping = 60;
    s.angular_damping = 60;
    (void)world.create(s);
    CHECK(world.size() == 4u);
}

TEST_CASE("A dynamic body's mass is BodySettings::mass for every shape and sets its response to impulses") {
    constexpr float mass = 10;
    constexpr float mass_tolerance = 1e-5F * mass; // Float rounding of density times area.
    constexpr auto dynamic_only = "Only dynamic 2D bodies have mass";
    constexpr auto invalid_mass = "Invalid 2D body mass/material";
    World world({{0, 0}, 8});
    auto s = box({}, {.5F, .25F}, Motion::dynamic);
    s.mass = mass;
    s.collider.shape = Shape::circle;
    auto circle = world.create(s);
    CHECK(std::abs(circle.mass() - mass) < mass_tolerance);
    circle.add_impulse({mass, 0});
    CHECK_MESSAGE((std::abs(circle.velocity().x - 1) < mass_tolerance && circle.velocity().y == 0),
                  "An impulse did not change the velocity by impulse / mass");
    circle.set_enabled(false);
    CHECK_MESSAGE(std::abs(circle.mass() - mass) < mass_tolerance, "A disabled body lost its mass");
    s.pose.position = {0, 5};
    s.collider.shape = Shape::box;
    CHECK_MESSAGE(std::abs(world.create(s).mass() - mass) < mass_tolerance, "A box's mass is not BodySettings::mass");
    s.pose.position = {0, 10};
    s.collider.shape = Shape::capsule;
    s.collider.radius = .25F;
    CHECK_MESSAGE(std::abs(world.create(s).mass() - mass) < mass_tolerance,
                  "A capsule's mass is not BodySettings::mass");
    CHECK_THROWS_WITH_AS((void)world.create(box({0, 15}, {.5F, .5F})).mass(), dynamic_only, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)world.create(box({0, 20}, {.5F, .5F}, Motion::kinematic)).mass(), dynamic_only,
                         std::invalid_argument);
    for (const float bad :
         {0.F, .0009F, 1'000'001.F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        CAPTURE(bad);
        auto invalid = box({}, {.5F, .5F}, Motion::dynamic);
        invalid.mass = bad;
        CHECK_THROWS_WITH_AS(world.create(invalid), invalid_mass, std::invalid_argument);
    }
    circle.remove();
    CHECK_THROWS_WITH_AS((void)circle.mass(), "Expired 2D physics body", std::out_of_range);
}

TEST_CASE("Impulses at points and angular impulses follow the body's inertia") {
    constexpr float mass = 3;
    const Vec2 half{1, .5F};
    const float inertia = box_inertia(mass, half); // 1.25
    World world({{0, 0}, 8});
    auto s = box({5, 2}, half, Motion::dynamic);
    s.mass = mass;
    auto struck = world.create(s);
    // At the corner (1, 0.5) from the center, an impulse of 2 along +Y has the moment 2.
    struck.add_impulse_at({0, 2}, {5 + half.x, 2 + half.y});
    CHECK_MESSAGE(near(struck.velocity(), {0, 2 / mass}), "An impulse at a point did not change the velocity by "
                                                          "impulse / mass");
    CHECK_MESSAGE(near(struck.angular_velocity(), 2 / inertia),
                  "An impulse at a corner did not turn the body by its moment over the inertia");
    s.pose.position = {-5, 2};
    auto spun = world.create(s);
    spun.add_angular_impulse(-inertia);
    CHECK_MESSAGE(near(spun.angular_velocity(), -1), "An angular impulse did not change the angular velocity by "
                                                     "impulse / inertia");
    CHECK(spun.velocity().x == 0);
    CHECK(spun.velocity().y == 0);
    s.pose.position = {0, -5};
    s.fixed_rotation = true;
    auto fixed = world.create(s);
    fixed.add_impulse_at({0, 2}, {half.x, -5 + half.y});
    fixed.add_angular_impulse(inertia);
    CHECK_MESSAGE(fixed.angular_velocity() == 0, "A fixed-rotation body turned");
    CHECK(near(fixed.velocity(), {0, 2 / mass}));
}

TEST_CASE("Forces and torques act over the next step and are then cleared") {
    constexpr float mass = 3;
    const Vec2 half{1, .5F};
    const float inertia = box_inertia(mass, half);
    const auto seconds = static_cast<float>(frame);
    World world; // Earth gravity, which a force of m g balances.
    auto s = box({0, 10}, half, Motion::dynamic);
    s.mass = mass;
    auto held = world.create(s);
    const float weight = held.mass() * 9.81F;
    held.add_force({0, weight / 2});
    held.add_force({0, weight / 2});
    world.step(frame);
    CHECK_MESSAGE((near(held.velocity(), {}) && near(held.pose().position, {0, 10})),
                  "The sum of the forces did not hold the body against gravity");
    world.step(frame);
    CHECK_MESSAGE(near(held.velocity(), {0, -9.81F * seconds}), "A step kept a force from the step before");

    World space({{0, 0}, 8});
    s.pose.position = {};
    auto pushed = space.create(s);
    // At the corner (1, 0.5) from the center, a force of 6 along +Y has the moment 6.
    pushed.add_force_at({0, 6}, half);
    s.pose.position = {5, 0};
    auto twisted = space.create(s);
    twisted.add_torque(inertia / seconds);
    s.pose.position = {-5, 0};
    auto thrown = space.create(s);
    thrown.add_force({6, 0});
    s.pose.position = {0, 5};
    s.fixed_rotation = true;
    auto fixed = space.create(s);
    fixed.add_force_at({0, 6}, {half.x, 5 + half.y});
    fixed.add_torque(inertia / seconds);
    space.step(frame);
    const Vec2 pushed_velocity{0, 6 / mass * seconds};
    const float pushed_spin = 6 / inertia * seconds;
    const Vec2 thrown_velocity{6 / mass * seconds, 0};
    CHECK_MESSAGE((near(pushed.velocity(), pushed_velocity) && near(pushed.angular_velocity(), pushed_spin)),
                  "A force at a point did not act as the force and its moment for one step");
    CHECK_MESSAGE((near(twisted.angular_velocity(), 1) && near(twisted.velocity(), {})),
                  "A torque did not change the angular velocity by torque / inertia per second");
    CHECK(near(thrown.velocity(), thrown_velocity));
    CHECK_MESSAGE((fixed.angular_velocity() == 0 && near(fixed.velocity(), pushed_velocity)),
                  "A fixed-rotation body turned under a torque");
    space.step(frame);
    CHECK_MESSAGE((near(pushed.velocity(), pushed_velocity) && near(pushed.angular_velocity(), pushed_spin) &&
                   near(twisted.angular_velocity(), 1) && near(thrown.velocity(), thrown_velocity)),
                  "A step kept a force or torque from the step before");
    // The force acts over every substep of a long step.
    constexpr double long_step = .1;
    thrown.add_force({6, 0});
    space.step(long_step);
    CHECK_MESSAGE(near(thrown.velocity(), {6 / mass * (seconds + static_cast<float>(long_step)), 0}),
                  "A force did not act over the whole of a long step");
}

TEST_CASE("Forces and torques wake a sleeping body") {
    World world;
    (void)world.create(box({0, -.5F}, {10, .5F}));
    auto resting = world.create(box({0, .5F}, {.5F, .5F}, Motion::dynamic));
    for (int i = 0; i < 120; ++i)
        world.step(frame);
    REQUIRE_FALSE_MESSAGE(resting.awake(), "A resting body did not sleep");
    resting.add_torque(1);
    CHECK_MESSAGE(resting.awake(), "A torque did not wake the body");
    world.step(frame);
    CHECK_MESSAGE(resting.angular_velocity() != 0, "A torque on a sleeping body did not act");
}

TEST_CASE("Disabling a body discards its forces and torques") {
    World world({{0, 0}, 1});
    auto body = world.create(box({}, {.5F, .5F}, Motion::dynamic));
    body.add_force({100, 0});
    body.add_force_at({0, 50}, {1, 0});
    body.add_torque(20);
    body.set_enabled(false);
    body.set_enabled(true);
    world.step(frame);
    CHECK_MESSAGE((body.velocity().x == 0 && body.velocity().y == 0 && body.angular_velocity() == 0),
                  "A force or torque added before the body was disabled acted after it was reenabled");
    // Each disable discards exactly what was added since the step or the last disable.
    body.add_force({3, 0});
    body.add_torque(1);
    body.set_enabled(false);
    body.set_enabled(true);
    body.add_force({6, 0});
    body.add_torque(2);
    body.set_enabled(false);
    body.set_enabled(true);
    world.step(frame);
    CHECK_MESSAGE((body.velocity().x == 0 && body.velocity().y == 0 && body.angular_velocity() == 0),
                  "A second disable did not discard the forces added since the first");
    body.add_force({6, 0});
    world.step(frame);
    CHECK_MESSAGE(near(body.velocity(), {6 * static_cast<float>(frame), 0}),
                  "A force added after disabling and reenabling did not act");
}

TEST_CASE("Forces, torques and impulses require an enabled dynamic body and valid values") {
    constexpr auto impulses_only = "Only enabled dynamic 2D bodies accept impulses";
    constexpr auto forces_only = "Only enabled dynamic 2D bodies accept forces and torques";
    constexpr auto value_range = "2D physics value outside finite supported range";
    constexpr auto expired = "Expired 2D physics body";
    World world({{0, 0}, 4});
    auto stationary = world.create(box({}, {.5F, .5F}));
    auto kinematic = world.create(box({3, 0}, {.5F, .5F}, Motion::kinematic));
    auto disabled = world.create(box({6, 0}, {.5F, .5F}, Motion::dynamic));
    disabled.set_enabled(false);
    for (auto *body : {&stationary, &kinematic, &disabled}) {
        CHECK_THROWS_WITH_AS(body->add_impulse_at({0, 1}, {}), impulses_only, std::invalid_argument);
        CHECK_THROWS_WITH_AS(body->add_angular_impulse(1), impulses_only, std::invalid_argument);
        CHECK_THROWS_WITH_AS(body->add_force({0, 1}), forces_only, std::invalid_argument);
        CHECK_THROWS_WITH_AS(body->add_force_at({0, 1}, {}), forces_only, std::invalid_argument);
        CHECK_THROWS_WITH_AS(body->add_torque(1), forces_only, std::invalid_argument);
    }
    CHECK_FALSE_MESSAGE(disabled.enabled(), "A rejected call reenabled a body");
    auto dynamic = world.create(box({9, 0}, {.5F, .5F}, Motion::dynamic));
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), 2e6F}) {
        CAPTURE(bad);
        CHECK_THROWS_WITH_AS(dynamic.add_impulse_at({bad, 0}, {}), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_impulse_at({0, 1}, {0, bad}), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_angular_impulse(bad), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_force({bad, 0}), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_force_at({0, bad}, {}), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_force_at({0, 1}, {bad, 0}), value_range, std::invalid_argument);
        CHECK_THROWS_WITH_AS(dynamic.add_torque(bad), value_range, std::invalid_argument);
    }
    world.step(frame);
    CHECK_MESSAGE((dynamic.velocity().x == 0 && dynamic.velocity().y == 0 && dynamic.angular_velocity() == 0),
                  "A rejected call moved the body");
    dynamic.remove();
    CHECK_THROWS_WITH_AS(dynamic.add_impulse_at({0, 1}, {}), expired, std::out_of_range);
    CHECK_THROWS_WITH_AS(dynamic.add_angular_impulse(1), expired, std::out_of_range);
    CHECK_THROWS_WITH_AS(dynamic.add_force({0, 1}), expired, std::out_of_range);
    CHECK_THROWS_WITH_AS(dynamic.add_force_at({0, 1}, {}), expired, std::out_of_range);
    CHECK_THROWS_WITH_AS(dynamic.add_torque(1), expired, std::out_of_range);
}

TEST_CASE("Body handles hash and order consistently with their identity") {
    World first, second;
    auto a = first.create({});
    const auto b = first.create({}), c = second.create({});
    const auto copy = a;
    CHECK(std::hash<Body>{}(copy) == std::hash<Body>{}(a));
    CHECK((copy <=> a) == std::strong_ordering::equal);
    CHECK_MESSAGE(a < b, "A body created later in a world did not order after an earlier one");
    CHECK(Body{} < a);
    // The first bodies of two worlds share a creation position but are distinct and ordered.
    CHECK(a != c);
    CHECK((a < c) != (c < a));
    const std::unordered_map<Body, int> indices{{a, 0}, {b, 1}, {c, 2}};
    const std::set<Body> ordered{c, b, a, copy};
    CHECK(indices.size() == 3u);
    CHECK(ordered.size() == 3u);
    a.remove();
    REQUIRE_FALSE(copy.valid());
    CHECK_MESSAGE(indices.at(copy) == 0, "A removed body's handle no longer finds its key");
    CHECK(ordered.contains(copy));
}
