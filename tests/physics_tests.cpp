#include <anima/physics.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace anima;
using namespace anima::physics;
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
    check(rejected, "Expected rejection");
}
template <class F> void rejects_argument(F f, const char *fragment) {
    bool rejected = false;
    try {
        f();
    } catch (const std::invalid_argument &error) {
        rejected = std::string(error.what()).find(fragment) != std::string::npos;
    }
    check(rejected, "Expected an invalid_argument naming the rejected condition");
}
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
void compound_and_hull() {
    const auto near = [](Vec3 a, Vec3 b) { return length(a - b) < .002F; };
    {
        World world({{0, 0, 0}, 16});
        BodySettings s;
        s.collider = tetrahedron();
        s.motion = Motion::dynamic;
        s.mass = 2;
        s.pose.position = {4, 2, 3};
        auto body = world.create(s);
        check(near(body.local_center_of_mass(), {.5F, .25F, .25F}) &&
                  near(body.world_center_of_mass(), {4.5F, 2.25F, 3.25F}),
              "Automatic hull center of mass is not its volume centroid");
        check(near(body.pose().position, s.pose.position), "Hull creation exposed center of mass as origin");
        body.add_impulse({2, 0, 0});
        check(near(body.velocity(), {1, 0, 0}), "Hull total mass was not applied");
        body.set_velocity({});
        body.set_angular_velocity({0, 0, 2});
        const Vec3 center{4.5F, 2.25F, 3.25F}; // Analytic centroid of this tetrahedron.
        world.step(.1);
        const auto pose = body.pose();
        check(near(point(matrix({pose.position, pose.rotation, {1, 1, 1}}), {.5F, .25F, .25F}), center),
              "Hull rotation moved its center of mass");
        check(!near(pose.position, s.pose.position), "Offset hull origin failed to orbit its center of mass");
        body.teleport({{0, 0, 0}, {0, 0, .70710678F, .70710678F}});
        check(near(body.pose().position, {}), "Hull teleport moved authored origin");
        auto ray = world.raycast({-2, .25F, .1F}, {4, 0, 0});
        check(ray && ray->body == body && ray->point.x < -.5F, "Rotated asymmetric hull geometry misplaced");
        body.remove();
        s.motion = Motion::kinematic;
        s.pose = {};
        auto moving = world.create(s);
        moving.move_kinematic({{2, 3, 0}, {0, 0, .70710678F, .70710678F}}, .1);
        world.step(.1);
        check(near(moving.pose().position, {2, 3, 0}), "Hull kinematic target interpreted as center of mass");
    }
    {
        World world({{0, 0, 0}, 16});
        BodySettings settings;
        settings.collider.shape = Shape::compound;
        ColliderChild first, second;
        first.pose.position = {1, 0, 0};
        second.pose.position = {3, 1, 0};
        settings.collider.children = {first, second};
        settings.motion = Motion::dynamic;
        settings.mass = 4;
        auto compound = world.create(settings);
        check(near(compound.local_center_of_mass(), {2, .5F, 0}), "Automatic compound centroid is incorrect");
        settings.collider.children.clear(); // Backend geometry must own its own immutable data.
        compound.add_impulse({0, 4, 0});
        check(near(compound.velocity(), {0, 1, 0}), "Compound total mass was not applied");
        compound.set_velocity({});
        compound.set_angular_velocity({0, 0, 2});
        world.step(.1);
        const auto pose = compound.pose();
        check(near(point(matrix({pose.position, pose.rotation, {1, 1, 1}}), {2, .5F, 0}), {2, .5F, 0}),
              "Compound origin/center of mass rotation is incorrect");
        compound.teleport({{0, 0, 0}, {0, 0, 0, 1}});
        auto ray = world.raycast({3, 4, 0}, {0, -6, 0});
        check(ray && ray->body == compound && std::abs(ray->point.y - 1.5F) < .001F,
              "Compound child placement or retained geometry failed");
        compound.remove();
        settings.collider.children = {first, second};
        settings.motion = Motion::kinematic;
        auto moving = world.create(settings);
        moving.move_kinematic({{2, 3, 0}, {0, 0, .70710678F, .70710678F}}, .1);
        world.step(.1);
        check(near(moving.pose().position, {2, 3, 0}), "Compound kinematic target used center of mass");
    }
    {
        World world({{0, 0, 0}, 8});
        BodySettings settings;
        settings.collider.shape = Shape::compound;
        ColliderChild small, large;
        large.pose.position = {3, 0, 0};
        large.collider.half_extent = {1, .5F, .5F};
        settings.collider.children = {small, large};
        const auto body = world.create(settings);
        check(near(body.local_center_of_mass(), {2, 0, 0}),
              "Automatic compound center used equal child weights instead of volume");
    }
    {
        World world({{0, 0, 0}, 8});
        BodySettings settings;
        settings.sensor = true;
        settings.collider.shape = Shape::compound;
        ColliderChild left, right;
        left.pose.position = {-1, 0, 0};
        right.pose.position = {1, 0, 0};
        settings.collider.children = {left, right};
        auto sensor = world.create(settings);
        auto moving = world.create(box({}, {1.5F, .5F, .5F}, Motion::dynamic));
        world.step(1. / 60);
        auto events = world.take_events();
        check(events.size() == 1 && events[0].sensor && events[0].phase == ContactPhase::begin,
              "Two compound child contacts did not aggregate into one begin");
        moving.teleport({{-1.75F, 0, 0}});
        world.step(1. / 60);
        check(world.take_events().empty(), "Leaving one compound child invented a body-pair exit");
        moving.teleport({{-5, 0, 0}});
        world.step(1. / 60);
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::end,
              "Leaving the final compound child did not emit one end");
        (void)sensor;
    }
    {
        World world;
        auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
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
        for (int i = 0; i < 240; ++i)
            world.step(1. / 60);
        check(hull.world_center_of_mass().y > 0 && hull.world_center_of_mass().y < .4F,
              "Dynamic asymmetric hull did not settle on floor");
        check(std::abs(compound.pose().position.y - .5F) < .03F,
              "Dynamic compound did not settle at its authored-origin height");
        (void)floor;
    }
    for (const bool assembly : {false, true}) {
        World automatic({{0, 0, 0}, 4}), authored({{0, 0, 0}, 4});
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
        const auto original = automatic.create(settings);
        settings.center_of_mass = Vec3{0, -2, 0};
        auto shifted = authored.create(settings);
        check(near(shifted.pose().position, {3, 0, 0}) && near(shifted.local_center_of_mass(), {0, -2, 0}) &&
                  near(shifted.world_center_of_mass(), {3, -2, 0}),
              "Explicit local center of mass was treated as relative offset");
        const auto ray = automatic.raycast({-5, .2F, .2F}, {20, 0, 0});
        const auto shifted_ray = authored.raycast({-5, .2F, .2F}, {20, 0, 0});
        check(ray && shifted_ray && near(ray->point, shifted_ray->point) && near(ray->normal, shifted_ray->normal),
              "Center of mass override moved ray geometry");
        Collider sphere;
        sphere.shape = Shape::sphere;
        sphere.radius = .1F;
        const auto cast = automatic.sweep(sphere, {{-5, .2F, .2F}}, {20, 0, 0});
        const auto shifted_cast = authored.sweep(sphere, {{-5, .2F, .2F}}, {20, 0, 0});
        check(cast && shifted_cast && std::abs(cast->fraction - shifted_cast->fraction) < .0001F,
              "Center of mass override moved swept geometry");
        check(!authored.overlap(sphere, {{assembly ? 5.1F : 3.1F, .1F, .1F}}).empty(),
              "Center of mass override moved overlap geometry");
        shifted.add_impulse({5, 0, 0});
        check(near(shifted.velocity(), {1, 0, 0}), "Explicit center of mass changed total mass");
        shifted.set_velocity({});
        shifted.set_angular_velocity({0, 0, 2});
        authored.step(.1);
        check(near(shifted.world_center_of_mass(), {3, -2, 0}) &&
                  !near(shifted.pose().position, settings.pose.position),
              "Body did not rotate about explicit center of mass");
        shifted.teleport({{5, 6, 7}, {0, 0, .70710678F, .70710678F}});
        check(near(shifted.pose().position, {5, 6, 7}) && near(shifted.world_center_of_mass(), {7, 6, 7}),
              "Teleport did not preserve explicit local center of mass");
        shifted.set_enabled(false);
        check(near(shifted.local_center_of_mass(), {0, -2, 0}), "Disabled body lost explicit center of mass");
        shifted.remove();
        rejects([&] { (void)shifted.local_center_of_mass(); });
        rejects([&] { (void)shifted.world_center_of_mass(); });
        settings.motion = Motion::kinematic;
        auto moving = authored.create(settings);
        moving.move_kinematic({{3, 4, 5}, {0, 0, .70710678F, .70710678F}}, .1);
        authored.step(.1);
        check(near(moving.pose().position, {3, 4, 5}) && near(moving.world_center_of_mass(), {5, 4, 5}),
              "Kinematic target lost explicit center of mass");
        (void)original;
    }
    {
        World world({{0, 0, 0}, 16});
        auto wall = world.create(box({5, 0, 0}, {.5F, 10, 10}));
        const auto hull = tetrahedron();
        auto cast = world.sweep(hull, {}, {10, 0, 0});
        check(cast && cast->body == wall && std::abs(cast->fraction - .25F) < .002F && cast->normal.x < -.99F,
              "Hull sweep did not transform its authored origin to center of mass");
        check(!world.overlap(hull, {{2, 0, 0}}).size(), "Hull overlap moved geometry toward its center of mass");
        check(!world.overlap(hull, {{3, 0, 0}}).empty(), "Offset hull overlap missed wall");
        Collider compound;
        compound.shape = Shape::compound;
        ColliderChild part;
        part.pose.position = {2, 0, 0};
        part.collider.half_extent = {1, .25F, .5F};
        part.pose.rotation = {0, 0, .70710678F, .70710678F};
        compound.children.push_back(part);
        cast = world.sweep(compound, {}, {10, 0, 0});
        check(cast && std::abs(cast->fraction - .225F) < .002F,
              "Rotated offset child sweep lost local origin or rotation");
        check(world.overlap(compound, {{2, 0, 0}}).empty(), "Compound overlap ignored child rotation");
        check(!world.overlap(compound, {{2.5F, 0, 0}}).empty(), "Compound overlap missed offset child");
        compound.children[0].collider = hull;
        compound.children[0].pose.rotation = {0, 0, 0, 1};
        cast = world.sweep(compound, {}, {10, 0, 0});
        check(cast && std::abs(cast->fraction - .05F) < .002F, "Compound hull child applied center of mass twice");
        auto initial = world.sweep(compound, {{1, 0, 0}}, {10, 0, 0});
        check(initial && initial->fraction == 0 && initial->penetration > 0, "Compound initial overlap missing");
    }
    {
        World world;
        const auto rejects_shape = [&](Collider collider) {
            BodySettings s;
            s.collider = collider;
            rejects([&] { (void)world.create(s); });
            rejects([&] { (void)world.sweep(collider, {}, {1, 0, 0}); });
            rejects([&] { (void)world.overlap(collider, {}); });
            check(world.size() == 0, "Invalid collider leaked a body");
        };
        auto hull = tetrahedron();
        hull.vertices[3] = {1, 1, 0};
        rejects_shape(hull);
        hull.vertices = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
        rejects_shape(hull);
        hull.vertices.assign(4, {});
        rejects_shape(hull);
        hull = tetrahedron();
        hull.vertices[3].z = .00001F;
        rejects_shape(hull);
        hull = tetrahedron();
        hull.vertices[0].x = std::numeric_limits<float>::infinity();
        rejects_shape(hull);
        hull = tetrahedron();
        hull.vertices.resize(257);
        rejects_shape(hull);
        hull = tetrahedron();
        hull.indices = {0, 1, 2};
        rejects_shape(hull);
        hull = tetrahedron();
        hull.children.resize(1);
        rejects_shape(hull);
        Collider compound;
        compound.shape = Shape::compound;
        rejects_shape(compound);
        compound.children.resize(65);
        rejects_shape(compound);
        compound.children.resize(1);
        compound.children[0].pose.rotation = {0, 0, 0, 0};
        rejects_shape(compound);
        compound.children[0].pose = {};
        compound.children[0].pose.position.x = 2e6F;
        rejects_shape(compound);
        compound.children[0].pose = {};
        compound.children[0].collider.shape = Shape::mesh;
        rejects_shape(compound);
        compound.children[0].collider.shape = Shape::compound;
        compound.children[0].collider.children.resize(64);
        rejects_shape(compound);
        Collider primitive;
        primitive.children.push_back({{}, compound});
        rejects_shape(primitive);
        for (const auto center : {Vec3{2e6F, 0, 0}, Vec3{0, std::numeric_limits<float>::infinity(), 0}}) {
            BodySettings settings;
            settings.center_of_mass = center;
            rejects([&] { (void)world.create(settings); });
            check(world.size() == 0, "Invalid center of mass leaked a body");
        }
        BodySettings mesh;
        mesh.collider.shape = Shape::mesh;
        mesh.collider.vertices = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};
        mesh.collider.indices = {0, 1, 2};
        mesh.center_of_mass = Vec3{};
        rejects([&] { (void)world.create(mesh); });
        check(world.size() == 0, "Mesh center of mass override leaked a body");
    }
}
void run() {
    compound_and_hull();
    Body stale;
    {
        World world;
        auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
        auto falling = world.create(box({0, 4, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        stale = falling;
        for (int i = 0; i < 240; ++i)
            world.step(1. / 60);
        check(std::abs(falling.pose().position.y - .5F) < .03F, "Dynamic body did not settle on floor");
        auto events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::begin && !events[0].sensor,
              "Contact begin not aggregated");
        falling.remove();
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::end, "Removal did not end contact");
        check(!stale.valid() && floor.valid(), "Body lifetime corrupted");
        auto replacement = world.create(box({0, 4, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        check(replacement != stale, "Reused body slot revived stale identity");
        rejects([&] { (void)stale.pose(); });
        stale.remove();
        replacement.set_enabled(false);
        world.step(1. / 60);
        check(!world.raycast({0, 10, 0}, {0, -8, 0}), "Disabled body appears in queries");
        replacement.set_enabled(true);
        check(world.raycast({0, 10, 0}, {0, -8, 0})->body == replacement, "Reenabled body missing");
        replacement.add_impulse({0, 2, 0});
        check(replacement.velocity().y > 0, "Impulse not applied");
        rejects([&] { world.step(0); });
        rejects([&] { world.step(1); });
        rejects([&] { (void)world.raycast({}, {0, 0, 0}); });
        rejects([&] { floor.set_velocity({1, 0, 0}); });
        auto bad = box({}, {0, 1, 1});
        rejects([&] { (void)world.create(bad); });
        bad = box({}, {1, 1, 1});
        bad.mass = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { (void)world.create(bad); });
        stale = replacement;
        World other;
        check(!other.owns(replacement), "Foreign world accepted body");
        rejects([&] {
            QueryFilter f;
            f.ignore = replacement;
            (void)other.raycast({0, 1, 0}, {0, -2, 0}, f);
        });
    }
    check(!stale.valid(), "World teardown retained body");
    stale.remove();
    { // Bridge upper deck, underside, seam and large sweep without heightfield assumptions.
        World world;
        BodySettings mesh;
        mesh.collider.shape = Shape::mesh;
        mesh.collider.vertices = {{-5, 3, -5}, {5, 3, -5}, {5, 3, 5}, {-5, 3, 5}};
        mesh.collider.indices = {0, 2, 1, 0, 3, 2};
        auto deck = world.create(mesh);
        auto floor = world.create(box({0, -.5F, 0}, {20, .5F, 20}));
        auto ray = world.raycast({0, 10, 0}, {0, -20, 0});
        check(ray && ray->body == deck && std::abs(ray->point.y - 3) < .001F && ray->normal.y > .99F,
              "Deck ray or normal failed");
        ray = world.raycast({0, 1, 0}, {0, 4, 0});
        check(ray && ray->body == deck, "Bridge underside missing");
        Collider sphere;
        sphere.shape = Shape::sphere;
        sphere.radius = .5F;
        auto sweep = world.sweep(sphere, {{0, 100, 0}}, {0, -200, 0});
        check(sweep && sweep->body == deck && std::abs(sweep->fraction - .4825F) < .001F && sweep->normal.y > .99F,
              "Large swept sphere crossed deck/seam");
        sweep = world.sweep(sphere, {{0, 1, 0}}, {0, 4, 0});
        check(sweep && sweep->body == deck && sweep->normal.y < -.99F, "Swept underside normal failed");
        auto overlaps = world.overlap(sphere, {{0, .25F, 0}});
        check(!overlaps.empty() && overlaps.front().body == floor && overlaps.front().penetration > .2F,
              "Clearance overlap failed");
        sphere.shape = Shape::capsule;
        sweep = world.sweep(sphere, {{0, 5, 0}}, {0, -5, 0});
        check(sweep && sweep->body == deck, "Capsule sweep failed");
        mesh.collider.indices = {0, 0, 1};
        rejects([&] { (void)world.create(mesh); });
        mesh.collider.indices = {0, 1, 99};
        rejects([&] { (void)world.create(mesh); });
        mesh.collider.indices = {0, 2, 1};
        mesh.motion = Motion::dynamic;
        rejects([&] { (void)world.create(mesh); });
    }
    {
        World world({{0, 0, 0}, 8});
        world.set_layer_collision(0, 1, false);
        auto sensorSettings = box({}, {2, 2, 2});
        sensorSettings.sensor = true;
        sensorSettings.layer = 2;
        auto sensor = world.create(sensorSettings);
        auto moving = world.create(box({}, {.2F, .2F, .2F}, Motion::dynamic));
        auto ignoredSettings = box({}, {1, 1, 1});
        ignoredSettings.layer = 1;
        auto ignored = world.create(ignoredSettings);
        (void)ignored;
        world.step(1. / 60);
        auto events = world.take_events();
        check(events.size() == 1 && events[0].sensor && events[0].phase == ContactPhase::begin,
              "Sensor/layer filtering failed");
        QueryFilter f;
        f.layers = 1u << 2;
        check(!world.raycast({0, 3, 0}, {0, -6, 0}, f), "Sensors included by default");
        f.sensors = true;
        check(world.raycast({0, 3, 0}, {0, -6, 0}, f)->body == sensor, "Sensor query missing");
        moving.teleport({{0, 10, 0}});
        world.step(1. / 60);
        events = world.take_events();
        check(events.size() == 1 && events[0].phase == ContactPhase::end, "Sensor exit missing");
        auto kinematic = world.create(box({10, 0, 0}, {.5F, .5F, .5F}, Motion::kinematic));
        kinematic.move_kinematic({{11, 0, 0}}, .1);
        world.step(.1);
        check(std::abs(kinematic.pose().position.x - 11) < .001F, "Kinematic target failed");
    }
    {
        World world({{0, 0, 0}, 16});
        BodySettings sphere;
        sphere.collider.shape = Shape::sphere;
        sphere.collider.radius = 1;
        auto target = world.create(sphere);
        const auto tangent = world.raycast({-2, 1, 0}, {4, 0, 0});
        check(tangent && tangent->body == target && std::abs(tangent->fraction - .5F) < .001F,
              "Tangent ray missed sphere");
        const auto inside = world.raycast({0, 0, 0}, {4, 0, 0});
        check(inside && inside->fraction == 0, "Ray starting inside did not report initial overlap");
        Collider query;
        query.shape = Shape::sphere;
        query.radius = .5F;
        auto initial = world.sweep(query, {{0, .25F, 0}}, {0, 5, 0});
        check(initial && initial->fraction == 0 && initial->penetration > 0, "Sweep initial penetration missing");
        target.remove();
        auto wall = world.create(box({0, 0, 0}, {.05F, 5, 5}));
        sphere.motion = Motion::dynamic;
        sphere.continuous = true;
        sphere.collider.radius = .2F;
        sphere.pose.position = {-5, 0, 0};
        sphere.velocity = {100, 0, 0};
        auto fast = world.create(sphere);
        world.step(.1);
        check(fast.pose().position.x < 0, "Continuous body tunneled through thin wall");
        fast.set_enabled(false);
        fast.teleport({{-4, 0, 0}});
        fast.set_velocity({});
        fast.set_angular_velocity({0, 1, 0});
        fast.set_enabled(true);
        world.step(1. / 60);
        check(fast.angular_velocity().y > 0 && std::abs(fast.pose().rotation[1]) > 0, "Angular state did not advance");
        (void)wall;
    }
    for (int i = 0; i < 3; ++i) {
        World world({{0, 0, 0}, 1});
        auto body = world.create({});
        rejects([&] { (void)world.create({}); });
        body.remove();
    }
    {
        // Disabled bodies are outside Jolt's broadphase; an impulse must not reactivate them.
        World world;
        auto floor = world.create(box({0, -.5F, 0}, {5, .5F, 5}));
        auto crate = world.create(box({0, .4F, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        auto parked = world.create(box({3, .4F, 0}, {.5F, .5F, .5F}, Motion::dynamic));
        world.step(1. / 60);
        crate.set_enabled(false);
        parked.set_enabled(false);
        rejects_argument([&] { crate.add_impulse({0, 0, 0}); }, "enabled");
        rejects_argument([&] { parked.add_impulse({0, 1, 0}); }, "enabled");
        parked.remove();
        for (int i = 0; i < 3; ++i)
            world.step(1. / 60);
        const auto hit = world.raycast({0, 10, 0}, {0, -12, 0});
        check(!crate.enabled() && hit && hit->body == floor, "Rejected impulse reactivated a disabled body");
        crate.set_enabled(true);
        crate.add_impulse({0, 2, 0});
        check(crate.velocity().y > 0, "Reenabled body did not accept an impulse");
    }
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS physics worlds, lifetime, contacts, layers, meshes and queries\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
