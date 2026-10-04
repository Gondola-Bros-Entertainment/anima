#include "component_payloads.hpp"
#include <anima/physics2d_scene.hpp>
#include <anima/prefab.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

using namespace anima;
namespace p = anima::physics2d;
namespace {
constexpr auto planar_transform = "2D physics requires unit scale, XY translation and Z rotation without tilt/shear";
constexpr auto duplicate_field = "Duplicate JSON document field";
struct Remove {
    ComponentOwner owner;
    void on_fixed_update(double) { owner.object.remove_component<p::RigidBody>(); }
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
        CHECK_MESSAGE(ball.position().y < 1, "Reenable did not restore physics pose");
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
    const std::vector<ComponentData> data{{"anima.rigid-body-2d.v1", "{}", true}};
    CHECK_THROWS_WITH_AS(expired.restore(survivor.create(), data, {}), "2D rigid body codec world expired",
                         std::out_of_range);
    CHECK_THROWS_WITH_AS(p::step(survivor, world, 1. / 60), "2D body belongs to another or expired world",
                         std::invalid_argument);
}
