#include "component_payloads.hpp"
#include <anima/physics_scene.hpp>
#include <anima/prefab.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace anima;
namespace p = anima::physics;
namespace {
constexpr auto duplicate_field = "Duplicate JSON document field";
constexpr auto unknown_field = "Unknown JSON field: unexpected";
constexpr auto nesting_limit = "JSON document exceeds nesting limit";
constexpr auto vector_range = "Physics vector outside finite supported range";
constexpr auto compound_child = "Compound child must be a primitive or hull";
constexpr auto three_numbers = "Physics vector requires three numbers";
struct Remove {
    GameObject owner;
    void on_fixed_update(double) { owner.remove_component<p::RigidBody>(); }
};
// A dynamic compound whose one hull child is offset from the body origin and turned a quarter about +Z.
p::BodySettings offset_hull_assembly() {
    p::BodySettings settings;
    settings.collider.shape = p::Shape::compound;
    p::ColliderChild child;
    child.pose.position = {2, 0, 0};
    child.pose.rotation = {0, 0, .70710678F, .70710678F};
    child.collider.shape = p::Shape::convex_hull;
    child.collider.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    settings.collider.children.push_back(child);
    settings.motion = p::Motion::dynamic;
    return settings;
}
} // namespace

TEST_CASE("A compound hull body keeps its authored origin through steps, scene documents and prefabs") {
    p::World world({{0, 0, 0}, 32});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    auto object = scene.create("offset hull assembly");
    object.set_position({10, 0, 0});
    auto rigid = object.add_component<p::RigidBody>(world, offset_hull_assembly());
    CHECK_MESSAGE(length(rigid->body().pose().position - object.position()) < .001F,
                  "Scene compound construction published center of mass");
    rigid->body().set_angular_velocity({0, 0, 2});
    p::step(scene, world, .1);
    CHECK_MESSAGE(length(object.position() - rigid->body().pose().position) < .001F,
                  "Scene driver published compound center of mass");
    CHECK_MESSAGE(length(point(object.world_matrix(), {1.75F, .5F, .25F}) - Vec3{11.75F, .5F, .25F}) < .002F,
                  "Compound scene transform moved analytic center of mass");
    rigid->body().set_angular_velocity({});
    const auto document = serialize_scene(scene, {}, codecs);
    auto loaded = load_scene(document, {}, codecs);
    const auto restored = loaded->components<p::RigidBody>();
    REQUIRE(restored.size() == 1u);
    REQUIRE(restored[0]->settings().collider.children.size() == 1u);
    CHECK_MESSAGE(restored[0]->settings().collider.children[0].collider.vertices.size() == 4u,
                  "Scene roundtrip lost compound hull geometry");
    CHECK_MESSAGE(length(restored[0]->body().pose().position - object.position()) < .001F,
                  "Scene roundtrip changed compound authored origin");
    auto prefab = Prefab::deserialize(Prefab::capture(object, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene, matrix({{0, 0, 5}, {0, 0, 0, 1}, {1, 1, 1}}));
    auto second = prefab.instantiate(scene, matrix({{0, 0, 10}, {0, 0, 0, 1}, {1, 1, 1}}));
    const auto independent = second.get_component<p::RigidBody>()->body();
    first.destroy();
    object.destroy();
    loaded.reset();
    CHECK_MESSAGE(independent.valid(), "Compound prefab instances share body lifetime");
    CHECK(world.size() == 1u);
    second.destroy();
    CHECK_MESSAGE(world.size() == 0u, "Compound scene teardown retained backend geometry/bodies");
}

TEST_CASE("A prefab keeps a body's absolute authored center of mass") {
    p::World world({{0, 0, 0}, 32});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    auto settings = offset_hull_assembly();
    settings.center_of_mass = Vec3{0, -2, 0};
    auto authored = scene.create("authored mass center");
    authored.set_position({4, 0, 0});
    authored.add_component<p::RigidBody>(world, settings);
    auto authored_copy = Prefab::deserialize(Prefab::capture(authored, codecs).serialize({}), {}, codecs)
                             .instantiate(scene, matrix({{0, 0, 5}, {0, 0, 0, 1}, {1, 1, 1}}));
    const auto mass_component = authored_copy.get_component<p::RigidBody>();
    CHECK_MESSAGE(mass_component->settings().center_of_mass.has_value(),
                  "Prefab lost absolute authored center of mass");
    CHECK(length(mass_component->body().local_center_of_mass() - Vec3{0, -2, 0}) < .001F);
    CHECK(length(mass_component->body().world_center_of_mass() - Vec3{4, -2, 5}) < .001F);
    mass_component->body().set_angular_velocity({0, 0, 2});
    p::step(scene, world, .1);
    CHECK_MESSAGE(length(point(authored_copy.world_matrix(), {0, -2, 0}) - Vec3{4, -2, 5}) < .002F,
                  "Scene synchronization moved explicit center of mass");
}

TEST_CASE("A malformed compound payload rolls back the prefab's earlier body") {
    p::World world({{0, 0, 0}, 32});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    auto settings = offset_hull_assembly();
    settings.center_of_mass = Vec3{0, -2, 0};
    settings.motion = p::Motion::stationary;
    auto root = scene.create();
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
        REQUIRE(at != std::string::npos);
        result.replace(at, before.size(), after);
        return result;
    };
    struct Malformed {
        std::string state;
        const char *error;
    };
    std::vector<Malformed> malformed;
    const auto generated = invalid_component_payloads(payload, "children");
    // invalid_component_payloads returns an empty object, the field renamed, an unknown field, the field
    // duplicated, the duplicate spelled with an escape, and an unknown field nested past the depth limit.
    const std::array generated_errors{"Missing JSON field: shape",
                                      "Missing JSON field: children",
                                      unknown_field,
                                      duplicate_field,
                                      duplicate_field,
                                      nesting_limit};
    REQUIRE(generated.size() == generated_errors.size());
    for (std::size_t i = 0; i < generated.size(); ++i)
        malformed.push_back({generated[i], generated_errors[i]});
    malformed.push_back(
        {changed("\"children\":[{", "\"children\":[{\"children\":[],"), "Unknown JSON field: children"});
    malformed.push_back({changed("\"shape\":\"convex_hull\"", "\"shape\":\"compound\""), compound_child});
    malformed.push_back({changed("\"shape\":\"convex_hull\"", "\"shape\":\"mesh\""), compound_child});
    malformed.push_back({changed("\"position\":[2.0,0.0,0.0]", "\"position\":[2000000,0,0]"), vector_range});
    // A value that replaces a number keeps the array's length, so only the numeric check rejects it; an added
    // number fails the length check instead.
    malformed.push_back({changed("\"rotation\":[0.0,", "\"rotation\":[false,"), "Compound rotation must be numeric"});
    malformed.push_back({changed("\"vertices\":[[0.0,", "\"vertices\":[[null,"), "Physics vector must be numeric"});
    malformed.push_back({changed("\"rotation\":[", "\"rotation\":[0.0,"), "Compound rotation requires four numbers"});
    malformed.push_back({changed("\"vertices\":[[", "\"vertices\":[[0.0,"), three_numbers});
    malformed.push_back({changed("\"children\":[{", "\"children\":[{\"shape\":0,"), duplicate_field});
    malformed.push_back({changed("\"children\":[{", "\"children\":[{\"sh\\u0061pe\":0,"), duplicate_field});
    malformed.push_back({changed("\"center_of_mass\":[0.0,-2.0,0.0]", "\"center_of_mass\":false"), three_numbers});
    malformed.push_back(
        {changed("\"center_of_mass\":[0.0,-2.0,0.0]", "\"center_of_mass\":[2000000,0,0]"), vector_range});
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        CAPTURE(i);
        nodes[1].components[0].state = malformed[i].state;
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), malformed[i].error, std::invalid_argument);
        CHECK_MESSAGE(world.size() == 1u, "Malformed compound restoration leaked staged bodies");
        CHECK(scene.size() == 1u);
    }
}

TEST_CASE("Rigid bodies synchronize scene poses and follow enablement, activity and prefabs") {
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
        CHECK_MESSAGE(std::abs(ball.position().y - .5F) < .03F, "Scene dynamic pose not synchronized");
        rigid.set_enabled(false);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE_MESSAGE(rigid->body().enabled(), "Disabled component remains active");
        rigid.set_enabled(true);
        p::step(scene, world, 1. / 60);
        CHECK_MESSAGE(rigid->body().enabled(), "Component reenable failed");
        ComponentCodecs codecs;
        p::add_component_codec(codecs, world);
        ball.set_active(false);
        p::step(scene, world, 1. / 60);
        CHECK(rigid.enabled());
        CHECK_FALSE_MESSAGE(rigid->body().enabled(), "Inactive object retained physics participation");
        ball.set_active(true);
        p::step(scene, world, 1. / 60);
        CHECK_MESSAGE(rigid->body().enabled(), "Object activation did not restore physics participation");
        rigid->body().set_angular_velocity({0, 1, 0});
        auto prefab = Prefab::capture(ball, codecs);
        const auto text = prefab.serialize({});
        auto restored = Prefab::deserialize(text, {}, codecs).instantiate(scene);
        CHECK(world.size() == 3u);
        REQUIRE_MESSAGE(restored.has_component<p::RigidBody>(), "Prefab did not restore independent body");
        const auto copy = restored.get_component<p::RigidBody>()->body();
        CHECK_MESSAGE(copy.angular_velocity().y == 1, "Prefab lost angular velocity");
        restored.destroy();
        CHECK_FALSE_MESSAGE(copy.valid(), "Prefab body leaked");
        CHECK(world.size() == 2u);
        ball.add_component<Remove>();
        scene.fixed_update(1. / 60);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE(rigid);
        CHECK_MESSAGE(world.size() == 1u, "Removal during callback leaked body");
        auto bad = scene.create();
        bad.set_transform({{}, {0, 0, 0, 1}, {2, 1, 1}});
        CHECK_THROWS_WITH_AS(bad.add_component<p::RigidBody>(world, s),
                             "Physics transforms require unit scale and no shear/reflection", std::invalid_argument);
        CHECK_MESSAGE(world.size() == 1u, "Failed construction leaked body");
        auto child = scene.create();
        child.set_parent(floor);
        CHECK_THROWS_WITH_AS(child.add_component<p::RigidBody>(world, s), "Dynamic rigid bodies must be scene roots",
                             std::invalid_argument);
    }
    CHECK_MESSAGE(world.size() == 0u, "Scene teardown leaked physics bodies");
}

TEST_CASE("A kinematic body's captured velocity restores beyond the dynamic caps") {
    // The driver moves a kinematic body to its object's pose through Body::move_kinematic, which does not clamp
    // its velocity: turning 1.6 radians in a 1/60 s step is 96 radians per second, about twice the dynamic cap.
    constexpr double tick = 1. / 60;
    constexpr float turn = 1.6F;
    constexpr float relative_tolerance = 1e-4F;
    p::World world({{0, 0, 0}, 4});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    auto spinner = scene.create("spinner");
    p::BodySettings settings;
    settings.motion = p::Motion::kinematic;
    const auto rigid = spinner.add_component<p::RigidBody>(world, settings);
    spinner.set_transform({{}, {0, std::sin(turn / 2), 0, std::cos(turn / 2)}, {1, 1, 1}});
    p::step(scene, world, tick);
    const auto spin = rigid->body().angular_velocity();
    const auto expected = turn / static_cast<float>(tick);
    REQUIRE_MESSAGE(std::abs(spin.y - expected) <= expected * relative_tolerance,
                    "The driver did not turn the kinematic body in one step");
    auto copy = Prefab::deserialize(Prefab::capture(spinner, codecs).serialize({}), {}, codecs).instantiate(scene);
    const auto restored = copy.get_component<p::RigidBody>()->body().angular_velocity();
    CHECK_MESSAGE((restored.x == spin.x && restored.y == spin.y && restored.z == spin.z),
                  "Restoring changed a kinematic body's captured angular velocity");
}

TEST_CASE("A stationary child's body follows its hierarchy's activity") {
    p::World world;
    {
        Scene scene;
        auto parent = scene.create(), child = scene.create();
        child.set_parent(parent);
        auto body = child.add_component<p::RigidBody>(world)->body();
        parent.set_active(false);
        p::step(scene, world, 1. / 60);
        CHECK_FALSE_MESSAGE(body.enabled(), "Inactive parent retained stationary child body");
        child.clear_parent();
        p::step(scene, world, 1. / 60);
        CHECK_MESSAGE(body.enabled(), "Reparent did not restore body participation");
    }
    CHECK_MESSAGE(world.size() == 0u, "Scene teardown leaked physics bodies");
}

TEST_CASE("An invalid snapshot moves no body") {
    p::World world;
    Scene scene;
    auto first = scene.create();
    auto second = scene.create();
    auto a = first.add_component<p::RigidBody>(world)->body();
    second.add_component<p::RigidBody>(world);
    first.set_position({10, 0, 0});
    second.set_position({2e6F, 0, 0});
    CHECK_THROWS_WITH_AS(p::step(scene, world, 1. / 60), "Physics position outside supported range",
                         std::invalid_argument);
    CHECK_MESSAGE(a.pose().position.x == 0, "Invalid snapshot partially moved an earlier body");
}

TEST_CASE("Bodies and codecs expire with their world") {
    ComponentCodecs expired;
    Scene survivor;
    p::Body body;
    {
        p::World temporary;
        p::add_component_codec(expired, temporary);
        auto object = survivor.create();
        body = object.add_component<p::RigidBody>(temporary)->body();
    }
    CHECK_FALSE_MESSAGE(body.valid(), "World destruction left component body alive");
    const std::vector<ComponentData> data{{"anima.rigid-body.v3", "{}", true}};
    CHECK_THROWS_WITH_AS(expired.restore(survivor.create(), data, {}), "Rigid body codec world expired",
                         std::out_of_range);
}

TEST_CASE("A malformed rigid body payload rolls back the prefab's earlier body") {
    p::World world;
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
    const auto payloads = invalid_component_payloads(nodes[0].components[0].state, "motion");
    // invalid_component_payloads returns an empty object, the field renamed, an unknown field, the field
    // duplicated, the duplicate spelled with an escape, and an unknown field nested past the depth limit.
    const std::array errors{"Missing JSON field: shape",
                            "Missing JSON field: motion",
                            unknown_field,
                            duplicate_field,
                            duplicate_field,
                            nesting_limit};
    REQUIRE(payloads.size() == errors.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        CAPTURE(i);
        nodes[1].components[0].state = payloads[i];
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), errors[i], std::invalid_argument);
        CHECK_MESSAGE(world.size() == 1u, "Prefab rollback leaked bodies or objects");
        CHECK(scene.size() == before);
    }
}

TEST_CASE("The rigid body codec keeps mass and damping and stores shape and motion by name") {
    p::World world({{0, 0, 0}, 8});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    p::BodySettings settings;
    settings.collider.half_extent = {.5F, .25F, 1};
    settings.motion = p::Motion::dynamic;
    settings.mass = 3;
    settings.linear_damping = .25F;
    settings.angular_damping = .5F;
    auto root = scene.create();
    root.add_component<p::RigidBody>(world, settings);
    const auto prefab = Prefab::capture(root, codecs);
    const auto &components = prefab.nodes()[0].components;
    REQUIRE(components.size() == 1u);
    CHECK(components[0].type == "anima.rigid-body.v3");
    const auto payload = components[0].state;
    for (const std::string_view field :
         {"\"shape\":\"box\"", "\"motion\":\"dynamic\"", "\"half_extent\":[0.5,0.25,1.0]", "\"mass\":3.0",
          "\"linear_damping\":0.25", "\"angular_damping\":0.5"}) {
        CAPTURE(field);
        CHECK(payload.find(field) != std::string::npos);
    }
    auto copy = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    const auto restored = copy.get_component<p::RigidBody>();
    const auto &s = restored->settings();
    CHECK((s.collider.shape == p::Shape::box && s.motion == p::Motion::dynamic));
    CHECK((s.collider.half_extent.x == .5F && s.collider.half_extent.y == .25F && s.collider.half_extent.z == 1));
    CHECK((s.mass == 3 && s.linear_damping == .25F && s.angular_damping == .5F));
    CHECK(std::abs(restored->body().mass() - 3) < 1e-5F);
    copy.destroy();

    // Version 2's keys, enumerator numbers and names that differ only in case are rejected.
    const auto changed = [&](std::string_view before, std::string_view after) {
        auto result = payload;
        const auto at = result.find(before);
        REQUIRE(at != std::string::npos);
        result.replace(at, before.size(), after);
        return result;
    };
    struct Malformed {
        std::string state;
        const char *error;
    };
    const std::array<Malformed, 10> malformed{{
        {changed("\"half_extent\":", "\"extent\":"), "Missing JSON field: half_extent"},
        {changed("\"angular_damping\":0.5,", ""), "Missing JSON field: angular_damping"},
        {changed("\"linear_damping\":0.25,", ""), "Missing JSON field: linear_damping"},
        {changed("\"shape\":\"box\"", "\"shape\":0"), "Unknown rigid body shape"},
        {changed("\"shape\":\"box\"", "\"shape\":\"Box\""), "Unknown rigid body shape"},
        {changed("\"motion\":\"dynamic\"", "\"motion\":2"), "Unknown rigid body motion"},
        {changed("\"motion\":\"dynamic\"", "\"motion\":null"), "Unknown rigid body motion"},
        {changed("\"linear_damping\":0.25", "\"linear_damping\":-1"), "Invalid body damping"},
        {changed("\"angular_damping\":0.5", "\"angular_damping\":61"), "Invalid body damping"},
        {changed("\"layer\":0", "\"layer\":16"), "Invalid rigid body layer"},
    }};
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        CAPTURE(i);
        const std::vector<ComponentData> data{{"anima.rigid-body.v3", malformed[i].state, true}};
        CHECK_THROWS_WITH_AS(codecs.restore(scene.create(), data, {}), malformed[i].error, std::invalid_argument);
        CHECK_MESSAGE(world.size() == 1u, "A rejected payload created a body");
    }
    const std::vector<ComponentData> version_2{{"anima.rigid-body.v2", payload, true}};
    CHECK_THROWS_WITH_AS(codecs.restore(scene.create(), version_2, {}), "Unknown serialized component type",
                         std::invalid_argument);
}

TEST_CASE("Capturing a rigid body payload over 16 MiB throws") {
    // Unreferenced mesh vertices are kept and stored. Each of these prints its coordinates in full, as
    // [-1.100000023841858,-1.100000023841858,-1.100000023841858], so the vertices take about 17.7 MB.
    constexpr std::size_t vertex_count = 300'000;
    p::World world({{0, 0, 0}, 1});
    ComponentCodecs codecs;
    p::add_component_codec(codecs, world);
    Scene scene;
    p::BodySettings settings;
    settings.collider.shape = p::Shape::mesh;
    settings.collider.vertices.assign(vertex_count, {-1.1F, -1.1F, -1.1F});
    settings.collider.vertices[0] = {0, 0, 0};
    settings.collider.vertices[1] = {0, 0, 1};
    settings.collider.vertices[2] = {1, 0, 0};
    settings.collider.indices = {0, 1, 2};
    auto object = scene.create();
    object.add_component<p::RigidBody>(world, std::move(settings));
    CHECK_THROWS_WITH_AS((void)codecs.capture(object, {}), "Rigid body payload exceeds 16 MiB", std::invalid_argument);
}
