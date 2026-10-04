#include "component_payloads.hpp"
#include <anima/physics2d_scene.hpp>
#include <anima/prefab.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace anima;
namespace p = anima::physics2d;
namespace {
constexpr auto planar_transform = "2D physics requires unit scale, XY translation and Z rotation without tilt/shear";
constexpr auto duplicate_field = "Duplicate JSON document field";
struct Remove {
    GameObject owner;
    void on_fixed_update(double) { owner.remove_component<p::RigidBody>(); }
};
struct Counter {
    int *count;
    void on_fixed_update(double) { ++*count; }
};
} // namespace

TEST_CASE("Rigid bodies drive XY poses, keep presentation depth and follow enablement, activity and prefabs") {
    p::World world;
    {
        Scene scene;
        auto floor = scene.create("floor");
        floor.set_position({0, -.5F, 0});
        p::BodySettings settings;
        settings.collider.half_extent = {10, .5F};
        floor.add_component<p::RigidBody>(world, settings);
        auto ball = scene.create("ball");
        ball.set_position({0, 4, 7});
        settings = {};
        settings.collider.shape = p::Shape::circle;
        settings.motion = p::Motion::dynamic;
        auto rigid = ball.add_component<p::RigidBody>(world, settings);
        int ticks = 0;
        auto counter = scene.create();
        counter.add_component<Counter>(&ticks);
        for (int i = 0; i < 240; ++i) {
            scene.fixed_update(1. / 60);
            p::step(scene, world, 1. / 60);
        }
        CHECK(ticks == 240);
        CHECK(std::abs(ball.position().y - .5F) < .03F);
        CHECK(ball.position().z == 7);
        CHECK_FALSE_MESSAGE(rigid->body().awake(), "Scene synchronization woke sleeping body");
        rigid.set_enabled(false);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE_MESSAGE(rigid->body().enabled(), "Disabled component remains active");
        ball.set_position({0, 10, 7});
        rigid.set_enabled(true);
        p::step(scene, world, 1. / 60);
        // The edit made while disabled moves the body when it is enabled again.
        CHECK_MESSAGE(std::abs(ball.position().y - 10) < .01F, "Reenabling ignored the object's edit");
        CHECK(rigid->body().enabled());
        ball.set_active(false);
        p::step(scene, world, 1. / 60);
        CHECK(rigid.enabled());
        CHECK_FALSE_MESSAGE(rigid->body().enabled(), "Inactive object retained physics participation");
        ball.set_active(true);
        p::step(scene, world, 1. / 60);
        CHECK_MESSAGE(rigid->body().enabled(), "Object activation did not restore physics participation");
        rigid->body().set_angular_velocity(1);
        ComponentCodecs codecs;
        p::add_component_codec(codecs, world);
        const auto text = Prefab::capture(ball, codecs).serialize({});
        auto restored = Prefab::deserialize(text, {}, codecs).instantiate(scene);
        auto body = restored.get_component<p::RigidBody>()->body();
        // The prefab restores an independent body with its own state and depth.
        CHECK(world.size() == 3u);
        CHECK(body.angular_velocity() == 1);
        CHECK(restored.position().z == 7);
        restored.destroy();
        CHECK_FALSE_MESSAGE(body.valid(), "Prefab body leaked");
        CHECK(world.size() == 2u);
        ball.add_component<Remove>();
        scene.fixed_update(1. / 60);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE(rigid);
        CHECK_MESSAGE(world.size() == 1u, "Callback removal retained body");
        auto bad = scene.create();
        bad.set_transform({{}, {0, 0, 0, 1}, {2, 1, 1}});
        CHECK_THROWS_WITH_AS(bad.add_component<p::RigidBody>(world, settings), planar_transform, std::invalid_argument);
        bad.set_transform({{}, {.70710678F, 0, 0, .70710678F}, {1, 1, 1}});
        CHECK_THROWS_WITH_AS(bad.add_component<p::RigidBody>(world, settings), planar_transform, std::invalid_argument);
        auto child = scene.create();
        child.set_parent(floor);
        CHECK_THROWS_WITH_AS(child.add_component<p::RigidBody>(world, settings),
                             "Dynamic 2D bodies must be scene roots", std::invalid_argument);
        settings.motion = p::Motion::stationary;
        child.add_component<p::RigidBody>(world, settings);
        p::step(scene, world, 1. / 60);
        child.destroy();
    }
    CHECK_MESSAGE(world.size() == 0u, "Scene teardown leaked 2D bodies");
}

TEST_CASE("A stationary child's body follows its hierarchy's activity") {
    p::World world;
    {
        Scene scene;
        auto parent = scene.create(), child = scene.create();
        child.set_parent(parent);
        auto body = child.add_component<p::RigidBody>(world)->body();
        parent.set_active(false);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE_MESSAGE(body.enabled(), "Inactive parent retained stationary child body");
        child.clear_parent();
        p::step(scene, world, 1. / 60);
        CHECK_MESSAGE(body.enabled(), "Reparent did not restore body participation");
    }
    CHECK_MESSAGE(world.size() == 0u, "Scene teardown leaked 2D bodies");
}

TEST_CASE("A malformed rigid body payload rolls back the prefab's earlier body") {
    p::World world;
    Scene scene;
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    auto root = scene.create();
    root.add_component<p::RigidBody>(world);
    const auto prefab = Prefab::capture(root, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto before = scene.size();
    const auto payloads = invalid_component_payloads(nodes[0].components[0].state, "motion");
    // invalid_component_payloads returns an empty object, the field renamed, an unknown field, the field
    // duplicated, the duplicate spelled with an escape, and an unknown field nested past the depth limit.
    const std::array errors{"Missing JSON field: shape",
                            "Missing JSON field: motion",
                            "Unknown JSON field: unexpected",
                            duplicate_field,
                            duplicate_field,
                            "JSON document exceeds nesting limit"};
    REQUIRE(payloads.size() == errors.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        CAPTURE(i);
        nodes[1].components[0].state = payloads[i];
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), errors[i], std::invalid_argument);
        // The rollback leaves only the scene's own root and body.
        CHECK(scene.size() == before);
        CHECK(world.size() == 1u);
    }
}

TEST_CASE("An invalid snapshot moves no body") {
    p::World world;
    Scene scene;
    auto first = scene.create();
    auto second = scene.create();
    auto a = first.add_component<p::RigidBody>(world)->body();
    second.add_component<p::RigidBody>(world);
    first.set_position({10, 0, 0});
    second.set_position({2e6F, 0, 0});
    CHECK_THROWS_WITH_AS(p::step(scene, world, 1. / 60), "2D physics position outside supported range",
                         std::invalid_argument);
    CHECK_MESSAGE(a.pose().position.x == 0, "Invalid snapshot partially moved an earlier body");
}

TEST_CASE("A dynamic body follows edits to its object, apart from depth, and keeps its velocity") {
    constexpr double tick = 1. / 60;
    constexpr float tolerance = 1e-5F;
    p::World world({{0, 0}, 4});
    Scene scene;
    auto ball = scene.create();
    ball.set_position({0, 0, 7});
    p::BodySettings settings;
    settings.collider.shape = p::Shape::circle;
    settings.motion = p::Motion::dynamic;
    settings.velocity = {1, 0};
    auto body = ball.add_component<p::RigidBody>(world, settings)->body();
    p::step(scene, world, tick);
    // A respawn written to the object teleports the body before the step.
    ball.set_position({0, 5, 7});
    p::step(scene, world, tick);
    CHECK(std::abs(body.pose().position.x - 1.F / 60) < tolerance);
    CHECK(body.pose().position.y == 5);
    CHECK(std::abs(body.velocity().x - 1) < tolerance);
    CHECK(ball.position().y == 5);
    // A teleport through body() holds while the object is left alone.
    body.teleport({{-3, 0}, 0});
    p::step(scene, world, tick);
    CHECK(std::abs(ball.position().x - (-3 + 1.F / 60)) < tolerance);
    CHECK(ball.position().y == 0);
    // Depth is presentation only: changing it neither moves nor wakes a sleeping body.
    body.set_velocity({0, 0});
    for (int i = 0; i < 60; ++i)
        p::step(scene, world, tick);
    REQUIRE_FALSE(body.awake());
    const auto rest = ball.position();
    ball.set_position({rest.x, rest.y, 2});
    p::step(scene, world, tick);
    CHECK_FALSE_MESSAGE(body.awake(), "A depth edit woke the body");
    CHECK(ball.position().z == 2);
}

TEST_CASE("Stationary and kinematic bodies follow their objects over writes through body()") {
    p::World world({{0, 0}, 4});
    Scene scene;
    auto wall = scene.create(), platform = scene.create();
    wall.set_position({0, -10, 0});
    platform.set_position({4, 0, 0});
    auto stationary = wall.add_component<p::RigidBody>(world)->body();
    p::BodySettings settings;
    settings.motion = p::Motion::kinematic;
    auto kinematic = platform.add_component<p::RigidBody>(world, settings)->body();
    stationary.teleport({{1, 2}, 0});
    kinematic.set_velocity({5, 0});
    p::step(scene, world, 1. / 60);
    const auto wall_pose = stationary.pose();
    CHECK_MESSAGE((wall_pose.position.x == 0 && wall_pose.position.y == -10),
                  "The step kept a teleport of a stationary body");
    const auto platform_velocity = kinematic.velocity();
    CHECK_MESSAGE((platform_velocity.x == 0 && platform_velocity.y == 0),
                  "The step kept a velocity written to a kinematic body");
    CHECK(kinematic.pose().position.x == 4);
}

TEST_CASE("Only an active rigid body's object must meet the transform rules") {
    constexpr double tick = 1. / 60;
    p::World world({{0, 0}, 4});
    Scene scene;
    auto hand = scene.create("hand");
    hand.set_transform({{0, 1, 0}, {0, 0, 0, 1}, {2, 2, 2}});
    p::BodySettings settings;
    settings.collider.shape = p::Shape::circle;
    settings.motion = p::Motion::dynamic;
    auto item = scene.create("item");
    auto held = item.add_component<p::RigidBody>(world, settings);
    // A disabled dynamic item may be carried under a scaled hand.
    held.set_enabled(false);
    item.set_parent(hand, ReparentMode::keep_local);
    CHECK_NOTHROW(p::step(scene, world, tick));
    held.set_enabled(true);
    CHECK_THROWS_WITH_AS(p::step(scene, world, tick), "Dynamic 2D bodies must be scene roots", std::invalid_argument);
    // Released as a root, its body moves to where the object was let go.
    item.clear_parent();
    item.set_transform({{3, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}});
    p::step(scene, world, tick);
    CHECK(held->body().enabled());
    CHECK(held->body().pose().position.x == 3);
    auto fixture = scene.create("fixture");
    auto mount = fixture.add_component<p::RigidBody>(world);
    mount.set_enabled(false);
    fixture.set_parent(hand, ReparentMode::keep_local);
    CHECK_NOTHROW(p::step(scene, world, tick));
    mount.set_enabled(true);
    CHECK_THROWS_WITH_AS(p::step(scene, world, tick), planar_transform, std::invalid_argument);
    fixture.destroy();
    // A fixed-rotation kinematic object may turn while inactive, but not once it is active again.
    settings.motion = p::Motion::kinematic;
    settings.fixed_rotation = true;
    auto lever = scene.create("lever");
    auto turning = lever.add_component<p::RigidBody>(world, settings);
    turning.set_enabled(false);
    lever.set_transform({{}, {0, 0, std::sin(.5F), std::cos(.5F)}, {1, 1, 1}});
    CHECK_NOTHROW(p::step(scene, world, tick));
    turning.set_enabled(true);
    CHECK_THROWS_WITH_AS(p::step(scene, world, tick), "Fixed-rotation kinematic object changed angle",
                         std::invalid_argument);
}

TEST_CASE("Bodies and codecs expire with their world") {
    p::World world;
    Scene survivor;
    ComponentCodecs expired;
    p::Body handle;
    {
        p::World temporary;
        p::add_component_codec(expired, temporary);
        handle = survivor.create().add_component<p::RigidBody>(temporary)->body();
    }
    CHECK_FALSE_MESSAGE(handle.valid(), "World teardown left body valid");
    const std::vector<ComponentData> data{{"anima.rigid-body-2d.v2", "{}", true}};
    CHECK_THROWS_WITH_AS(expired.restore(survivor.create(), data, {}), "2D rigid body codec world expired",
                         std::out_of_range);
    CHECK_THROWS_WITH_AS(p::step(survivor, world, 1. / 60), "2D body belongs to another or expired world",
                         std::invalid_argument);
}

TEST_CASE("The 2D rigid body codec keeps mass and damping and stores shape and motion by name") {
    p::World world({{0, 0}, 8});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    p::BodySettings settings;
    settings.collider.shape = p::Shape::capsule;
    settings.collider.radius = .25F;
    settings.motion = p::Motion::dynamic;
    settings.mass = 3;
    settings.linear_damping = .25F;
    settings.angular_damping = .5F;
    auto root = scene.create();
    root.add_component<p::RigidBody>(world, settings);
    const auto prefab = Prefab::capture(root, codecs);
    const auto &components = prefab.nodes()[0].components;
    REQUIRE(components.size() == 1u);
    CHECK(components[0].type == "anima.rigid-body-2d.v2");
    const auto payload = components[0].state;
    for (const std::string_view field : {"\"shape\":\"capsule\"", "\"motion\":\"dynamic\"", "\"half_extent\":[0.5,0.5]",
                                         "\"mass\":3.0", "\"linear_damping\":0.25", "\"angular_damping\":0.5"}) {
        CAPTURE(field);
        CHECK(payload.find(field) != std::string::npos);
    }
    auto copy = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    const auto restored = copy.get_component<p::RigidBody>();
    const auto &s = restored->settings();
    CHECK((s.collider.shape == p::Shape::capsule && s.motion == p::Motion::dynamic && s.collider.radius == .25F));
    CHECK((s.mass == 3 && s.linear_damping == .25F && s.angular_damping == .5F));
    CHECK(std::abs(restored->body().mass() - 3) < 1e-5F);
    copy.destroy();

    // Version 1's keys, enumerator numbers and names that differ only in case are rejected.
    const auto changed = [&](std::string_view before, std::string_view after) {
        auto result = payload;
        const auto at = result.find(before);
        REQUIRE(at != std::string::npos);
        result.replace(at, before.size(), after);
        return result;
    };
    struct Malformed {
        std::string state;
        const char *error;
    };
    const std::array<Malformed, 11> malformed{{
        {changed("\"half_extent\":", "\"extent\":"), "Missing JSON field: half_extent"},
        {changed("\"mass\":", "\"density\":"), "Missing JSON field: mass"},
        {changed("\"angular_damping\":0.5,", ""), "Missing JSON field: angular_damping"},
        {changed("\"linear_damping\":0.25,", ""), "Missing JSON field: linear_damping"},
        {changed("\"shape\":\"capsule\"", "\"shape\":2"), "Unknown 2D rigid body shape"},
        {changed("\"shape\":\"capsule\"", "\"shape\":\"Capsule\""), "Unknown 2D rigid body shape"},
        {changed("\"motion\":\"dynamic\"", "\"motion\":2"), "Unknown 2D rigid body motion"},
        {changed("\"linear_damping\":0.25", "\"linear_damping\":-1"), "Invalid 2D body damping"},
        {changed("\"angular_damping\":0.5", "\"angular_damping\":61"), "Invalid 2D body damping"},
        {changed("\"mass\":3.0", "\"mass\":0"), "Invalid 2D body mass/material"},
        {changed("\"layer\":0", "\"layer\":16"), "Invalid 2D rigid body layer"},
    }};
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        CAPTURE(i);
        const std::vector<ComponentData> data{{"anima.rigid-body-2d.v2", malformed[i].state, true}};
        CHECK_THROWS_WITH_AS(codecs.restore(scene.create(), data, {}), malformed[i].error, std::invalid_argument);
        CHECK_MESSAGE(world.size() == 1u, "A rejected payload created a body");
    }
    const std::vector<ComponentData> version_1{{"anima.rigid-body-2d.v1", payload, true}};
    CHECK_THROWS_WITH_AS(codecs.restore(scene.create(), version_1, {}), "Unknown serialized component type",
                         std::invalid_argument);
}
