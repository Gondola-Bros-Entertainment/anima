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
#include <string_view>
#include <utility>
#include <vector>

namespace n = anima::navigation;
using namespace anima;
namespace {
constexpr auto idle_scene = "Scene drivers require an idle live scene";
constexpr auto set_scheduling = "Scene drivers cannot run during set mutation or scheduling";
constexpr auto step_range = "Navigation step must be in [0.000001, 0.1] seconds";
constexpr auto position_range = "Navigation position outside finite supported range";
constexpr auto invalid_settings = "Invalid navigation speed/arrival distance";
constexpr auto duplicate_field = "Duplicate JSON document field";
struct HookCounts {
    int enabled{}, frame{}, fixed{}, late{};
};
struct DriverProbe {
    Scene *scene;
    SceneSet *scenes;
    HookCounts *counts;
    // What the set driver reports from these hooks: set scheduling when the set runs them, and an
    // updating scene when that scene runs them itself.
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
        CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), idle_scene, std::logic_error);
    }
};
} // namespace

TEST_CASE("An agent steers from its world pose and keeps its state through rejections and persistence") {
    Scene scene;
    auto parent = scene.create();
    parent.set_position({5, 0, 0});
    auto object = scene.create();
    object.set_parent(parent, ReparentMode::keep_local);
    auto agent = object.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}}, 2.F, 0.F);
    n::update_agents(scene, .1);
    // Navigation reads the world pose and leaves moving the object to the application.
    CHECK(agent->follower().next() == 1u);
    CHECK(agent->desired_velocity().x == 2);
    CHECK(object.position().x == 5);
    CHECK(object.local_position().x == 0);
    for (const auto invalid : {-1.F, std::nextafter(10'000.F, std::numeric_limits<float>::infinity()),
                               std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(invalid);
        CHECK_THROWS_WITH_AS(agent->set_speed(invalid), invalid_settings, std::invalid_argument);
        CHECK_THROWS_WITH_AS(agent->set_arrival_distance(invalid), invalid_settings, std::invalid_argument);
        // Rejected settings leave the accepted state unchanged.
        CHECK(agent->speed() == 2);
        CHECK(agent->arrival_distance() == 0);
        CHECK(agent->desired_velocity().x == 2);
        CHECK(agent->follower().next() == 1u);
    }
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
    CHECK(copy->speed() == 2);
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
    const auto payloads = invalid_component_payloads(nodes[0].components[0].state, "speed");
    // invalid_component_payloads returns an empty object, the field renamed, an unknown field, the field
    // duplicated, the duplicate spelled with an escape, and an unknown field nested past the depth limit.
    const std::array errors{"Missing JSON field: route",
                            "Missing JSON field: speed",
                            "Unknown JSON field: unexpected",
                            duplicate_field,
                            duplicate_field,
                            "JSON document exceeds nesting limit"};
    REQUIRE(payloads.size() == errors.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        CAPTURE(i);
        nodes[1].components[0].state = payloads[i];
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), errors[i], std::invalid_argument);
        CHECK_MESSAGE(scene.size() == before, "Navigation prefab rollback leaked objects");
    }
    object.destroy();
    restored.destroy();
    // Component handles expire with their objects.
    CHECK_FALSE(agent);
    CHECK_FALSE(copy);
}

TEST_CASE("Set navigation derives each scene's intent and never runs inside a phase") {
    HookCounts counts;
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto parent = first->create(), early = first->create(), late = second->create();
    early.set_parent(parent);
    late.set_position({5, 0, 0});
    auto a = early.add_component<n::Agent>(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}}, 2.F, 0.F);
    auto b = late.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}}, 2.F, 0.F);
    auto probe = early.add_component<DriverProbe>(&first.get(), &scenes, &counts, set_scheduling);
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
    probe->set_rejection = idle_scene;
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
            CHECK_THROWS_WITH_AS(n::update_agents(scenes, .1), set_scheduling, std::logic_error);
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
