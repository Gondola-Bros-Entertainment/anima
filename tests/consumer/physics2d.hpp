#pragma once
#include <anima/physics2d.hpp>
#include <cmath>
#include <stdexcept>
#ifdef B2_IS_NULL
#error Box2D backend API leaked into the consumer
#endif
#ifdef CONSUMER_PHYSICS2D_SCENE
#include <anima/physics2d_scene.hpp>
#include <anima/prefab.hpp>
#endif
inline void consume_physics2d() {
    namespace p = anima::physics2d;
    p::World world;
    p::BodySettings floor;
    floor.collider.half_extent = {5, .5F};
    floor.pose.position = {0, -.5F};
    const auto ground = world.create(floor);
    p::BodySettings falling;
    falling.motion = p::Motion::dynamic;
    falling.collider.shape = p::Shape::capsule;
    falling.pose.position = {0, 3};
    auto body = world.create(falling);
    for (int i = 0; i < 240; ++i)
        world.step(1. / 60);
    if (std::abs(body.pose().position.y - 1) > .03F || body.awake())
        throw std::runtime_error("Independent 2D capsule did not land/sleep");
    p::QueryFilter filter;
    filter.ignore = body;
    const auto hit = world.raycast({0, 3}, {0, -6}, filter);
    if (!hit || !(hit->body == ground) || hit->normal.y < .99F)
        throw std::runtime_error("Independent 2D query failed");
    body.remove();
#ifdef CONSUMER_PHYSICS2D_SCENE
    anima::Scene scene;
    auto object = scene.create("independent 2D body");
    object.set_position({2, 3, 4});
    object.add_component<p::RigidBody>(world, falling);
    anima::ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    auto prefab = anima::Prefab::capture(object, codecs);
    auto copy = anima::Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    p::step(scene, world, 1. / 60);
    if (copy.position().z != 4 || copy.position().y >= 3 || world.size() != 3)
        throw std::runtime_error("Independent 2D prefab/synchronization failed");
    object.destroy();
    copy.destroy();
    if (world.size() != 1)
        throw std::runtime_error("Independent 2D scene teardown leaked bodies");
#endif
}
