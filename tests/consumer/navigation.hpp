#pragma once
#include <anima/navigation.hpp>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include <anima/navigation_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#endif
inline void consume_navigation() {
    namespace n = anima::navigation;
    n::Grid grid{3, 3, {}, 1, {1, 1, 1, 1, 0, 1, 1, 1, 1}, false};
    const auto graph = n::make_grid(grid);
    const auto start = n::grid_node(grid, {0, 1, 0}), goal = n::grid_node(grid, {2.2F, 1, 1.8F});
    if (start != 0u || goal != 8u || n::grid_node(grid, {3, 0, 0}))
        throw std::runtime_error("Independent grid lookup failed");
    const auto path = n::find_path(graph, *start, *goal);
    if (path.status != n::PathStatus::found || path.cost != 4)
        throw std::runtime_error("Independent navigation route failed");
    n::Follower follower(n::waypoints(graph, path));
    // A position one unit above the grid steers in the XZ plane and stays at its height.
    anima::Vec3 position{0, 1, 0};
    constexpr n::SteerSettings steering{.speed = 2, .arrival_distance = .001F, .plane = n::SteerPlane::xz};
    for (unsigned i = 0; i < 100 && !follower.finished(); ++i)
        position = position + follower.steer(position, .1, steering) * .1F;
    if (!follower.finished() || anima::length(position - anima::Vec3{2, 1, 2}) > .01F)
        throw std::runtime_error("Independent route following failed");
#ifdef CONSUMER_ASSETS
    anima::SceneSet scenes;
    auto persistent = scenes.create("persistent"), level = scenes.create("level");
    auto object = persistent->create("independent route follower");
    object.add_component<n::Agent>(n::waypoints(graph, path), n::SteerSettings{.speed = 2});
    anima::ComponentCodecs codecs;
    n::add_component_codec(codecs);
    const auto prefab = anima::Prefab::capture(object, codecs);
    auto restored = anima::Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(level.get());
    auto original_agent = object.get_component<n::Agent>(), restored_agent = restored.get_component<n::Agent>();
    n::update_agents(scenes, .1);
    if (anima::length(restored_agent->desired_velocity()) < 1 || anima::length(original_agent->desired_velocity()) < 1)
        throw std::runtime_error("Independent agent scene set/prefab failed");
    restored.set_active(false);
    n::update_agents(scenes, .1);
    if (anima::length(restored_agent->desired_velocity()) != 0 || anima::length(original_agent->desired_velocity()) < 1)
        throw std::runtime_error("Independent agent activation leaked across scenes");
    restored.set_active(true);
    n::update_agents(scenes, .1);
    if (anima::length(restored_agent->desired_velocity()) < 1)
        throw std::runtime_error("Independent agent reactivation lost intent");
    scenes.unload(level);
    n::update_agents(scenes, .1);
    if (restored_agent || anima::length(original_agent->desired_velocity()) < 1)
        throw std::runtime_error("Independent agent scene unload retained an attachment");
#endif
}
