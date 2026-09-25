#include "component_payloads.hpp"
#include <anima/physics2d_scene.hpp>
#include <anima/prefab.hpp>
#include <iostream>
using namespace anima;
namespace p = anima::physics2d;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "Expected scene rejection");
}
struct Remove {
    GameObject owner;
    void on_fixed_update(double) { owner.remove_component<p::RigidBody>(); }
};
struct Counter {
    int *count;
    void on_fixed_update(double) { ++*count; }
};
void run() {
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
        check(ticks == 240 && std::abs(ball.position().y - .5F) < .03F && ball.position().z == 7,
              "Scene tick, XY simulation or presentation depth failed");
        check(!rigid->body().awake(), "Scene synchronization woke sleeping body");
        rigid.set_enabled(false);
        p::step(scene, world, 1. / 60);
        check(!rigid->body().enabled(), "Disabled component remains active");
        ball.set_position({0, 10, 7});
        rigid.set_enabled(true);
        p::step(scene, world, 1. / 60);
        check(ball.position().y < 1 && rigid->body().enabled(), "Reenable did not restore physics pose");
        ball.set_active(false);
        p::step(scene, world, 1. / 60);
        check(rigid.enabled() && !rigid->body().enabled(), "Inactive object retained physics participation");
        ball.set_active(true);
        p::step(scene, world, 1. / 60);
        check(rigid->body().enabled(), "Object activation did not restore physics participation");
        rigid->body().set_angular_velocity(1);
        ComponentCodecs codecs;
        p::add_component_codec(codecs, world);
        const auto text = Prefab::capture(ball, codecs).serialize({});
        auto restored = Prefab::deserialize(text, {}, codecs).instantiate(scene);
        auto body = restored.get_component<p::RigidBody>()->body();
        check(world.size() == 3 && body.angular_velocity() == 1 && restored.position().z == 7,
              "2D prefab did not restore independent state");
        restored.destroy();
        check(!body.valid() && world.size() == 2, "Prefab body leaked");
        ball.add_component<Remove>();
        scene.fixed_update(1. / 60);
        p::step(scene, world, 1. / 60);
        check(!rigid && world.size() == 1, "Callback removal retained body");
        auto bad = scene.create();
        bad.set_transform({{}, {0, 0, 0, 1}, {2, 1, 1}});
        rejects([&] { bad.add_component<p::RigidBody>(world, settings); });
        bad.set_transform({{}, {.70710678F, 0, 0, .70710678F}, {1, 1, 1}});
        rejects([&] { bad.add_component<p::RigidBody>(world, settings); });
        auto child = scene.create();
        child.set_parent(floor);
        rejects([&] { child.add_component<p::RigidBody>(world, settings); });
        settings.motion = p::Motion::stationary;
        child.add_component<p::RigidBody>(world, settings);
        p::step(scene, world, 1. / 60);
        child.destroy();
    }
    {
        Scene scene;
        auto parent = scene.create(), child = scene.create();
        child.set_parent(parent);
        auto body = child.add_component<p::RigidBody>(world)->body();
        parent.set_active(false);
        p::step(scene, world, 1. / 60);
        check(!body.enabled(), "Inactive parent retained stationary child body");
        child.clear_parent();
        p::step(scene, world, 1. / 60);
        check(body.enabled(), "Reparent did not restore body participation");
    }
    check(world.size() == 0, "Scene teardown leaked 2D bodies");
    {
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
        for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "motion")) {
            nodes[1].components[0].state = payload;
            rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
            check(scene.size() == before && world.size() == 1, "Prefab rollback leaked scene/body state");
        }
    }
    {
        Scene scene;
        auto first = scene.create();
        auto second = scene.create();
        auto a = first.add_component<p::RigidBody>(world)->body();
        second.add_component<p::RigidBody>(world);
        first.set_position({10, 0, 0});
        second.set_position({2e6F, 0, 0});
        rejects([&] { p::step(scene, world, 1. / 60); });
        check(a.pose().position.x == 0, "Invalid snapshot partially moved an earlier body");
    }
    Scene survivor;
    ComponentCodecs expired;
    p::Body handle;
    {
        p::World temporary;
        p::add_component_codec(expired, temporary);
        handle = survivor.create().add_component<p::RigidBody>(temporary)->body();
    }
    check(!handle.valid(), "World teardown left body valid");
    rejects([&] {
        const std::vector<ComponentData> data{{"anima.rigid-body-2d.v1", "{}", true}};
        expired.restore(survivor.create(), data, {});
    });
    rejects([&] { p::step(survivor, world, 1. / 60); });
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS 2D physics scene, depth, prefab, teardown and rollback\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
