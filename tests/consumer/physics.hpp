#pragma once
#include <anima/physics.hpp>
#ifdef JPH_VERSION_MAJOR
#error Jolt implementation definitions leaked into the consumer
#endif
inline void consume_physics() {
    anima::physics::World world;
    anima::physics::BodySettings settings;
    settings.collider.half_extent = {5, .5F, 5};
    settings.pose.position = {0, -.5F, 0};
    auto floor = world.create(settings);
    settings.collider.shape = anima::physics::Shape::sphere;
    settings.motion = anima::physics::Motion::dynamic;
    settings.pose.position = {0, 2, 0};
    auto ball = world.create(settings);
    for (int i = 0; i < 120; ++i)
        world.step(1. / 60);
    if (std::abs(ball.pose().position.y - .5F) > .03F)
        throw std::runtime_error("External rigid body did not land");
    ball.remove();
    floor.remove();
    anima::physics::Body assembly;
    {
        anima::physics::BodySettings parts;
        parts.motion = anima::physics::Motion::dynamic;
        parts.collider.shape = anima::physics::Shape::compound;
        anima::physics::ColliderChild child;
        child.pose.position = {2, 0, 0};
        child.pose.rotation = {0, 0, .70710678F, .70710678F};
        child.collider.shape = anima::physics::Shape::convex_hull;
        child.collider.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        parts.collider.children.push_back(child);
        parts.center_of_mass = anima::Vec3{0, -1, 0};
        assembly = world.create(parts);
    }
    if (anima::length(assembly.local_center_of_mass() - anima::Vec3{0, -1, 0}) > .001F)
        throw std::runtime_error("External compound lost its authored center of mass");
    const auto hit = world.raycast({-1, .25F, .1F}, {6, 0, 0});
    if (!hit || hit->body != assembly || hit->point.x < 1 || hit->point.x > 2)
        throw std::runtime_error("External compound lost owned child geometry/rotation");
    auto copy = assembly;
    assembly.remove();
    if (copy.valid() || world.size() != 0)
        throw std::runtime_error("External compound body lifetime was not independent of settings");
}
