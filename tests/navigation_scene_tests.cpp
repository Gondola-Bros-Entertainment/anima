#include "component_payloads.hpp"
#include <anima/navigation_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <iostream>
#include <limits>
namespace n = anima::navigation;
using namespace anima;
namespace {
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Expected navigation scene rejection");
}
template <class F> void rejects_driver(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::logic_error &) {
        caught = true;
    }
    check(caught, "Navigation driver accepted an incomplete scene phase");
}
struct HookCounts {
    int enabled{}, frame{}, fixed{}, late{};
};
struct DriverProbe {
    Scene *scene;
    SceneSet *scenes;
    HookCounts *counts;
    void reject_nested() const {
        rejects_driver([&] { n::update_agents(*scene, .1); });
        rejects_driver([&] { n::update_agents(*scenes, .1); });
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
        rejects_driver([&] { n::update_agents(scene, .1); });
        rejects_driver([&] { n::update_agents(scenes, .1); });
    }
};
void scene_set_driver() {
    HookCounts counts;
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto parent = first->create(), early = first->create(), late = second->create();
    early.set_parent(parent);
    late.set_position({5, 0, 0});
    auto a = early.add_component<n::Agent>(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}}, 2.F, 0.F);
    auto b = late.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}}, 2.F, 0.F);
    early.add_component<DriverProbe>(&first.get(), &scenes, &counts);
    early.add_component<DriverConstruction>(first.get(), scenes);
    n::update_agents(scenes, .1);
    check(a->follower().next() == 1 && b->follower().next() == 1 && a->desired_velocity().x == 2 &&
              b->desired_velocity().x == 2 && early.position().x == 0 && late.position().x == 5,
          "Scene set navigation failed to derive independent intent without moving objects");
    check(counts.enabled == 0 && counts.frame == 0 && counts.fixed == 0 && counts.late == 0,
          "Navigation driver ran lifecycle or component hooks");

    // A late failure preserves earlier arrival cursors, velocity and disablement.
    early.set_position({1, 0, 0});
    late.set_position({2e6F, 0, 0});
    rejects([&] { n::update_agents(scenes, .1); });
    check(a->follower().next() == 1 && b->follower().next() == 1 && a->desired_velocity().x == 2 &&
              b->desired_velocity().x == 2,
          "Invalid later scene partially consumed a route or published velocity");
    a.set_enabled(false);
    rejects([&] { n::update_agents(scenes, .1); });
    check(a->desired_velocity().x == 2, "Invalid later scene partially cleared disabled intent");
    late.set_position({5, 0, 0});
    n::update_agents(scenes, .1);
    check(length(a->desired_velocity()) == 0 && a->follower().next() == 1 && b->desired_velocity().x == 2,
          "Disabled set agent consumed its route or affected another scene");
    a.set_enabled(true);
    parent.set_active(false);
    n::update_agents(scenes, .1);
    check(length(a->desired_velocity()) == 0 && a->follower().next() == 1,
          "Inactive hierarchy consumed a route across the scene set");
    parent.set_active(true);
    n::update_agents(scenes, .1);
    check(a->follower().finished() && length(a->desired_velocity()) == 0,
          "Reactivated set agent failed to observe arrival");
    a->set_route({{1, 0, 0}, {2, 0, 0}});
    n::update_agents(scenes, .1);
    check(a->follower().next() == 1 && a->desired_velocity().x == 2,
          "Set agent did not resume intent after reactivation");

    scenes.update(.1);
    scenes.fixed_update(.1);
    first->update(.1);
    first->fixed_update(.1);
    check(counts.enabled == 1 && counts.frame == 2 && counts.fixed == 2 && counts.late == 2,
          "Navigation boundary checks interfered with component scheduling");

    struct Marker {};
    int decoded = 0;
    ComponentCodecs codecs;
    codecs.add<Marker>(
        "test.navigation-marker.v1", [](const Marker &, const ObjectReferences &) { return "{}"; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            rejects_driver([&] { n::update_agents(scenes, .1); });
            ++decoded;
            object.add_component<Marker>();
        });
    Scene source;
    source.create().add_component<Marker>();
    const auto loaded = scenes.load("loaded", serialize_scene(source, {}, codecs), {}, codecs);
    check(loaded && decoded == 1, "Set loading boundary did not execute the navigation regression");

    SceneSet empty;
    n::update_agents(empty, .1);
    for (const auto seconds : {0., -1., std::nextafter(.1, 1.), std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { n::update_agents(empty, seconds); });
}
void run() {
    Scene scene;
    auto parent = scene.create();
    parent.set_position({5, 0, 0});
    auto object = scene.create();
    object.set_parent(parent, ReparentMode::keep_local);
    auto agent = object.add_component<n::Agent>(std::vector<Vec3>{{5, 0, 0}, {6, 0, 0}}, 2.F, 0.F);
    n::update_agents(scene, .1);
    check(agent->follower().next() == 1 && agent->desired_velocity().x == 2 && object.position().x == 5 &&
              object.local_position().x == 0,
          "Navigation failed world pose or moved object without application authority");
    for (const auto invalid : {-1.F, std::nextafter(10'000.F, std::numeric_limits<float>::infinity()),
                               std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        rejects([&] { agent->set_speed(invalid); });
        rejects([&] { agent->set_arrival_distance(invalid); });
        check(agent->speed() == 2 && agent->arrival_distance() == 0 && agent->desired_velocity().x == 2 &&
                  agent->follower().next() == 1,
              "Rejected navigation settings changed accepted state");
    }
    Scene empty;
    rejects([&] { n::update_agents(empty, 0); });
    rejects([&] { n::update_agents(empty, std::numeric_limits<double>::quiet_NaN()); });
    agent.set_enabled(false);
    n::update_agents(scene, .1);
    check(length(agent->desired_velocity()) == 0 && agent->follower().next() == 1,
          "Disabled agent retained motion/consumed route");
    agent.set_enabled(true);
    parent.set_active(false);
    n::update_agents(scene, .1);
    check(agent.enabled() && length(agent->desired_velocity()) == 0 && agent->follower().next() == 1,
          "Inactive hierarchy retained navigation intent");
    parent.set_active(true);
    n::update_agents(scene, .1);
    check(agent->desired_velocity().x == 2, "Navigation did not resume with hierarchy");
    agent.set_enabled(false);
    ComponentCodecs codecs;
    n::add_component_codec(codecs);
    auto prefab = Prefab::capture(object, codecs);
    auto restored = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    auto copy = restored.get_component<n::Agent>();
    check(!copy.enabled() && copy->follower().next() == 1 && copy->speed() == 2,
          "Agent persistence lost state/enablement");
    copy.set_enabled(true);
    restored.set_position({6, 0, 0});
    n::update_agents(scene, .1);
    check(copy->follower().finished() && !agent->follower().finished(), "Prefab cursor state aliased source");
    auto bad = scene.create();
    auto bad_agent = bad.add_component<n::Agent>(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}});
    bad.set_position({2e6F, 0, 0});
    agent.set_enabled(true);
    object.set_position({6, 0, 0});
    rejects([&] { n::update_agents(scene, .1); });
    check(agent->follower().next() == 1 && bad_agent->follower().next() == 0,
          "Invalid snapshot partially consumed routes");
    bad.destroy();
    n::update_agents(scene, .1);
    check(agent->follower().finished(), "Valid snapshot did not consume observed arrival");
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    auto before = scene.size();
    for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "speed")) {
        nodes[1].components[0].state = payload;
        rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
        check(scene.size() == before, "Navigation prefab rollback leaked objects");
    }
    object.destroy();
    restored.destroy();
    check(!agent && !copy, "Navigation component lifetime stale");
}
} // namespace
int main() {
    try {
        run();
        scene_set_driver();
        std::cout << "PASS navigation scene/set intent, phase boundaries, enablement, prefab and rollback\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
