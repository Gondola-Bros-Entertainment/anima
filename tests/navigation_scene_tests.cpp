#include "component_payloads.hpp"
#include <anima/navigation_scene.hpp>
#include <anima/prefab.hpp>
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
        std::cout << "PASS navigation scene intent, depth, enablement, prefab and rollback\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
