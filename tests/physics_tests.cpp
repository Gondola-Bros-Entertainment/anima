#include <anima/physics.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

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
constexpr float vector_limit = 1e6F; // Physics vector components are limited to +/-1e6.
constexpr float beyond_vector_range = 2 * vector_limit;
// Velocity caps, documented in include/anima/physics.hpp: units or radians per second.
constexpr float dynamic_speed_cap = 500;
constexpr float dynamic_angular_speed_cap = 15 * std::numbers::pi_v<float>;
constexpr float kinematic_speed_cap = 2 * vector_limit; // Beyond the length of any valid vector.
constexpr float cap_tolerance = 1e-5F;                  // Relative error of a velocity clamped to its cap.
constexpr auto expired_body = "Expired physics body";
constexpr auto vector_range = "Physics vector outside finite supported range";
constexpr auto step_range = "Physics step must be in [0.000001, 0.1] seconds";
constexpr auto thin_hull = "Hull points are coplanar or too thin";
constexpr auto compound_count = "Compound requires 1..64 children";
constexpr auto compound_child = "Compound children must be primitives or hulls";
constexpr auto stray_children = "Noncompound collider contains children";

bool near(Vec3 a, Vec3 b) { return length(a - b) < tolerance; }
bool identical(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
// Whether @p velocity points along the unit @p direction with the length @p cap.
bool at_cap(Vec3 velocity, Vec3 direction, float cap) {
    return length(velocity - direction * cap) <= cap * cap_tolerance;
}
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
// A box's principal moments of inertia about its center: m / 3 (h_y^2 + h_z^2) about X, and so on.
Vec3 box_inertia(float mass, Vec3 half) {
    return Vec3{half.y * half.y + half.z * half.z, half.x * half.x + half.z * half.z,
                half.x * half.x + half.y * half.y} *
           (mass / 3);
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

TEST_CASE("Collider factories build bodies of their shapes and leave other fields at their defaults") {
    const Collider defaults;
    const auto keeps_defaults = [&](const Collider &c, bool extent, bool radius, bool height) {
        return (!extent || identical(c.half_extent, defaults.half_extent)) &&
               (!radius || c.radius == defaults.radius) && (!height || c.half_height == defaults.half_height);
    };
    const auto box_collider = Collider::box({1, .25F, .5F});
    CHECK((box_collider.shape == Shape::box && identical(box_collider.half_extent, {1, .25F, .5F}) &&
           keeps_defaults(box_collider, false, true, true) && box_collider.vertices.empty()));
    const auto sphere = Collider::sphere(.25F);
    CHECK((sphere.shape == Shape::sphere && sphere.radius == .25F && keeps_defaults(sphere, true, false, true)));
    const auto capsule = Collider::capsule(.25F, 1);
    CHECK((capsule.shape == Shape::capsule && capsule.radius == .25F && capsule.half_height == 1 &&
           keeps_defaults(capsule, true, false, false)));
    const auto hull = Collider::convex_hull(tetrahedron().vertices);
    CHECK((hull.shape == Shape::convex_hull && hull.vertices.size() == 4u && hull.indices.empty() &&
           keeps_defaults(hull, true, true, true)));
    // One triangle facing +Y over x, z >= 0 with x + z <= 1.
    const auto mesh = Collider::mesh({{0, 0, 0}, {0, 0, 1}, {1, 0, 0}}, {0, 1, 2});
    CHECK((mesh.shape == Shape::mesh && mesh.vertices.size() == 3u && mesh.indices.size() == 3u &&
           keeps_defaults(mesh, true, true, true)));
    ColliderChild left, right;
    left.pose.position = {-1, 0, 0};
    left.collider = Collider::box({.25F, .25F, .25F});
    right.pose.position = {1, 0, 0};
    right.collider = left.collider;
    const auto compound = Collider::compound({left, right});
    CHECK((compound.shape == Shape::compound && compound.children.size() == 2u && compound.vertices.empty() &&
           keeps_defaults(compound, true, true, true)));

    World world(weightless(16));
    // Each body stands at x = 10 i; a vertical ray from y = 5 finds its top surface, or nothing.
    const auto top = [&](float x, float z) -> std::optional<float> {
        const auto hit = world.raycast({x, 5, z}, {0, -10, 0});
        if (!hit)
            return std::nullopt;
        return hit->point.y;
    };
    const auto at = [&](const Collider &collider, float x) {
        BodySettings settings;
        settings.collider = collider;
        settings.pose.position = {x, 0, 0};
        return world.create(settings);
    };
    const auto on = [](std::optional<float> y, float expected) {
        return y && std::abs(*y - expected) < surface_tolerance;
    };
    (void)at(box_collider, 0);
    CHECK_MESSAGE((on(top(.9F, .4F), .25F) && !top(1.1F, 0)), "Box factory built the wrong extent");
    (void)at(sphere, 10);
    // A box of half size 0.25 would cover (0.2, 0.2); the sphere leaves it open.
    CHECK_MESSAGE((on(top(10, 0), .25F) && !top(10.2F, .2F)), "Sphere factory built the wrong shape");
    (void)at(capsule, 20);
    CHECK_MESSAGE((on(top(20, 0), 1.25F) && on(top(20.2F, 0), 1.15F) && !top(20.2F, .2F)),
                  "Capsule factory built the wrong shape");
    (void)at(hull, 30);
    // The tetrahedron's slanted face is x / 2 + y + z = 1.
    CHECK_MESSAGE((on(top(30.25F, .25F), .625F) && !top(31.5F, .5F)), "Hull factory built the wrong shape");
    (void)at(mesh, 40);
    CHECK_MESSAGE((on(top(40.25F, .25F), 0) && !top(40.75F, .75F)), "Mesh factory built the wrong shape");
    (void)at(compound, 50);
    CHECK_MESSAGE((on(top(49, 0), .25F) && !top(50, 0) && on(top(51, 0), .25F)),
                  "Compound factory misplaced its children");
    CHECK(world.size() == 6u);
    // The factories validate nothing; World::create rejects their invalid geometry.
    World empty(weightless(1));
    rejects_collider<std::invalid_argument>(empty, Collider::sphere(0), "Invalid collider dimensions");
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

TEST_CASE("A sensor pair is reported only when either body can move") {
    World world(weightless(8));
    auto zone_settings = box({}, {1, 1, 1});
    zone_settings.sensor = true;
    const auto zone = world.create(zone_settings);
    const auto wall = world.create(box({}, {1, 1, 1}));
    auto probe_settings = box({}, {.2F, .2F, .2F}, Motion::dynamic);
    probe_settings.sensor = true;
    const auto probe = world.create(probe_settings);
    world.step(tick);
    const auto events = world.take_events();
    const auto reported = [&](const Body &a, const Body &b) {
        return std::ranges::any_of(events, [&](const ContactEvent &e) {
            return e.sensor && e.phase == ContactPhase::begin &&
                   ((e.first == a && e.second == b) || (e.first == b && e.second == a));
        });
    };
    CHECK_MESSAGE(reported(probe, zone), "A dynamic sensor did not detect a stationary sensor");
    CHECK_MESSAGE(reported(probe, wall), "A dynamic sensor did not detect a stationary body");
    CHECK_MESSAGE(!reported(zone, wall), "Two stationary bodies reported a sensor overlap");
    CHECK(events.size() == 2u);
    for (const auto &event : events)
        CHECK_MESSAGE(!event.contact, "A sensor overlap reported a contact point");
}

TEST_CASE("A long step does not let a discrete body pass through a floor") {
    // Covering one tick's distance per collision step, the body meets the floor during a 0.1 s step; a single
    // collision step at the start of the step would see it 0.25 m away and move it straight through.
    constexpr float speed = 12;
    constexpr float floor_half_thickness = .25F;
    constexpr float start_height = .6F;
    World world(weightless(4));
    (void)world.create(box({}, {5, floor_half_thickness, 5}));
    auto body = world.create(box({0, start_height, 0}, {.1F, .1F, .1F}, Motion::dynamic));
    body.set_velocity({0, -speed, 0});
    world.step(long_step);
    INFO("height after one long step: ", body.pose().position.y);
    CHECK(body.pose().position.y > 0);
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
    REQUIRE_MESSAGE((!tangent->initial_overlap && tangent->penetration == 0), "Tangent ray reported an overlap");
    const auto inside = world.raycast({0, 0, 0}, {4, 0, 0});
    REQUIRE_MESSAGE((inside && inside->body == target && inside->fraction == 0 && inside->initial_overlap),
                    "Ray starting inside did not report initial overlap");
    Collider query;
    query.shape = Shape::sphere;
    query.radius = .5F;
    const auto initial = world.sweep(query, {{0, .25F, 0}}, {0, 5, 0});
    REQUIRE_MESSAGE((initial && initial->fraction == 0 && initial->penetration > 0 && initial->initial_overlap),
                    "Sweep initial penetration missing");
    // The query sphere starts 0.01 meters below the target and meets it after 0.01 of 5 meters.
    const auto outside = world.sweep(query, {{0, -1.51F, 0}}, {0, 5, 0});
    REQUIRE_MESSAGE((outside && outside->body == target && std::abs(outside->fraction - .002F) < surface_tolerance &&
                     !outside->initial_overlap && outside->penetration == 0),
                    "Sweep starting outside reported an initial overlap");
    const auto overlaps = world.overlap(query, {{0, .25F, 0}});
    REQUIRE_MESSAGE((overlaps.size() == 1u && overlaps.front().initial_overlap), "Overlap result not flagged");
    World other;
    QueryFilter foreign;
    foreign.ignore = target;
    REQUIRE_THROWS_WITH_AS(other.sweep(query, {}, {1, 0, 0}, foreign), "Foreign query body", std::invalid_argument);
    REQUIRE_THROWS_WITH_AS(other.overlap(query, {}, foreign), "Foreign query body", std::invalid_argument);
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
    REQUIRE_MESSAGE(events[0].contact, "A reenabled contact's begin event has no contact point");
    CHECK(near(events[0].contact->normal, {0, 1, 0}));
}

TEST_CASE("A solid contact's begin event reports its point, normal and approach speed") {
    constexpr float impact_speed = 5; // Without gravity the crate meets the floor at its launch speed.
    // Creating the crate first names it first. Jolt reuses the body slot freed last first, so after two removals the
    // crate takes the higher slot and the floor the lower one, and Jolt reports the pair in the opposite order to the
    // event, whose normal must then be reversed.
    for (const int order : {0, 1, 2}) {
        CAPTURE(order);
        World world(weightless(8));
        if (order == 2) {
            auto low_slot = world.create(box({10, 0, 0}, {.5F, .5F, .5F}));
            auto high_slot = world.create(box({20, 0, 0}, {.5F, .5F, .5F}));
            low_slot.remove();
            high_slot.remove();
        }
        const auto floor_settings = box({0, -.5F, 0}, {20, .5F, 20});
        auto crate_settings = box({0, 1, 0}, {.5F, .5F, .5F}, Motion::dynamic);
        crate_settings.velocity = {0, -impact_speed, 0};
        Body floor, crate;
        if (order == 0) {
            floor = world.create(floor_settings);
            crate = world.create(crate_settings);
        } else {
            crate = world.create(crate_settings);
            floor = world.create(floor_settings);
        }
        std::vector<ContactEvent> events;
        for (int i = 0; i < settle_ticks && events.empty(); ++i) {
            world.step(tick);
            events = world.take_events();
        }
        REQUIRE(events.size() == 1u);
        const auto &begin = events[0];
        REQUIRE((begin.phase == ContactPhase::begin && !begin.sensor));
        REQUIRE_MESSAGE(begin.contact, "A solid begin event has no contact point");
        const bool floor_first = begin.first == floor;
        CHECK(floor_first == (order == 0));
        CHECK_MESSAGE(near(begin.contact->normal, floor_first ? Vec3{0, 1, 0} : Vec3{0, -1, 0}),
                      "The normal does not point from the first body toward the second");
        // The crate lands flat, centered on the origin, and the surfaces were at most one step's travel apart.
        const auto point = begin.contact->point;
        CHECK(std::abs(point.x) < tolerance);
        CHECK(std::abs(point.z) < tolerance);
        CHECK(std::abs(point.y) <= impact_speed * tick);
        CHECK(std::abs(begin.contact->approach_speed - impact_speed) < tolerance);
        crate.teleport({{0, 10, 0}});
        world.step(tick);
        events = world.take_events();
        REQUIRE(events.size() == 1u);
        CHECK(events[0].phase == ContactPhase::end);
        CHECK_MESSAGE(!events[0].contact, "An end event reported a contact point");
    }
}

TEST_CASE("Removing or disabling a body ends only its own contacts, in body creation order") {
    World world;
    const auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
    auto crate = world.create(box({0, resting_height, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    auto zone_settings = box({0, resting_height, 0}, {1, 1, 1});
    zone_settings.sensor = true;
    const auto zone = world.create(zone_settings);
    auto other = world.create(box({5, resting_height, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    world.step(tick);
    REQUIRE(world.take_events().size() == 3u); // Floor and crate, crate and zone, floor and other.
    crate.remove();
    auto events = world.take_events();
    REQUIRE(events.size() == 2u);
    CHECK((events[0].first == floor && events[0].second == crate && events[0].phase == ContactPhase::end &&
           !events[0].sensor));
    CHECK((events[1].first == crate && events[1].second == zone && events[1].phase == ContactPhase::end &&
           events[1].sensor));
    world.step(tick);
    CHECK_MESSAGE(world.take_events().empty(), "A step after removal changed the remaining contacts");
    other.set_enabled(false);
    events = world.take_events();
    REQUIRE(events.size() == 1u);
    CHECK((events[0].first == floor && events[0].second == other && events[0].phase == ContactPhase::end &&
           !events[0].sensor));
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

TEST_CASE("A dynamic body's velocities are clamped to the caps") {
    // Each velocity exceeds its cap and must keep its direction, scaled to the cap's length.
    World world(weightless(1));
    auto settings = box({}, {.5F, .5F, .5F}, Motion::dynamic);
    settings.velocity = {600, 0, 0};
    settings.angular_velocity = {0, 94, 0};
    auto body = world.create(settings);
    REQUIRE_MESSAGE(at_cap(body.velocity(), {1, 0, 0}, dynamic_speed_cap), "Creation did not clamp the velocity");
    REQUIRE_MESSAGE(at_cap(body.angular_velocity(), {0, 1, 0}, dynamic_angular_speed_cap),
                    "Creation did not clamp the angular velocity");
    body.set_velocity({0, 0, -600});
    body.set_angular_velocity({-94, 0, 0});
    REQUIRE_MESSAGE(at_cap(body.velocity(), {0, 0, -1}, dynamic_speed_cap), "set_velocity did not clamp");
    REQUIRE_MESSAGE(at_cap(body.angular_velocity(), {-1, 0, 0}, dynamic_angular_speed_cap),
                    "set_angular_velocity did not clamp");
}

TEST_CASE("A kinematic body keeps velocities beyond the dynamic caps") {
    const Vec3 start{0, 10, 0};
    const Vec3 velocity{600, 0, 0};
    const Vec3 spin{0, 94, 0}; // About 1.57 radians per 1/60 s step, just under twice the dynamic cap.
    World world(weightless(1));
    auto settings = box(start, {.5F, .5F, .5F}, Motion::kinematic);
    settings.velocity = velocity;
    settings.angular_velocity = spin;
    auto body = world.create(settings);
    REQUIRE_MESSAGE((identical(body.velocity(), velocity) && identical(body.angular_velocity(), spin)),
                    "Creation clamped a kinematic body's velocities");
    world.step(tick);
    REQUIRE_MESSAGE((identical(body.velocity(), velocity) && identical(body.angular_velocity(), spin)),
                    "A step changed a kinematic body's velocities");
    const auto pose = body.pose();
    const auto turn = spin.y * static_cast<float>(tick);
    REQUIRE_MESSAGE(near(pose.position, start + velocity * static_cast<float>(tick)),
                    "A kinematic body did not move at its velocity");
    REQUIRE_MESSAGE((std::abs(pose.rotation[1] - std::sin(turn / 2)) < tolerance &&
                     std::abs(pose.rotation[3] - std::cos(turn / 2)) < tolerance),
                    "A kinematic body did not turn at its angular velocity");
    // The longest vector that passes validation is kept as well.
    const Vec3 fastest{vector_limit, vector_limit, -vector_limit};
    body.set_velocity(fastest);
    body.set_angular_velocity(fastest);
    REQUIRE_MESSAGE((identical(body.velocity(), fastest) && identical(body.angular_velocity(), fastest)),
                    "A kinematic body's longest valid velocities were clamped");
    // Only Body::move_kinematic exceeds the kinematic cap, which disabling the body then applies.
    constexpr double shortest_move = .000001;
    const Vec3 leap{20, 0, 0}; // Twenty million units per second over the shortest move.
    const auto here = body.pose();
    body.move_kinematic({here.position + leap, here.rotation}, shortest_move);
    REQUIRE_MESSAGE(body.velocity().x > kinematic_speed_cap, "move_kinematic clamped the velocity");
    body.set_enabled(false);
    REQUIRE_MESSAGE(at_cap(body.velocity(), {1, 0, 0}, kinematic_speed_cap),
                    "Disabling did not clamp the velocity to the kinematic cap");
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

TEST_CASE("Damping slows only dynamic bodies, by the documented factor per collision step") {
    constexpr int ticks = 60; // One second, one collision step per tick.
    constexpr float damping = 1;
    constexpr float decay_tolerance = 1e-4F; // Float rounding over the steps.
    constexpr auto invalid_damping = "Invalid body damping";
    World world(weightless(4));
    const Vec3 velocity{1, 0, 0};
    const Vec3 spin{0, 1, 0};
    auto settings = box({}, {.5F, .5F, .5F}, Motion::dynamic);
    settings.velocity = velocity;
    settings.angular_velocity = spin;
    auto undamped = world.create(settings);
    settings.pose.position = {0, 5, 0};
    settings.linear_damping = damping;
    settings.angular_damping = damping;
    auto damped = world.create(settings);
    settings.pose.position = {0, 10, 0};
    settings.motion = Motion::kinematic;
    auto kinematic = world.create(settings);
    for (int i = 0; i < ticks; ++i)
        world.step(tick);
    REQUIRE_MESSAGE((identical(undamped.velocity(), velocity) && identical(undamped.angular_velocity(), spin)),
                    "An undamped body lost speed");
    const auto factor = static_cast<float>(std::pow(1 - damping * tick, ticks));
    REQUIRE_MESSAGE(length(damped.velocity() - velocity * factor) < decay_tolerance,
                    "Linear damping did not scale the velocity by max(0, 1 - c dt) per step");
    REQUIRE_MESSAGE(length(damped.angular_velocity() - spin * factor) < decay_tolerance,
                    "Angular damping did not scale the angular velocity by max(0, 1 - c dt) per step");
    REQUIRE_MESSAGE((identical(kinematic.velocity(), velocity) && identical(kinematic.angular_velocity(), spin)),
                    "Damping slowed a kinematic body");
    for (const float bad :
         {-1.F, 61.F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        CAPTURE(bad);
        auto linear = box({}, {.5F, .5F, .5F}, Motion::dynamic);
        linear.linear_damping = bad;
        REQUIRE_THROWS_WITH_AS(world.create(linear), invalid_damping, std::invalid_argument);
        auto angular = box({}, {.5F, .5F, .5F}, Motion::dynamic);
        angular.angular_damping = bad;
        REQUIRE_THROWS_WITH_AS(world.create(angular), invalid_damping, std::invalid_argument);
    }
    settings.linear_damping = 60;
    settings.angular_damping = 60;
    (void)world.create(settings);
    REQUIRE(world.size() == 4u);
}

TEST_CASE("A dynamic body reports its mass, which sets its response to impulses") {
    constexpr float mass = 10;
    constexpr float mass_tolerance = 1e-5F * mass; // A mass recovered from Jolt's inverse mass.
    constexpr auto dynamic_only = "Only dynamic bodies have mass";
    World world(weightless(4));
    auto settings = box({}, {.5F, .25F, 1}, Motion::dynamic);
    settings.mass = mass;
    auto body = world.create(settings);
    REQUIRE(std::abs(body.mass() - mass) < mass_tolerance);
    body.add_impulse({mass, 0, 0});
    REQUIRE_MESSAGE(near(body.velocity(), {1, 0, 0}), "An impulse did not change the velocity by impulse / mass");
    body.set_enabled(false);
    REQUIRE_MESSAGE(std::abs(body.mass() - mass) < mass_tolerance, "A disabled body lost its mass");
    settings.pose.position = {0, 5, 0};
    settings.collider = tetrahedron();
    REQUIRE_MESSAGE(std::abs(world.create(settings).mass() - mass) < mass_tolerance,
                    "A hull's mass did not follow BodySettings::mass");
    REQUIRE_THROWS_WITH_AS((void)world.create(box({0, 10, 0}, {.5F, .5F, .5F})).mass(), dynamic_only,
                           std::invalid_argument);
    REQUIRE_THROWS_WITH_AS((void)world.create(box({0, 15, 0}, {.5F, .5F, .5F}, Motion::kinematic)).mass(), dynamic_only,
                           std::invalid_argument);
    body.remove();
    REQUIRE_THROWS_WITH_AS((void)body.mass(), expired_body, std::out_of_range);
}

TEST_CASE("Impulses at points and angular impulses follow the body's inertia") {
    constexpr float mass = 3;
    const Vec3 half{1, .5F, .25F};
    const auto inertia = box_inertia(mass, half); // (0.3125, 1.0625, 1.25)
    World world(weightless(3));
    auto settings = box({5, 2, -3}, half, Motion::dynamic);
    settings.mass = mass;
    auto struck = world.create(settings);
    // At the corner (1, 0.5, 0.25) from the center, an impulse of 2 along +Z has the moment (1, -2, 0).
    struck.add_impulse_at({0, 0, 2}, struck.world_center_of_mass() + half);
    REQUIRE_MESSAGE(near(struck.velocity(), {0, 0, 2 / mass}), "An impulse at a point did not change the velocity by "
                                                               "impulse / mass");
    REQUIRE_MESSAGE(near(struck.angular_velocity(), {1 / inertia.x, -2 / inertia.y, 0}),
                    "An impulse at a corner did not turn the body by its moment over the inertia");
    // A quarter turn about Z exchanges the X and Y moments in world space.
    settings.pose = {{-5, 2, -3}, quarter_turn_z};
    auto spun = world.create(settings);
    spun.add_angular_impulse({inertia.y, inertia.x, inertia.z});
    REQUIRE_MESSAGE(near(spun.angular_velocity(), {1, 1, 1}),
                    "An angular impulse did not use the body's world-space inertia");
    REQUIRE(identical(spun.velocity(), {}));
}

TEST_CASE("Forces and torques act over the next step and are then cleared") {
    constexpr float mass = 3;
    const Vec3 half{1, .5F, .25F};
    const auto inertia = box_inertia(mass, half);
    const auto seconds = static_cast<float>(tick);
    World world; // Earth gravity, which a force of m g balances.
    auto settings = box({0, 10, 0}, half, Motion::dynamic);
    settings.mass = mass;
    auto held = world.create(settings);
    const auto weight = held.mass() * 9.81F;
    held.add_force({0, weight / 2, 0});
    held.add_force({0, weight / 2, 0});
    world.step(tick);
    REQUIRE_MESSAGE((near(held.velocity(), {}) && near(held.pose().position, {0, 10, 0})),
                    "The sum of the forces did not hold the body against gravity");
    world.step(tick);
    REQUIRE_MESSAGE(near(held.velocity(), {0, -9.81F * seconds, 0}), "A step kept a force from the step before");

    World space(weightless(3));
    settings.pose.position = {};
    auto pushed = space.create(settings);
    // At the corner (1, 0.5, 0.25) from the center, a force of 6 along +Z has the moment (3, -6, 0).
    pushed.add_force_at({0, 0, 6}, pushed.world_center_of_mass() + half);
    settings.pose.position = {5, 0, 0};
    auto twisted = space.create(settings);
    twisted.add_torque(inertia * (1 / seconds));
    settings.pose.position = {-5, 0, 0};
    auto thrown = space.create(settings);
    thrown.add_force({6, 0, 0});
    space.step(tick);
    const Vec3 pushed_velocity{0, 0, 6 / mass * seconds};
    const Vec3 pushed_spin{3 / inertia.x * seconds, -6 / inertia.y * seconds, 0};
    const Vec3 thrown_velocity{6 / mass * seconds, 0, 0};
    REQUIRE_MESSAGE((near(pushed.velocity(), pushed_velocity) && near(pushed.angular_velocity(), pushed_spin)),
                    "A force at a point did not act as the force and its moment for one step");
    REQUIRE_MESSAGE((near(twisted.angular_velocity(), {1, 1, 1}) && identical(twisted.velocity(), {})),
                    "A torque did not change the angular velocity by torque / inertia per second");
    REQUIRE(near(thrown.velocity(), thrown_velocity));
    space.step(tick);
    REQUIRE_MESSAGE((near(pushed.velocity(), pushed_velocity) && near(pushed.angular_velocity(), pushed_spin) &&
                     near(twisted.angular_velocity(), {1, 1, 1}) && near(thrown.velocity(), thrown_velocity)),
                    "A step kept a force or torque from the step before");
    // A step longer than 1/60 s takes several collision steps, and the force acts over all of them.
    thrown.add_force({6, 0, 0});
    space.step(long_step);
    REQUIRE_MESSAGE(near(thrown.velocity(), {6 / mass * (seconds + static_cast<float>(long_step)), 0, 0}),
                    "A force did not act over the whole of a long step");
}

TEST_CASE("Disabling a body discards its forces and torques") {
    World world(weightless(1));
    auto body = world.create(box({}, {.5F, .5F, .5F}, Motion::dynamic));
    body.add_force({100, 0, 0});
    body.add_force_at({0, 50, 0}, {1, 0, 0});
    body.add_torque({0, 0, 20});
    body.set_enabled(false);
    body.set_enabled(true);
    world.step(tick);
    REQUIRE_MESSAGE((identical(body.velocity(), {}) && identical(body.angular_velocity(), {})),
                    "A force or torque added before the body was disabled acted after it was reenabled");
}

TEST_CASE("Forces, torques and impulses require an enabled dynamic body and valid vectors") {
    constexpr auto impulses_only = "Only enabled dynamic bodies accept impulses";
    constexpr auto forces_only = "Only enabled dynamic bodies accept forces and torques";
    World world(weightless(4));
    auto stationary = world.create(box({}, {.5F, .5F, .5F}));
    auto kinematic = world.create(box({3, 0, 0}, {.5F, .5F, .5F}, Motion::kinematic));
    auto disabled = world.create(box({6, 0, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    disabled.set_enabled(false);
    for (auto *body : {&stationary, &kinematic, &disabled}) {
        REQUIRE_THROWS_WITH_AS(body->add_impulse_at({0, 1, 0}, {}), impulses_only, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(body->add_angular_impulse({0, 1, 0}), impulses_only, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(body->add_force({0, 1, 0}), forces_only, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(body->add_force_at({0, 1, 0}, {}), forces_only, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(body->add_torque({0, 1, 0}), forces_only, std::invalid_argument);
    }
    REQUIRE_FALSE_MESSAGE(disabled.enabled(), "A rejected call reenabled a body");
    auto dynamic = world.create(box({9, 0, 0}, {.5F, .5F, .5F}, Motion::dynamic));
    for (const Vec3 bad : {Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0}, Vec3{0, beyond_vector_range, 0}}) {
        CAPTURE(bad.x);
        REQUIRE_THROWS_WITH_AS(dynamic.add_impulse_at(bad, {}), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_impulse_at({0, 1, 0}, bad), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_angular_impulse(bad), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_force(bad), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_force_at(bad, {}), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_force_at({0, 1, 0}, bad), vector_range, std::invalid_argument);
        REQUIRE_THROWS_WITH_AS(dynamic.add_torque(bad), vector_range, std::invalid_argument);
    }
    world.step(tick);
    REQUIRE_MESSAGE((identical(dynamic.velocity(), {}) && identical(dynamic.angular_velocity(), {})),
                    "A rejected call moved the body");
    dynamic.remove();
    REQUIRE_THROWS_WITH_AS(dynamic.add_impulse_at({0, 1, 0}, {}), expired_body, std::out_of_range);
    REQUIRE_THROWS_WITH_AS(dynamic.add_angular_impulse({0, 1, 0}), expired_body, std::out_of_range);
    REQUIRE_THROWS_WITH_AS(dynamic.add_force({0, 1, 0}), expired_body, std::out_of_range);
    REQUIRE_THROWS_WITH_AS(dynamic.add_force_at({0, 1, 0}, {}), expired_body, std::out_of_range);
    REQUIRE_THROWS_WITH_AS(dynamic.add_torque({0, 1, 0}), expired_body, std::out_of_range);
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
