#include <anima/physics.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace anima;
using namespace anima::physics;
namespace {
constexpr double tick = 1. / 60;                             // One 60 Hz fixed step.
constexpr double long_step = .1;                             // The longest step World::step accepts.
constexpr int settle_ticks = 240;                            // Four seconds of 60 Hz simulation.
constexpr float tolerance = .002F;                           // Positions, centers of mass and sweep fractions.
constexpr float surface_tolerance = .001F;                   // Ray and sweep contacts on analytic surfaces.
constexpr float axis_alignment = .99F;                       // A unit normal this aligned faces along an axis.
constexpr float resting_height = .5F;                        // A unit box's origin height on a floor at y = 0.
constexpr float settle_tolerance = .03F;                     // Solver slop after settling.
constexpr Quat identity_rotation{0, 0, 0, 1};                // Quat{} is all zeros, not the identity.
constexpr Quat quarter_turn_z{0, 0, .70710678F, .70710678F}; // 90 degrees about +Z.
constexpr std::size_t hull_point_limit = 256;                // Documented in include/anima/physics.hpp.
constexpr std::size_t compound_child_limit = 64;
constexpr float beyond_vector_range = 2e6F; // Physics vectors are limited to +/-1e6.
constexpr auto expired_body = "Expired physics body";
constexpr auto vector_range = "Physics vector outside finite supported range";
constexpr auto step_range = "Physics step must be in [0.000001, 0.1] seconds";
constexpr auto thin_hull = "Hull points are coplanar or too thin";
constexpr auto compound_count = "Compound requires 1..64 children";
constexpr auto compound_child = "Compound children must be primitives or hulls";
constexpr auto stray_children = "Noncompound collider contains children";

bool near(Vec3 a, Vec3 b) { return length(a - b) < tolerance; }
WorldSettings weightless(std::uint32_t max_bodies) { return {{0, 0, 0}, max_bodies}; }
BodySettings box(Vec3 position, Vec3 extent, Motion motion = Motion::stationary) {
    BodySettings s;
    s.pose.position = position;
    s.collider.half_extent = extent;
    s.motion = motion;
    return s;
}
Collider tetrahedron() {
    Collider c;
    c.shape = Shape::convex_hull;
    c.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    return c;
}
// Every entry point that compiles a collider must reject it the same way without creating a body.
template <class Expected> void rejects_collider(World &world, const Collider &collider, const char *message) {
    BodySettings settings;
    settings.collider = collider;
    REQUIRE_THROWS_WITH_AS(world.create(settings), message, Expected);
    REQUIRE_THROWS_WITH_AS(world.sweep(collider, {}, {1, 0, 0}), message, Expected);
    REQUIRE_THROWS_WITH_AS(world.overlap(collider, {}), message, Expected);
    REQUIRE(world.size() == 0u);
}
} // namespace

TEST_CASE("A hull's mass properties follow its volume centroid") {
    World world(weightless(16));
    BodySettings s;
    s.collider = tetrahedron();
    s.motion = Motion::dynamic;
    s.mass = 2;
    s.pose.position = {4, 2, 3};
    const Vec3 centroid{.5F, .25F, .25F}; // Analytic volume centroid of the tetrahedron.
    const Vec3 center = s.pose.position + centroid;
    auto body = world.create(s);
    REQUIRE_MESSAGE((near(body.local_center_of_mass(), centroid) && near(body.world_center_of_mass(), center)),
                    "Automatic hull center of mass is not its volume centroid");
    REQUIRE_MESSAGE(near(body.pose().position, s.pose.position), "Hull creation exposed center of mass as origin");
    body.add_impulse({2, 0, 0});
    REQUIRE_MESSAGE(near(body.velocity(), {1, 0, 0}), "Hull total mass was not applied");
    body.set_velocity({});
    body.set_angular_velocity({0, 0, 2});
    world.step(long_step);
    const auto pose = body.pose();
    REQUIRE_MESSAGE(near(point(matrix({pose.position, pose.rotation, {1, 1, 1}}), centroid), center),
                    "Hull rotation moved its center of mass");
    REQUIRE_MESSAGE(!near(pose.position, s.pose.position), "Offset hull origin failed to orbit its center of mass");
    body.teleport({{0, 0, 0}, quarter_turn_z});
    REQUIRE_MESSAGE(near(body.pose().position, {}), "Hull teleport moved authored origin");
    const auto ray = world.raycast({-2, .25F, .1F}, {4, 0, 0});
    REQUIRE_MESSAGE((ray && ray->body == body && ray->point.x < -.5F), "Rotated asymmetric hull geometry misplaced");
    body.remove();
    s.motion = Motion::kinematic;
    s.pose = {};
    auto moving = world.create(s);
    const Vec3 target{2, 3, 0};
    moving.move_kinematic({target, quarter_turn_z}, long_step);
    world.step(long_step);
    REQUIRE_MESSAGE(near(moving.pose().position, target), "Hull kinematic target interpreted as center of mass");
}

TEST_CASE("A compound's center of mass and child placement follow its children") {
    World world(weightless(16));
    BodySettings settings;
    settings.collider.shape = Shape::compound;
    ColliderChild first, second;
    first.pose.position = {1, 0, 0};
    second.pose.position = {3, 1, 0};
    settings.collider.children = {first, second};
    settings.motion = Motion::dynamic;
    settings.mass = 4;
    const Vec3 centroid{2, .5F, 0}; // Midpoint of two equal unit boxes.
    auto compound = world.create(settings);
    REQUIRE_MESSAGE(near(compound.local_center_of_mass(), centroid), "Automatic compound centroid is incorrect");
    settings.collider.children.clear(); // Backend geometry must own its own immutable data.
    compound.add_impulse({0, 4, 0});
    REQUIRE_MESSAGE(near(compound.velocity(), {0, 1, 0}), "Compound total mass was not applied");
    compound.set_velocity({});
    compound.set_angular_velocity({0, 0, 2});
    world.step(long_step);
    const auto pose = compound.pose();
    REQUIRE_MESSAGE(near(point(matrix({pose.position, pose.rotation, {1, 1, 1}}), centroid), centroid),
                    "Compound origin/center of mass rotation is incorrect");
    compound.teleport({{0, 0, 0}, identity_rotation});
    const auto ray = world.raycast({3, 4, 0}, {0, -6, 0});
    // The second child's top face: its center height 1 plus a half extent of 0.5.
    REQUIRE_MESSAGE((ray && ray->body == compound && std::abs(ray->point.y - 1.5F) < surface_tolerance),
                    "Compound child placement or retained geometry failed");
    compound.remove();
    settings.collider.children = {first, second};
    settings.motion = Motion::kinematic;
    auto moving = world.create(settings);
    const Vec3 target{2, 3, 0};
    moving.move_kinematic({target, quarter_turn_z}, long_step);
    world.step(long_step);
    REQUIRE_MESSAGE(near(moving.pose().position, target), "Compound kinematic target used center of mass");
}

TEST_CASE("A compound's automatic center of mass is weighted by child volume") {
    World world(weightless(8));
    BodySettings settings;
    settings.collider.shape = Shape::compound;
    ColliderChild small, large;
    large.pose.position = {3, 0, 0};
    large.collider.half_extent = {1, .5F, .5F}; // Twice the volume of the default unit box.
    settings.collider.children = {small, large};
    const auto body = world.create(settings);
    REQUIRE_MESSAGE(near(body.local_center_of_mass(), {2, 0, 0}),
                    "Automatic compound center used equal child weights instead of volume");
}

TEST_CASE("Contacts with several compound children aggregate into one body pair") {
    World world(weightless(8));
    BodySettings settings;
    settings.sensor = true;
    settings.collider.shape = Shape::compound;
    ColliderChild left, right;
    left.pose.position = {-1, 0, 0};
    right.pose.position = {1, 0, 0};
    settings.collider.children = {left, right};
    [[maybe_unused]] const auto sensor = world.create(settings);
    auto moving = world.create(box({}, {1.5F, .5F, .5F}, Motion::dynamic));
    world.step(tick);
    auto events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE_MESSAGE((events[0].sensor && events[0].phase == ContactPhase::begin),
                    "Two compound child contacts did not aggregate into one begin");
    moving.teleport({{-1.75F, 0, 0}});
    world.step(tick);
    REQUIRE_MESSAGE(world.take_events().empty(), "Leaving one compound child invented a body-pair exit");
    moving.teleport({{-5, 0, 0}});
    world.step(tick);
    events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE_MESSAGE(events[0].phase == ContactPhase::end, "Leaving the final compound child did not emit one end");
}

TEST_CASE("Dynamic hulls and compounds settle on a floor") {
    World world;
    [[maybe_unused]] const auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
    BodySettings settings;
    settings.motion = Motion::dynamic;
    settings.collider = tetrahedron();
    settings.pose.position = {5, 3, 0};
    auto hull = world.create(settings);
    settings.collider = {};
    settings.collider.shape = Shape::compound;
    ColliderChild left, right;
    left.pose.position = {-1, 0, 0};
    right.pose.position = {1, 0, 0};
    settings.collider.children = {left, right};
    settings.pose.position = {0, 3, 0};
    auto compound = world.create(settings);
    for (int i = 0; i < settle_ticks; ++i)
        world.step(tick);
    const auto hull_height = hull.world_center_of_mass().y;
    REQUIRE_MESSAGE((hull_height > 0 && hull_height < .4F), "Dynamic asymmetric hull did not settle on floor");
    REQUIRE_MESSAGE(std::abs(compound.pose().position.y - resting_height) < settle_tolerance,
                    "Dynamic compound did not settle at its authored-origin height");
}

TEST_CASE("An explicit center of mass changes dynamics without moving geometry") {
    for (const bool assembly : {false, true}) {
        CAPTURE(assembly);
        World automatic(weightless(4)), authored(weightless(4));
        BodySettings settings;
        settings.collider = tetrahedron();
        if (assembly) {
            auto leaf = settings.collider;
            settings.collider = {};
            settings.collider.shape = Shape::compound;
            settings.collider.children.push_back({{{2, 0, 0}}, leaf});
        }
        settings.pose.position = {3, 0, 0};
        settings.motion = Motion::dynamic;
        settings.mass = 5;
        [[maybe_unused]] const auto original = automatic.create(settings);
        const Vec3 local_center{0, -2, 0};
        settings.center_of_mass = local_center;
        auto shifted = authored.create(settings);
        REQUIRE_MESSAGE((near(shifted.pose().position, {3, 0, 0}) &&
                         near(shifted.local_center_of_mass(), local_center) &&
                         near(shifted.world_center_of_mass(), {3, -2, 0})),
                        "Explicit local center of mass was treated as relative offset");
        const Vec3 ray_origin{-5, .2F, .2F}, ray_span{20, 0, 0};
        const auto ray = automatic.raycast(ray_origin, ray_span);
        const auto shifted_ray = authored.raycast(ray_origin, ray_span);
        REQUIRE_MESSAGE(
            (ray && shifted_ray && near(ray->point, shifted_ray->point) && near(ray->normal, shifted_ray->normal)),
            "Center of mass override moved ray geometry");
        Collider sphere;
        sphere.shape = Shape::sphere;
        sphere.radius = .1F;
        const auto cast = automatic.sweep(sphere, {ray_origin}, ray_span);
        const auto shifted_cast = authored.sweep(sphere, {ray_origin}, ray_span);
        REQUIRE_MESSAGE((cast && shifted_cast && std::abs(cast->fraction - shifted_cast->fraction) < surface_tolerance),
                        "Center of mass override moved swept geometry");
        const float inside_x = assembly ? 5.1F : 3.1F; // Just inside the hull's origin vertex.
        REQUIRE_MESSAGE(!authored.overlap(sphere, {{inside_x, .1F, .1F}}).empty(),
                        "Center of mass override moved overlap geometry");
        shifted.add_impulse({5, 0, 0});
        REQUIRE_MESSAGE(near(shifted.velocity(), {1, 0, 0}), "Explicit center of mass changed total mass");
        shifted.set_velocity({});
        shifted.set_angular_velocity({0, 0, 2});
        authored.step(long_step);
        REQUIRE_MESSAGE((near(shifted.world_center_of_mass(), {3, -2, 0}) &&
                         !near(shifted.pose().position, settings.pose.position)),
                        "Body did not rotate about explicit center of mass");
        // A quarter turn about +Z carries the local center (0, -2, 0) to (2, 0, 0).
        shifted.teleport({{5, 6, 7}, quarter_turn_z});
        REQUIRE_MESSAGE((near(shifted.pose().position, {5, 6, 7}) && near(shifted.world_center_of_mass(), {7, 6, 7})),
                        "Teleport did not preserve explicit local center of mass");
        shifted.set_enabled(false);
        REQUIRE_MESSAGE(near(shifted.local_center_of_mass(), local_center),
                        "Disabled body lost explicit center of mass");
        shifted.remove();
        REQUIRE_THROWS_WITH_AS(shifted.local_center_of_mass(), expired_body, std::out_of_range);
        REQUIRE_THROWS_WITH_AS(shifted.world_center_of_mass(), expired_body, std::out_of_range);
        settings.motion = Motion::kinematic;
        auto moving = authored.create(settings);
        moving.move_kinematic({{3, 4, 5}, quarter_turn_z}, long_step);
        authored.step(long_step);
        REQUIRE_MESSAGE((near(moving.pose().position, {3, 4, 5}) && near(moving.world_center_of_mass(), {5, 4, 5})),
                        "Kinematic target lost explicit center of mass");
    }
}

TEST_CASE("Hull and compound queries use their authored origins") {
    World world(weightless(16));
    const auto wall = world.create(box({5, 0, 0}, {.5F, 10, 10}));
    const auto hull = tetrahedron();
    // The hull spans x in [0, 2] and meets the wall face at x = 4.5 after 2.5 of 10 meters.
    auto cast = world.sweep(hull, {}, {10, 0, 0});
    REQUIRE_MESSAGE(
        (cast && cast->body == wall && std::abs(cast->fraction - .25F) < tolerance && cast->normal.x < -axis_alignment),
        "Hull sweep did not transform its authored origin to center of mass");
    REQUIRE_MESSAGE(world.overlap(hull, {{2, 0, 0}}).empty(), "Hull overlap moved geometry toward its center of mass");
    REQUIRE_MESSAGE(!world.overlap(hull, {{3, 0, 0}}).empty(), "Offset hull overlap missed wall");
    Collider compound;
    compound.shape = Shape::compound;
    ColliderChild part;
    part.pose.position = {2, 0, 0};
    part.collider.half_extent = {1, .25F, .5F};
    part.pose.rotation = quarter_turn_z;
    compound.children.push_back(part);
    cast = world.sweep(compound, {}, {10, 0, 0});
    REQUIRE_MESSAGE((cast && std::abs(cast->fraction - .225F) < tolerance),
                    "Rotated offset child sweep lost local origin or rotation");
    REQUIRE_MESSAGE(world.overlap(compound, {{2, 0, 0}}).empty(), "Compound overlap ignored child rotation");
    REQUIRE_MESSAGE(!world.overlap(compound, {{2.5F, 0, 0}}).empty(), "Compound overlap missed offset child");
    compound.children[0].collider = hull;
    compound.children[0].pose.rotation = identity_rotation;
    cast = world.sweep(compound, {}, {10, 0, 0});
    REQUIRE_MESSAGE((cast && std::abs(cast->fraction - .05F) < tolerance),
                    "Compound hull child applied center of mass twice");
    const auto initial = world.sweep(compound, {{1, 0, 0}}, {10, 0, 0});
    REQUIRE_MESSAGE((initial && initial->fraction == 0 && initial->penetration > 0),
                    "Compound initial overlap missing");
}

TEST_CASE("Invalid colliders and centers of mass are rejected without leaking bodies") {
    World world;
    auto hull = tetrahedron();
    hull.vertices[3] = {1, 1, 0};
    rejects_collider<std::invalid_argument>(world, hull, thin_hull);
    hull.vertices = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    rejects_collider<std::invalid_argument>(world, hull, "Hull points are collinear or too thin");
    hull.vertices.assign(4, {});
    rejects_collider<std::invalid_argument>(world, hull, "Hull points do not span a line");
    hull = tetrahedron();
    hull.vertices[3].z = .00001F; // Flatter than the minimum hull thickness.
    rejects_collider<std::invalid_argument>(world, hull, thin_hull);
    hull = tetrahedron();
    hull.vertices[0].x = std::numeric_limits<float>::infinity();
    rejects_collider<std::invalid_argument>(world, hull, vector_range);
    hull = tetrahedron();
    hull.vertices.resize(hull_point_limit + 1);
    rejects_collider<std::invalid_argument>(world, hull, "Hull requires 4..256 points");
    hull = tetrahedron();
    hull.indices = {0, 1, 2};
    rejects_collider<std::invalid_argument>(world, hull, "Hull collider cannot contain triangle indices");
    hull = tetrahedron();
    hull.children.resize(1);
    rejects_collider<std::invalid_argument>(world, hull, stray_children);
    Collider compound;
    compound.shape = Shape::compound;
    rejects_collider<std::invalid_argument>(world, compound, compound_count);
    compound.children.resize(compound_child_limit + 1);
    rejects_collider<std::invalid_argument>(world, compound, compound_count);
    compound.children.resize(1);
    compound.children[0].pose.rotation = {0, 0, 0, 0};
    rejects_collider<MathError>(world, compound, math_error_message(MathErrorCode::zero_quaternion));
    compound.children[0].pose = {};
    compound.children[0].pose.position.x = beyond_vector_range;
    rejects_collider<std::invalid_argument>(world, compound, vector_range);
    compound.children[0].pose = {};
    compound.children[0].collider.shape = Shape::mesh;
    rejects_collider<std::invalid_argument>(world, compound, compound_child);
    compound.children[0].collider.shape = Shape::compound;
    compound.children[0].collider.children.resize(compound_child_limit);
    rejects_collider<std::invalid_argument>(world, compound, compound_child);
    Collider primitive;
    primitive.children.push_back({{}, compound});
    rejects_collider<std::invalid_argument>(world, primitive, stray_children);
    for (const auto center : {Vec3{beyond_vector_range, 0, 0}, Vec3{0, std::numeric_limits<float>::infinity(), 0}}) {
        BodySettings settings;
        settings.center_of_mass = center;
        REQUIRE_THROWS_WITH_AS(world.create(settings), vector_range, std::invalid_argument);
        REQUIRE(world.size() == 0u);
    }
    BodySettings mesh;
    mesh.collider.shape = Shape::mesh;
    mesh.collider.vertices = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};
    mesh.collider.indices = {0, 1, 2};
    mesh.center_of_mass = Vec3{};
    REQUIRE_THROWS_WITH_AS(world.create(mesh), "Triangle meshes have no authored center of mass override",
                           std::invalid_argument);
    REQUIRE(world.size() == 0u);
}

TEST_CASE("Body lifetime, contacts and argument validation") {
    Body stale;
    {
        World world;
        auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20})); // Mutated below to test rejection.
        auto falling = world.create(box({0, 4, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        stale = falling;
        for (int i = 0; i < settle_ticks; ++i)
            world.step(tick);
        REQUIRE_MESSAGE(std::abs(falling.pose().position.y - resting_height) < settle_tolerance,
                        "Dynamic body did not settle on floor");
        auto events = world.take_events();
        REQUIRE(events.size() == 1u);
        REQUIRE_MESSAGE((events[0].phase == ContactPhase::begin && !events[0].sensor), "Contact begin not aggregated");
        falling.remove();
        events = world.take_events();
        REQUIRE(events.size() == 1u);
        REQUIRE_MESSAGE(events[0].phase == ContactPhase::end, "Removal did not end contact");
        REQUIRE_MESSAGE((!stale.valid() && floor.valid()), "Body lifetime corrupted");
        auto replacement = world.create(box({0, 4, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        REQUIRE_MESSAGE(replacement != stale, "Reused body slot revived stale identity");
        REQUIRE_THROWS_WITH_AS(stale.pose(), expired_body, std::out_of_range);
        stale.remove();
        replacement.set_enabled(false);
        world.step(tick);
        REQUIRE_MESSAGE(!world.raycast({0, 10, 0}, {0, -8, 0}), "Disabled body appears in queries");
        replacement.set_enabled(true);
        const auto hit = world.raycast({0, 10, 0}, {0, -8, 0});
        REQUIRE_MESSAGE((hit && hit->body == replacement), "Reenabled body missing");
        replacement.add_impulse({0, 2, 0});
        REQUIRE_MESSAGE(replacement.velocity().y > 0, "Impulse not applied");
        REQUIRE_THROWS_WITH_AS(world.step(0), step_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(world.step(1), step_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(world.raycast({}, {0, 0, 0}), "Zero ray displacement", std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(floor.set_velocity({1, 0, 0}), "Static bodies cannot have velocity",
                               std::invalid_argument);
        auto bad = box({}, {0, 1, 1});
        REQUIRE_THROWS_WITH_AS(world.create(bad), "Invalid collider dimensions", std::invalid_argument);
        bad = box({}, {1, 1, 1});
        bad.mass = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_THROWS_WITH_AS(world.create(bad), "Invalid body mass/material", std::invalid_argument);
        stale = replacement;
        World other;
        REQUIRE_MESSAGE(!other.owns(replacement), "Foreign world accepted body");
        QueryFilter foreign;
        foreign.ignore = replacement;
        REQUIRE_THROWS_WITH_AS(other.raycast({0, 1, 0}, {0, -2, 0}, foreign), "Foreign query body",
                               std::invalid_argument);
    }
    REQUIRE_MESSAGE(!stale.valid(), "World teardown retained body");
    stale.remove();
}

TEST_CASE("Mesh decks support rays, sweeps and overlaps from above and below") {
    // A bridge deck with an upper surface, an underside and a seam, without heightfield assumptions.
    constexpr float deck_height = 3;
    World world;
    BodySettings mesh;
    mesh.collider.shape = Shape::mesh;
    mesh.collider.vertices = {{-5, deck_height, -5}, {5, deck_height, -5}, {5, deck_height, 5}, {-5, deck_height, 5}};
    mesh.collider.indices = {0, 2, 1, 0, 3, 2};
    const auto deck = world.create(mesh);
    const auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
    auto ray = world.raycast({0, 10, 0}, {0, -20, 0});
    REQUIRE_MESSAGE((ray && ray->body == deck && std::abs(ray->point.y - deck_height) < surface_tolerance &&
                     ray->normal.y > axis_alignment),
                    "Deck ray or normal failed");
    ray = world.raycast({0, 1, 0}, {0, 4, 0});
    REQUIRE_MESSAGE((ray && ray->body == deck), "Bridge underside missing");
    REQUIRE_MESSAGE(ray->normal.y < -axis_alignment, "Underside ray normal must face the ray, as the sweep's does");
    Collider sphere;
    sphere.shape = Shape::sphere;
    sphere.radius = .5F;
    // The sphere's lowest point meets the deck after 100 - 3 - 0.5 = 96.5 of 200 meters.
    auto sweep = world.sweep(sphere, {{0, 100, 0}}, {0, -200, 0});
    REQUIRE_MESSAGE((sweep && sweep->body == deck && std::abs(sweep->fraction - .4825F) < surface_tolerance &&
                     sweep->normal.y > axis_alignment),
                    "Large swept sphere crossed deck/seam");
    sweep = world.sweep(sphere, {{0, 1, 0}}, {0, 4, 0});
    REQUIRE_MESSAGE((sweep && sweep->body == deck && sweep->normal.y < -axis_alignment),
                    "Swept underside normal failed");
    const auto overlaps = world.overlap(sphere, {{0, .25F, 0}});
    REQUIRE_MESSAGE((!overlaps.empty() && overlaps.front().body == floor && overlaps.front().penetration > .2F),
                    "Clearance overlap failed");
    sphere.shape = Shape::capsule;
    sweep = world.sweep(sphere, {{0, 5, 0}}, {0, -5, 0});
    REQUIRE_MESSAGE((sweep && sweep->body == deck), "Capsule sweep failed");
    mesh.collider.indices = {0, 0, 1};
    REQUIRE_THROWS_WITH_AS(world.create(mesh), "Degenerate collider triangle", std::invalid_argument);
    mesh.collider.indices = {0, 1, 99};
    REQUIRE_THROWS_WITH_AS(world.create(mesh), "Mesh index out of range", std::invalid_argument);
    mesh.collider.indices = {0, 2, 1};
    mesh.motion = Motion::dynamic;
    REQUIRE_THROWS_WITH_AS(world.create(mesh), "Mesh bodies must be stationary", std::invalid_argument);
}

TEST_CASE("Sensors, collision layers and query filters") {
    World world(weightless(8));
    world.set_layer_collision(0, 1, false);
    auto sensor_settings = box({}, {2, 2, 2});
    sensor_settings.sensor = true;
    sensor_settings.layer = 2;
    const auto sensor = world.create(sensor_settings);
    auto moving = world.create(box({}, {.2F, .2F, .2F}, Motion::dynamic));
    auto ignored_settings = box({}, {1, 1, 1});
    ignored_settings.layer = 1;
    [[maybe_unused]] const auto ignored = world.create(ignored_settings);
    world.step(tick);
    auto events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE_MESSAGE((events[0].sensor && events[0].phase == ContactPhase::begin), "Sensor/layer filtering failed");
    QueryFilter filter;
    filter.layers = static_cast<std::uint16_t>(1u << sensor_settings.layer);
    REQUIRE_MESSAGE(!world.raycast({0, 3, 0}, {0, -6, 0}, filter), "Sensors included by default");
    filter.sensors = true;
    const auto hit = world.raycast({0, 3, 0}, {0, -6, 0}, filter);
    REQUIRE_MESSAGE((hit && hit->body == sensor), "Sensor query missing");
    moving.teleport({{0, 10, 0}});
    world.step(tick);
    events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE_MESSAGE(events[0].phase == ContactPhase::end, "Sensor exit missing");
    auto kinematic = world.create(box({10, 0, 0}, {.5F, .5F, .5F}, Motion::kinematic));
    const float target_x = 11;
    kinematic.move_kinematic({{target_x, 0, 0}}, long_step);
    world.step(long_step);
    REQUIRE_MESSAGE(std::abs(kinematic.pose().position.x - target_x) < surface_tolerance, "Kinematic target failed");
}

TEST_CASE("Tangent rays, initial overlaps and continuous collision") {
    World world(weightless(16));
    BodySettings sphere;
    sphere.collider.shape = Shape::sphere;
    sphere.collider.radius = 1;
    auto target = world.create(sphere);
    const auto tangent = world.raycast({-2, 1, 0}, {4, 0, 0});
    REQUIRE_MESSAGE((tangent && tangent->body == target && std::abs(tangent->fraction - .5F) < surface_tolerance),
                    "Tangent ray missed sphere");
    const auto inside = world.raycast({0, 0, 0}, {4, 0, 0});
    REQUIRE_MESSAGE((inside && inside->fraction == 0), "Ray starting inside did not report initial overlap");
    Collider query;
    query.shape = Shape::sphere;
    query.radius = .5F;
    const auto initial = world.sweep(query, {{0, .25F, 0}}, {0, 5, 0});
    REQUIRE_MESSAGE((initial && initial->fraction == 0 && initial->penetration > 0),
                    "Sweep initial penetration missing");
    target.remove();
    [[maybe_unused]] const auto wall = world.create(box({0, 0, 0}, {.05F, 5, 5}));
    sphere.motion = Motion::dynamic;
    sphere.continuous = true;
    sphere.collider.radius = .2F;
    sphere.pose.position = {-5, 0, 0};
    sphere.velocity = {100, 0, 0}; // Ten meters per 0.1 s step, far beyond the wall's thickness.
    auto fast = world.create(sphere);
    world.step(long_step);
    REQUIRE_MESSAGE(fast.pose().position.x < 0, "Continuous body tunneled through thin wall");
    fast.set_enabled(false);
    fast.teleport({{-4, 0, 0}});
    fast.set_velocity({});
    fast.set_angular_velocity({0, 1, 0});
    fast.set_enabled(true);
    world.step(tick);
    REQUIRE_MESSAGE((fast.angular_velocity().y > 0 && std::abs(fast.pose().rotation[1]) > 0),
                    "Angular state did not advance");
}

TEST_CASE("Body capacity is enforced across repeated world lifetimes") {
    constexpr int lifetimes = 3;
    for (int i = 0; i < lifetimes; ++i) {
        World world(weightless(1));
        auto body = world.create({});
        REQUIRE_THROWS_WITH_AS(world.create({}), "Physics body capacity exhausted", std::length_error);
        body.remove();
    }
}

TEST_CASE("Disabled bodies reject impulses") {
    // Disabled bodies are outside Jolt's broadphase; an impulse must not reactivate them.
    constexpr auto enabled_only = "Only enabled dynamic bodies accept impulses";
    constexpr int ticks_after_rejection = 3;
    World world;
    const auto floor = world.create(box({0, -.5F, 0}, {5, .5F, 5}));
    auto crate = world.create(box({0, .4F, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    auto parked = world.create(box({3, .4F, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    world.step(tick);
    crate.set_enabled(false);
    parked.set_enabled(false);
    REQUIRE_THROWS_WITH_AS(crate.add_impulse({0, 0, 0}), enabled_only, std::invalid_argument);
    REQUIRE_THROWS_WITH_AS(parked.add_impulse({0, 1, 0}), enabled_only, std::invalid_argument);
    parked.remove();
    for (int i = 0; i < ticks_after_rejection; ++i)
        world.step(tick);
    const auto hit = world.raycast({0, 10, 0}, {0, -12, 0});
    REQUIRE_MESSAGE(!crate.enabled(), "Rejected impulse reenabled a body");
    REQUIRE_MESSAGE((hit && hit->body == floor), "Rejected impulse reactivated a disabled body");
    crate.set_enabled(true);
    crate.add_impulse({0, 2, 0});
    REQUIRE_MESSAGE(crate.velocity().y > 0, "Reenabled body did not accept an impulse");
}

TEST_CASE("Reenabling a body before the next step reports its contact again") {
    World world;
    (void)world.create(box({0, -.5F, 0}, {20, .5F, 20}));
    auto resting = world.create(box({0, 4, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    for (int i = 0; i < settle_ticks; ++i)
        world.step(tick);
    auto events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE(events[0].phase == ContactPhase::begin);
    resting.set_enabled(false);
    events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE(events[0].phase == ContactPhase::end);
    resting.set_enabled(true);
    world.step(tick);
    events = world.take_events();
    REQUIRE_MESSAGE(events.size() == 1u, "Reenabled contact reported no begin event");
    REQUIRE(events[0].phase == ContactPhase::begin);
}

TEST_CASE("Disabling a body keeps its velocities") {
    World world(weightless(1));
    auto moving = world.create(box({}, {.5F, .5F, .5F}, Motion::dynamic));
    const Vec3 velocity{1, 2, 3};
    const Vec3 spin{.5F, 0, 0};
    moving.set_velocity(velocity);
    moving.set_angular_velocity(spin);
    moving.set_enabled(false);
    REQUIRE_MESSAGE(near(moving.velocity(), velocity), "Disabling cleared the linear velocity");
    moving.set_enabled(true);
    REQUIRE_MESSAGE((near(moving.velocity(), velocity) && near(moving.angular_velocity(), spin)),
                    "Reenabling lost the saved velocities");
}

TEST_CASE("Kinematic bodies pair with stationary bodies only as sensors") {
    // The stationary box spans x in [-1, 1]; each kinematic box overlaps it from one side only.
    World world(weightless(5));
    (void)world.create(box({}, {1, 1, 1}));
    (void)world.create(box({-.9F, 0, 0}, {.5F, .5F, .5F}, Motion::kinematic));
    world.step(tick);
    REQUIRE_MESSAGE(world.take_events().empty(), "Kinematic body reported a contact with a stationary body");
    auto sensor = box({.9F, 0, 0}, {.5F, .5F, .5F}, Motion::kinematic);
    sensor.sensor = true;
    (void)world.create(sensor);
    world.step(tick);
    const auto events = world.take_events();
    REQUIRE(events.size() == 1u);
    REQUIRE_MESSAGE((events[0].sensor && events[0].phase == ContactPhase::begin),
                    "Kinematic sensor missed a stationary body");
    // A stationary trigger still detects a kinematic body, well away from the boxes above.
    auto trigger = box({0, 10, 0}, {1, 1, 1});
    trigger.sensor = true;
    (void)world.create(trigger);
    (void)world.create(box({0, 10, 0}, {.5F, .5F, .5F}, Motion::kinematic));
    world.step(tick);
    const auto triggered = world.take_events();
    REQUIRE(triggered.size() == 1u);
    REQUIRE_MESSAGE((triggered[0].sensor && triggered[0].phase == ContactPhase::begin),
                    "Stationary trigger missed a kinematic body");
}

TEST_CASE("A disabled body stores pose and velocity writes until it is reenabled") {
    World world(weightless(1));
    auto body = world.create(box({}, {.5F, .5F, .5F}, Motion::dynamic));
    body.set_enabled(false);
    const Vec3 position{1, 2, 3};
    const Vec3 velocity{1, 0, 0};
    const Vec3 spin{0, 1, 0};
    body.teleport({position});
    body.set_velocity(velocity);
    body.set_angular_velocity(spin);
    REQUIRE_MESSAGE((near(body.pose().position, position) && near(body.velocity(), velocity) &&
                     near(body.angular_velocity(), spin)),
                    "Disabled body lost a write");
    body.set_enabled(true);
    REQUIRE_MESSAGE((near(body.velocity(), velocity) && near(body.angular_velocity(), spin)),
                    "Reenabled body lost its stored velocities");
}
