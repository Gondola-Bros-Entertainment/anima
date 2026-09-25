#include "component_payloads.hpp"
#include <anima/physics_scene.hpp>
#include <anima/prefab.hpp>
#include <iostream>
using namespace anima;
namespace p = anima::physics;
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
    check(rejected, "Expected rejection");
}
struct Remove {
    GameObject owner;
    void on_fixed_update(double) { owner.remove_component<p::RigidBody>(); }
};
void collider_documents() {
    p::World world({{0, 0, 0}, 32});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    auto object = scene.create("offset hull assembly");
    object.set_position({10, 0, 0});
    p::BodySettings settings;
    settings.collider.shape = p::Shape::compound;
    p::ColliderChild child;
    child.pose.position = {2, 0, 0};
    child.pose.rotation = {0, 0, .70710678F, .70710678F};
    child.collider.shape = p::Shape::convex_hull;
    child.collider.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    settings.collider.children.push_back(child);
    settings.motion = p::Motion::dynamic;
    auto rigid = object.add_component<p::RigidBody>(world, settings);
    check(length(rigid->body().pose().position - object.position()) < .001F,
          "Scene compound construction published center of mass");
    rigid->body().set_angular_velocity({0, 0, 2});
    p::step(scene, world, .1);
    check(length(object.position() - rigid->body().pose().position) < .001F,
          "Scene driver published compound center of mass");
    check(length(point(object.world_matrix(), {1.75F, .5F, .25F}) - Vec3{11.75F, .5F, .25F}) < .002F,
          "Compound scene transform moved analytic center of mass");
    rigid->body().set_angular_velocity({});
    const auto document = serialize_scene(scene, {}, codecs);
    auto loaded = load_scene(document, {}, codecs);
    const auto restored = loaded->components<p::RigidBody>();
    check(restored.size() == 1 && restored[0]->settings().collider.children.size() == 1 &&
              restored[0]->settings().collider.children[0].collider.vertices.size() == 4,
          "Scene roundtrip lost compound hull geometry");
    check(length(restored[0]->body().pose().position - object.position()) < .001F,
          "Scene roundtrip changed compound authored origin");
    auto prefab = Prefab::deserialize(Prefab::capture(object, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene, matrix({{0, 0, 5}, {0, 0, 0, 1}, {1, 1, 1}}));
    auto second = prefab.instantiate(scene, matrix({{0, 0, 10}, {0, 0, 0, 1}, {1, 1, 1}}));
    const auto independent = second.get_component<p::RigidBody>()->body();
    first.destroy();
    object.destroy();
    loaded.reset();
    check(independent.valid() && world.size() == 1, "Compound prefab instances share body lifetime");
    second.destroy();
    check(world.size() == 0, "Compound scene teardown retained backend geometry/bodies");

    settings.center_of_mass = Vec3{0, -2, 0};
    auto authored = scene.create("authored mass center");
    authored.set_position({4, 0, 0});
    authored.add_component<p::RigidBody>(world, settings);
    auto authored_copy = Prefab::deserialize(Prefab::capture(authored, codecs).serialize({}), {}, codecs)
                             .instantiate(scene, matrix({{0, 0, 5}, {0, 0, 0, 1}, {1, 1, 1}}));
    const auto mass_component = authored_copy.get_component<p::RigidBody>();
    check(mass_component->settings().center_of_mass.has_value() &&
              length(mass_component->body().local_center_of_mass() - Vec3{0, -2, 0}) < .001F &&
              length(mass_component->body().world_center_of_mass() - Vec3{4, -2, 5}) < .001F,
          "Prefab lost absolute authored center of mass");
    mass_component->body().set_angular_velocity({0, 0, 2});
    p::step(scene, world, .1);
    check(length(point(authored_copy.world_matrix(), {0, -2, 0}) - Vec3{4, -2, 5}) < .002F,
          "Scene synchronization moved explicit center of mass");
    authored.destroy();
    authored_copy.destroy();

    // A later malformed component must roll back an already-created earlier body.
    auto root = scene.create();
    settings.motion = p::Motion::stationary;
    root.add_component<p::RigidBody>(world, settings);
    const auto valid = Prefab::capture(root, codecs);
    auto nodes = std::vector<Prefab::Node>(valid.nodes().begin(), valid.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes.back().parent = 0;
    const auto payload = nodes[0].components[0].state;
    const auto changed = [&](std::string_view before, std::string_view after) {
        auto result = payload;
        const auto at = result.find(before);
        check(at != std::string::npos, "Malformed compound test did not find target field");
        result.replace(at, before.size(), after);
        return result;
    };
    auto malformed = invalid_component_payloads(payload, "children");
    malformed.push_back(changed("\"children\":[{", "\"children\":[{\"children\":[],"));
    malformed.push_back(changed("\"shape\":4", "\"shape\":5"));
    malformed.push_back(changed("\"shape\":4", "\"shape\":3"));
    malformed.push_back(changed("\"position\":[2.0,0.0,0.0]", "\"position\":[2000000,0,0]"));
    malformed.push_back(changed("\"rotation\":[", "\"rotation\":[false,"));
    malformed.push_back(changed("\"vertices\":[[", "\"vertices\":[[null,"));
    malformed.push_back(changed("\"children\":[{", "\"children\":[{\"shape\":0,"));
    malformed.push_back(changed("\"children\":[{", "\"children\":[{\"sh\\u0061pe\":0,"));
    malformed.push_back(changed("\"center_of_mass\":[0.0,-2.0,0.0]", "\"center_of_mass\":false"));
    malformed.push_back(changed("\"center_of_mass\":[0.0,-2.0,0.0]", "\"center_of_mass\":[2000000,0,0]"));
    for (const auto &state : malformed) {
        nodes[1].components[0].state = state;
        rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
        check(world.size() == 1 && scene.size() == 1, "Malformed compound restoration leaked staged bodies");
    }
}
void run() {
    collider_documents();
    p::World world;
    {
        Scene scene;
        auto floor = scene.create("floor");
        floor.set_position({0, -.5F, 0});
        p::BodySettings s;
        s.collider.half_extent = {10, .5F, 10};
        floor.add_component<p::RigidBody>(world, s);
        auto ball = scene.create("ball");
        ball.set_position({0, 4, 0});
        s.collider.shape = p::Shape::sphere;
        s.motion = p::Motion::dynamic;
        auto rigid = ball.add_component<p::RigidBody>(world, s);
        for (int i = 0; i < 240; ++i) {
            scene.fixed_update(1. / 60);
            p::step(scene, world, 1. / 60);
        }
        check(std::abs(ball.position().y - .5F) < .03F, "Scene dynamic pose not synchronized");
        rigid.set_enabled(false);
        p::step(scene, world, 1. / 60);
        check(!rigid->body().enabled(), "Disabled component remains active");
        rigid.set_enabled(true);
        p::step(scene, world, 1. / 60);
        check(rigid->body().enabled(), "Component reenable failed");
        ComponentCodecs codecs;
        p::add_component_codec(codecs, world);
        ball.set_active(false);
        p::step(scene, world, 1. / 60);
        check(rigid.enabled() && !rigid->body().enabled(), "Inactive object retained physics participation");
        ball.set_active(true);
        p::step(scene, world, 1. / 60);
        check(rigid->body().enabled(), "Object activation did not restore physics participation");
        rigid->body().set_angular_velocity({0, 1, 0});
        auto prefab = Prefab::capture(ball, codecs);
        const auto text = prefab.serialize({});
        auto restored = Prefab::deserialize(text, {}, codecs).instantiate(scene);
        check(world.size() == 3 && restored.has_component<p::RigidBody>(), "Prefab did not restore independent body");
        const auto copy = restored.get_component<p::RigidBody>()->body();
        check(copy.angular_velocity().y == 1, "Prefab lost angular velocity");
        restored.destroy();
        check(!copy.valid() && world.size() == 2, "Prefab body leaked");
        ball.add_component<Remove>();
        scene.fixed_update(1. / 60);
        p::step(scene, world, 1. / 60);
        check(!rigid && world.size() == 1, "Removal during callback leaked body");
        auto bad = scene.create();
        bad.set_transform({{}, {0, 0, 0, 1}, {2, 1, 1}});
        rejects([&] { bad.add_component<p::RigidBody>(world, s); });
        check(world.size() == 1, "Failed construction leaked body");
        auto child = scene.create();
        child.set_parent(floor);
        rejects([&] { child.add_component<p::RigidBody>(world, s); });
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
    check(world.size() == 0, "Scene teardown leaked physics bodies");
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
    ComponentCodecs expired;
    Scene survivor;
    p::Body body;
    {
        p::World temporary;
        p::add_component_codec(expired, temporary);
        auto object = survivor.create();
        body = object.add_component<p::RigidBody>(temporary)->body();
    }
    check(!body.valid(), "World destruction left component body alive");
    rejects([&] {
        const std::vector<ComponentData> data{{"anima.rigid-body.v2", "{}", true}};
        expired.restore(survivor.create(), data, {});
    });
    {
        Scene scene;
        ComponentCodecs codecs;
        p::add_component_codec(codecs, world);
        auto root = scene.create();
        root.add_component<p::RigidBody>(world);
        auto prefab = Prefab::capture(root, codecs);
        auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
        nodes.push_back(nodes[0]);
        nodes.back().key = {};
        nodes[1].parent = 0;
        const auto before = scene.size();
        for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "motion")) {
            nodes[1].components[0].state = payload;
            rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
            check(world.size() == 1 && scene.size() == before, "Prefab rollback leaked bodies or objects");
        }
    }
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS physics scene lifecycle, prefab and rollback\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
