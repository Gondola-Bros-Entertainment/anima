#include "component_payloads.hpp"
#include <anima/navigation_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace n = anima::navigation;
using namespace anima;
namespace {
constexpr auto idle_scene = "Scene drivers require an idle live scene";
constexpr auto member_callbacks = "A member scene is running callbacks";
constexpr auto set_updating = "Scene set is updating";
constexpr auto set_changing = "Scene set is changing membership";
constexpr auto step_range = "Navigation step must be in [0.000001, 0.1] seconds";
constexpr auto position_range = "Navigation position outside finite supported range";
constexpr auto invalid_settings = "Invalid navigation speed/arrival distance";
constexpr auto unknown_plane = "Unknown navigation steer plane";
constexpr auto duplicate_field = "Duplicate JSON document field";
struct HookCounts {
    int enabled{}, frame{}, fixed{}, late{};
};
struct DriverProbe {
    Scene *scene;
    SceneSet *scenes;
    HookCounts *counts;
    // What the set driver reports from these hooks: that the set is updating when the set runs them, and
    // that a member scene is running callbacks when that scene runs them itself.
    const char *set_rejection;
    void reject_nested() const {
        CHECK_THROWS_WITH_AS(n::update_agents(*scene, .1), idle_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(n::update_agents(*scenes, .1), set_rejection, std::logic_error);
    }
    void on_enable() noexcept { ++counts->enabled; }
    void on_update(double) {
        ++counts->frame;
        reject_nested();
    }
    void on_fixed_update(double) {
        ++counts->fixed;
        reject_nested();
    }
    void on_late_update(double) {
        ++counts->late;
        reject_nested();
    }
};
struct DriverConstruction {
    DriverConstruction(Scene &scene, SceneSet &scenes) {
        CHECK_THROWS_WITH_AS(n::update_agents(scene, .1), idle_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), member_callbacks, std::logic_error);
    }
};
} // namespace

TEST_CASE("An agent steers from its world pose and keeps its state through rejections and persistence") {
    Scene scene;
    auto parent = scene.create();
    parent.set_position({5, 0, 0});
    auto object = scene.create();
    object.set_parent(parent, ReparentMode::keep_local);
    auto agent = object.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}},
                                                n::SteerSettings{.speed = 2, .arrival_distance = 0});
    n::update_agents(scene, .1);
    // Navigation reads the world pose and leaves moving the object to the application.
    CHECK(agent->follower().next() == 1u);
    CHECK(agent->desired_velocity().x == 2);
    CHECK(object.position().x == 5);
    CHECK(object.local_position().x == 0);
    for (const auto invalid : {-1.F, std::nextafter(10'000.F, std::numeric_limits<float>::infinity()),
                               std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(invalid);
        CHECK_THROWS_WITH_AS(agent->configure({.speed = invalid}), invalid_settings, std::invalid_argument);
        CHECK_THROWS_WITH_AS(agent->configure({.speed = 2, .arrival_distance = invalid}), invalid_settings,
                             std::invalid_argument);
        // Rejected settings leave the accepted state unchanged.
        CHECK(agent->settings().speed == 2);
        CHECK(agent->settings().arrival_distance == 0);
        CHECK(agent->desired_velocity().x == 2);
        CHECK(agent->follower().next() == 1u);
    }
    CHECK_THROWS_WITH_AS(agent->configure({.speed = 2, .arrival_distance = 0, .plane = static_cast<n::SteerPlane>(2)}),
                         unknown_plane, std::invalid_argument);
    CHECK(agent->settings().plane == n::SteerPlane::xyz);
    CHECK(agent->desired_velocity().x == 2);
    CHECK_THROWS_WITH_AS(n::Agent({}, {.speed = -1}), invalid_settings, std::invalid_argument);
    Scene empty;
    CHECK_THROWS_WITH_AS(n::update_agents(empty, 0), step_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(n::update_agents(empty, std::numeric_limits<double>::quiet_NaN()), step_range,
                         std::invalid_argument);
    agent.set_enabled(false);
    n::update_agents(scene, .1);
    CHECK_MESSAGE(length(agent->desired_velocity()) == 0, "Disabled agent retained motion");
    CHECK_MESSAGE(agent->follower().next() == 1u, "Disabled agent consumed its route");
    agent.set_enabled(true);
    parent.set_active(false);
    n::update_agents(scene, .1);
    CHECK(agent.enabled());
    CHECK_MESSAGE(length(agent->desired_velocity()) == 0, "Inactive hierarchy retained navigation intent");
    CHECK(agent->follower().next() == 1u);
    parent.set_active(true);
    n::update_agents(scene, .1);
    CHECK_MESSAGE(agent->desired_velocity().x == 2, "Navigation did not resume with hierarchy");
    agent.set_enabled(false);
    ComponentCodecs codecs;
    n::add_component_codec(codecs);
    auto prefab = Prefab::capture(object, codecs);
    auto restored = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    auto copy = restored.get_component<n::Agent>();
    CHECK_FALSE_MESSAGE(copy.enabled(), "Agent persistence lost enablement");
    CHECK(copy->follower().next() == 1u);
    CHECK(copy->settings().speed == 2);
    CHECK(copy->settings().plane == n::SteerPlane::xyz);
    copy.set_enabled(true);
    restored.set_position({6, 0, 0});
    n::update_agents(scene, .1);
    CHECK(copy->follower().finished());
    CHECK_FALSE_MESSAGE(agent->follower().finished(), "Prefab cursor state aliased source");
    auto bad = scene.create();
    auto bad_agent = bad.add_component<n::Agent>(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}});
    bad.set_position({2e6F, 0, 0});
    agent.set_enabled(true);
    object.set_position({6, 0, 0});
    CHECK_THROWS_WITH_AS(n::update_agents(scene, .1), position_range, std::invalid_argument);
    // The invalid snapshot consumes no route, not even the valid agent's observed arrival.
    CHECK(agent->follower().next() == 1u);
    CHECK(bad_agent->follower().next() == 0u);
    bad.destroy();
    n::update_agents(scene, .1);
    CHECK_MESSAGE(agent->follower().finished(), "Valid snapshot did not consume observed arrival");
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto before = scene.size();
    const auto state = nodes[0].components[0].state;
    for (const std::string_view field : {"speed", "plane"}) {
        CAPTURE(field);
        const auto payloads = invalid_component_payloads(state, field);
        // invalid_component_payloads returns an empty object, the field renamed, an unknown field, the field
        // duplicated, the duplicate spelled with an escape, and an unknown field nested past the depth limit.
        const auto missing = "Missing JSON field: " + std::string(field);
        const std::array<std::string, 6> errors{"Missing JSON field: route",
                                                missing,
                                                "Unknown JSON field: unexpected",
                                                duplicate_field,
                                                duplicate_field,
                                                "JSON document exceeds nesting limit"};
        REQUIRE(payloads.size() == errors.size());
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            CAPTURE(i);
            nodes[1].components[0].state = payloads[i];
            CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), errors[i].c_str(), std::invalid_argument);
            CHECK_MESSAGE(scene.size() == before, "Navigation prefab rollback leaked objects");
        }
    }
    // The plane is stored by name, so neither another name nor an enumerator's number restores.
    for (const auto *plane : {"\"plane\":\"xy\"", "\"plane\":0"}) {
        CAPTURE(plane);
        auto payload = state;
        const std::string stored = "\"plane\":\"xyz\"";
        REQUIRE(payload.find(stored) != std::string::npos);
        nodes[1].components[0].state = payload.replace(payload.find(stored), stored.size(), plane);
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), unknown_plane, std::invalid_argument);
        CHECK(scene.size() == before);
    }
    object.destroy();
    restored.destroy();
    // Component handles expire with their objects.
    CHECK_FALSE(agent);
    CHECK_FALSE(copy);
}

TEST_CASE("An agent above a grid route steers in the XZ plane and keeps its plane through persistence") {
    const n::Grid grid{4, 3, {-2, 0, -1}, 1, std::vector<float>(12, 1), true};
    const auto map = n::make_grid(grid);
    const auto start = n::grid_node(grid, {-2, 1, -1}), goal = n::grid_node(grid, {1, 1, 1});
    REQUIRE(start.has_value());
    REQUIRE(goal.has_value());
    const auto route = n::waypoints(map, n::find_path(map, *start, *goal));
    Scene scene;
    // The object's origin is one unit above the grid, like the center of a character standing on it.
    auto object = scene.create();
    object.set_position({-2, 1, -1});
    auto agent = object.add_component<n::Agent>(route, n::SteerSettings{.speed = 2, .plane = n::SteerPlane::xz});
    constexpr double step = .05;
    for (int i = 0; i < 100 && !agent->follower().finished(); ++i) {
        n::update_agents(scene, step);
        const auto velocity = agent->desired_velocity();
        CHECK_MESSAGE(velocity.y == 0, "Planar agent steered out of the XZ plane");
        object.set_position(object.position() + velocity * static_cast<float>(step));
    }
    CHECK_MESSAGE(agent->follower().finished(), "Agent above its route never arrived");
    CHECK(length(object.position() - Vec3{1, 1, 1}) < .06F);
    CHECK(object.position().y == 1);

    // The codec stores the plane, and the restored agent steers in it.
    ComponentCodecs codecs;
    n::add_component_codec(codecs);
    agent->set_route(route);
    auto restored = Prefab::deserialize(Prefab::capture(object, codecs).serialize({}), {}, codecs).instantiate(scene);
    auto copy = restored.get_component<n::Agent>();
    CHECK(copy->settings().speed == 2);
    CHECK(copy->settings().arrival_distance == n::default_arrival_distance);
    CHECK(copy->settings().plane == n::SteerPlane::xz);
    CHECK(copy->follower().route().size() == route.size());
    restored.set_position({-2, 5, -1});
    n::update_agents(scene, step);
    CHECK(copy->follower().next() == 1u);
    CHECK(copy->desired_velocity().y == 0);
    CHECK(length(copy->desired_velocity()) == doctest::Approx(2));
}

TEST_CASE("Set navigation derives each scene's intent and never runs inside a phase") {
    HookCounts counts;
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto parent = first->create(), early = first->create(), late = second->create();
    early.set_parent(parent);
    late.set_position({5, 0, 0});
    constexpr n::SteerSettings settings{.speed = 2, .arrival_distance = 0};
    auto a = early.add_component<n::Agent>(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}}, settings);
    auto b = late.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}}, settings);
    auto probe = early.add_component<DriverProbe>(&first.get(), &scenes, &counts, set_updating);
    early.add_component<DriverConstruction>(first.get(), scenes);
    n::update_agents(scenes, .1);
    // Each scene's agent derives its own intent, and navigation moves no object.
    CHECK(a->follower().next() == 1u);
    CHECK(b->follower().next() == 1u);
    CHECK(a->desired_velocity().x == 2);
    CHECK(b->desired_velocity().x == 2);
    CHECK(early.position().x == 0);
    CHECK(late.position().x == 5);
    // Navigation runs no lifecycle or component hook.
    CHECK(counts.enabled == 0);
    CHECK(counts.frame == 0);
    CHECK(counts.fixed == 0);
    CHECK(counts.late == 0);

    // A late failure preserves earlier arrival cursors, velocity and disablement.
    early.set_position({1, 0, 0});
    late.set_position({2e6F, 0, 0});
    CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), position_range, std::invalid_argument);
    CHECK(a->follower().next() == 1u);
    CHECK(b->follower().next() == 1u);
    CHECK(a->desired_velocity().x == 2);
    CHECK(b->desired_velocity().x == 2);
    a.set_enabled(false);
    CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), position_range, std::invalid_argument);
    CHECK_MESSAGE(a->desired_velocity().x == 2, "Invalid later scene partially cleared disabled intent");
    late.set_position({5, 0, 0});
    n::update_agents(scenes, .1);
    // The disabled agent keeps its route without affecting the other scene.
    CHECK(length(a->desired_velocity()) == 0);
    CHECK(a->follower().next() == 1u);
    CHECK(b->desired_velocity().x == 2);
    a.set_enabled(true);
    parent.set_active(false);
    n::update_agents(scenes, .1);
    CHECK(length(a->desired_velocity()) == 0);
    CHECK_MESSAGE(a->follower().next() == 1u, "Inactive hierarchy consumed a route across the scene set");
    parent.set_active(true);
    n::update_agents(scenes, .1);
    CHECK_MESSAGE(a->follower().finished(), "Reactivated set agent failed to observe arrival");
    CHECK(length(a->desired_velocity()) == 0);
    a->set_route({{1, 0, 0}, {2, 0, 0}});
    n::update_agents(scenes, .1);
    CHECK(a->follower().next() == 1u);
    CHECK(a->desired_velocity().x == 2);

    scenes.update(.1);
    scenes.fixed_update(.1);
    probe->set_rejection = member_callbacks;
    first->update(.1);
    first->fixed_update(.1);
    // The boundary checks above leave component scheduling intact.
    CHECK(counts.enabled == 1);
    CHECK(counts.frame == 2);
    CHECK(counts.fixed == 2);
    CHECK(counts.late == 2);

    struct Marker {};
    int decoded = 0;
    ComponentCodecs codecs;
    codecs.add<Marker>(
        "test.navigation-marker.v1", [](const Marker &, const ObjectReferences &) { return "{}"; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), set_changing, std::logic_error);
            ++decoded;
            object.add_component<Marker>();
        });
    Scene source;
    source.create().add_component<Marker>();
    const auto loaded = scenes.load("loaded", serialize_scene(source, {}, codecs), {}, codecs);
    CHECK(loaded);
    CHECK_MESSAGE(decoded == 1, "Set loading boundary did not execute the navigation regression");

    SceneSet empty;
    CHECK_NOTHROW(n::update_agents(empty, .1));
    for (const auto seconds : {0., -1., std::nextafter(.1, 1.), std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(seconds);
        CHECK_THROWS_WITH_AS(n::update_agents(empty, seconds), step_range, std::invalid_argument);
    }
}

// The decoder rejects payloads over 16 MiB, so capture must too, or a long route is saved but can never be
// restored. A coordinate such as 0.1 prints with 17 significant digits, so 300,000 waypoints exceed it.
TEST_CASE("A route too large to restore is not captured") {
    constexpr std::size_t waypoints = 300'000;
    constexpr float spacing = .1F;
    std::vector<Vec3> route(waypoints);
    for (std::size_t i = 0; i < waypoints; ++i)
        route[i] = {static_cast<float>(i) * spacing, spacing, spacing};
    Scene scene;
    auto object = scene.create();
    object.add_component<n::Agent>(std::move(route));
    ComponentCodecs codecs;
    n::add_component_codec(codecs);
    CHECK_THROWS_WITH_AS(codecs.capture(object, {}), "Navigation agent payload exceeds 16 MiB", std::invalid_argument);
}
