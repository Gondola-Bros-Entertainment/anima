#pragma once
#include <anima/navigation.hpp>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include <anima/navigation_scene.hpp>
#include <anima/prefab.hpp>
#endif
inline void consume_navigation() {
    namespace n = anima::navigation;
    n::Grid grid{3, 3, {}, 1, {1, 1, 1, 1, 0, 1, 1, 1, 1}, false};
    const auto graph = n::make_grid(grid);
    const auto path = n::find_path(graph, 0, 8);
    if (path.status != n::PathStatus::found || path.cost != 4)
        throw std::runtime_error("Independent navigation route failed");
    n::Follower follower(n::waypoints(graph, path));
    anima::Vec3 position{};
    for (unsigned i = 0; i < 100 && !follower.finished(); ++i)
        position = position + follower.steer(position, 2, .1, .001F) * .1F;
    if (!follower.finished() || anima::length(position - anima::Vec3{2, 0, 2}) > .01F)
        throw std::runtime_error("Independent route following failed");
#ifdef CONSUMER_ASSETS
    anima::Scene scene;
    auto object = scene.create("independent route follower");
    object.add_component<n::Agent>(n::waypoints(graph, path), 2.F);
    anima::ComponentCodecs codecs;
    n::add_component_codec(codecs);
    const auto prefab = anima::Prefab::capture(object, codecs);
    auto restored = anima::Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    n::update_agents(scene, .1);
    if (anima::length(restored.get_component<n::Agent>()->desired_velocity()) < 1)
        throw std::runtime_error("Independent agent prefab failed");
    restored.destroy();
    object.destroy();
#endif
}
