#include "component_payloads.hpp"
#include <anima/lighting.hpp>
#include <anima/scene_set.hpp>
#include <iostream>
#include <limits>

using namespace anima;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(float a, float b) { check(std::abs(a - b) < 2e-5F, "Lighting numeric mismatch"); }
void equal(Vec3 a, Vec3 b) {
    near(a.x, b.x);
    near(a.y, b.y);
    near(a.z, b.z);
}
void equal(const DirectionalShadow &a, const DirectionalShadow &b) {
    check(a.enabled == b.enabled && a.resolution == b.resolution, "Shadow state changed");
    equal(a.center, b.center);
    near(a.extent, b.extent);
    near(a.depth, b.depth);
    near(a.constant_bias, b.constant_bias);
    near(a.slope_bias, b.slope_bias);
}
void equal(const Environment &a, const Environment &b) {
    equal(a.sun.direction, b.sun.direction);
    equal(a.fill.direction, b.fill.direction);
    equal(a.sun.radiance, b.sun.radiance);
    equal(a.fill.radiance, b.fill.radiance);
    equal(a.ambient_sky, b.ambient_sky);
    equal(a.ambient_ground, b.ambient_ground);
    equal(a.ambient_specular, b.ambient_specular);
    equal(a.sky_zenith, b.sky_zenith);
    equal(a.sky_horizon, b.sky_horizon);
    equal(a.sky_ground, b.sky_ground);
    equal(a.fog_color, b.fog_color);
    near(a.fog_density, b.fog_density);
    near(a.exposure, b.exposure);
    check(a.sky == b.sky && a.tone_mapping == b.tone_mapping, "Environment state changed");
    equal(a.shadow, b.shadow);
    equal(a.detail_shadow, b.detail_shadow);
}
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Expected lighting rejection");
}
GameObject light(Scene &scene, Vec3 radiance = {1, 2, 3}) {
    auto object = scene.create("light");
    object.add_component<DirectionalLightComponent>(radiance);
    return object;
}
auto environment(Scene &scene, GameObject sun, GameObject fill) {
    auto component = scene.create("environment").add_component<SceneEnvironment>();
    component->sun = sun;
    component->fill = fill;
    return component;
}
void transforms_and_configuration() {
    Scene scene;
    auto sun = light(scene), fill = light(scene, {});
    auto selected = environment(scene, sun, fill);
    auto result = lighting_environment(scene);
    equal(result.sun.direction, {0, 0, 1});
    equal(result.sun.radiance, {1, 2, 3});
    equal(result.fill.radiance, {});
    auto parent = scene.create();
    parent.set_transform({.translation = {8, 5, 3}, .rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    sun.set_parent(parent, ReparentMode::keep_local);
    sun.set_local_position({300, 200, 100});
    equal(lighting_environment(scene).sun.direction, {0, 0, -1});
    sun.clear_parent();
    sun.set_transform({.rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    const auto accepted = lighting_environment(scene);
    sun.set_transform({.rotation = {0, 1, 0, 0}});
    equal(accepted, lighting_environment(scene));
    for (float scale : {0.F, -1.F, 1e-5F}) {
        sun.set_transform({.scale = {1, scale, 1}});
        rejects([&] { (void)lighting_environment(scene); });
    }
    auto shear = identity();
    shear[4] = .3F;
    sun.set_world_matrix(shear);
    rejects([&] { (void)lighting_environment(scene); });
    sun.set_world_matrix(identity());
    auto component = sun.get_component<DirectionalLightComponent>();
    for (float invalid : {-1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        for (auto value : {Vec3{invalid, 1, 1}, Vec3{1, invalid, 1}, Vec3{1, 1, invalid}}) {
            rejects([&] { component->set_radiance(value); });
            equal(component->radiance(), {1, 2, 3});
        }
    auto settings = selected->settings();
    settings.sky = true;
    settings.tone_mapping = true;
    settings.exposure = 1.5F;
    settings.shadow.enabled = true;
    settings.shadow.extent = 8;
    selected->configure(settings);
    const auto configured = lighting_environment(scene);
    near(point(directional_shadow_matrix(configured), {0, 0, 0}).z, .5F);
    for (unsigned field = 0; field < 9; ++field) {
        auto bad = settings;
        if (field == 0)
            bad.exposure = 0;
        if (field == 1)
            bad.fog_density = -1;
        if (field == 2)
            bad.ambient_specular.x = -1;
        if (field == 3)
            bad.sky_zenith.y = std::numeric_limits<float>::infinity();
        if (field == 4)
            bad.shadow.resolution = 0;
        if (field == 5)
            bad.shadow.extent = 0;
        if (field == 6)
            bad.detail_shadow.depth = 0;
        if (field == 7)
            bad.detail_shadow.center.z = std::numeric_limits<float>::quiet_NaN();
        if (field == 8)
            bad.shadow.slope_bias = -1;
        rejects([&] { selected->configure(bad); });
        equal(configured, lighting_environment(scene));
    }
    settings.detail_shadow.extent = std::numeric_limits<float>::denorm_min();
    selected->configure(settings); // Scalar-valid but cannot form a float projection.
    rejects([&] { (void)lighting_environment(scene); });
}
void selection_and_lifetime() {
    SceneSet scenes;
    auto a = scenes.create("a"), b = scenes.create("b");
    auto sun = light(a.get()), fill = light(b.get());
    rejects([&] { (void)lighting_environment(scenes); });
    auto selected = environment(b.get(), sun, fill);
    const auto accepted = lighting_environment(scenes);
    scenes.set_active(a);
    equal(accepted, lighting_environment(scenes));
    scenes.set_active(b);
    equal(accepted, lighting_environment(scenes));
    rejects([&] { (void)lighting_environment(b.get()); });
    auto extra = environment(a.get(), sun, fill);
    rejects([&] { (void)lighting_environment(scenes); });
    extra.set_enabled(false);
    equal(accepted, lighting_environment(scenes));
    selected.set_enabled(false);
    rejects([&] { (void)lighting_environment(scenes); });
    selected.set_enabled(true);
    selected.object().set_active(false);
    rejects([&] { (void)lighting_environment(scenes); });
    selected.object().set_active(true);
    auto group = a->create();
    sun.set_parent(group);
    group.set_active(false);
    rejects([&] { (void)lighting_environment(scenes); });
    group.set_active(true);
    sun.get_component<DirectionalLightComponent>().set_enabled(false);
    rejects([&] { (void)lighting_environment(scenes); });
    sun.remove_component<DirectionalLightComponent>();
    rejects([&] { (void)lighting_environment(scenes); });
    sun.add_component<DirectionalLightComponent>(Vec3{1, 2, 3});
    equal(accepted, lighting_environment(scenes));
    selected->fill = sun; // One authored light may deliberately fill both slots.
    equal(accepted, lighting_environment(scenes));
    selected->fill = fill;
    selected->sun = {};
    rejects([&] { (void)lighting_environment(scenes); });
    Scene foreign;
    selected->sun = light(foreign);
    rejects([&] { (void)lighting_environment(scenes); });
    selected->sun = a->create("not a light");
    rejects([&] { (void)lighting_environment(scenes); });
    selected->sun = sun;
    fill.destroy();
    (void)light(b.get());
    rejects([&] { (void)lighting_environment(scenes); });
    selected->fill = sun;
    scenes.unload(a);
    a = scenes.create("a");
    (void)light(a.get());
    rejects([&] { (void)lighting_environment(scenes); });
    scenes.clear();
    check(!selected, "Environment retained its unloaded scene");
    rejects([&] { (void)lighting_environment(scenes); });
}
struct Reentry {
    SceneSet *scenes;
    Scene *scene;
    unsigned *calls;
    void on_update(double) {
        rejects([&] { (void)lighting_environment(*scene); });
        rejects([&] { (void)lighting_environment(*scenes); });
        ++*calls;
    }
};
struct Construction {
    Construction(Scene &scene) {
        rejects([&] { (void)lighting_environment(scene); });
    }
};
void boundaries() {
    SceneSet scenes;
    auto scene = scenes.create("level");
    auto sun = light(scene.get());
    (void)environment(scene.get(), sun, sun);
    unsigned calls = 0;
    sun.add_component<Reentry>(&scenes, &scene.get(), &calls);
    sun.add_component<Construction>(scene.get());
    scenes.update(0);
    check(calls == 1, "Lighting test callback did not run");
    (void)lighting_environment(scenes);
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    check(position != std::string::npos, "Test payload field not found");
    value.replace(position, from.size(), to);
    return value;
}
void persistence() {
    ComponentCodecs codecs;
    add_lighting_component_codecs(codecs);
    Scene scene;
    auto root = scene.create("rig");
    auto selected = root.add_component<SceneEnvironment>();
    auto sun = light(scene), fill = light(scene, {.25F, .5F, .75F});
    sun.set_parent(root, ReparentMode::keep_local);
    fill.set_parent(root, ReparentMode::keep_local);
    sun.set_transform({.rotation = {0, 1, 0, 0}, .scale = {3, 4, 5}});
    selected->sun = sun;
    selected->fill = fill;
    EnvironmentSettings settings;
    settings.ambient_sky = {.2F, .3F, .4F};
    settings.ambient_ground = {.4F, .3F, .2F};
    settings.ambient_specular = {.5F, .6F, .7F};
    settings.sky = true;
    settings.sky_zenith = {.7F, .8F, .9F};
    settings.sky_horizon = {.9F, .8F, .7F};
    settings.sky_ground = {.6F, .5F, .4F};
    settings.fog_color = {.3F, .2F, .1F};
    settings.fog_density = .05F;
    settings.exposure = 1.5F;
    settings.tone_mapping = true;
    settings.shadow = {true, {1, 2, 3}, 20, 80, 1024, .002F, .003F};
    settings.detail_shadow = {true, {4, 5, 6}, 4, 40, 2048, .004F, .005F};
    selected->configure(settings);
    const auto prefab = Prefab::deserialize(Prefab::capture(root, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    check(first.get_component<SceneEnvironment>()->sun.id() == first.children()[0].id() &&
              first.get_component<SceneEnvironment>()->fill.id() == first.children()[1].id() &&
              second.get_component<SceneEnvironment>()->sun.id() == second.children()[0].id(),
          "Prefab lighting links did not remap");
    first.set_active(false);
    second.get_component<SceneEnvironment>().set_enabled(false);
    const auto accepted = lighting_environment(scene);
    auto restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    equal(accepted, lighting_environment(*restored));
    check(!restored->find(first.key()).active_self() &&
              !restored->find(second.key()).get_component<SceneEnvironment>().enabled(),
          "Lighting activation state lost");
    rejects([&] { add_lighting_component_codecs(codecs); });
    equal(accepted, lighting_environment(*load_scene(serialize_scene(scene, {}, codecs), {}, codecs)));
    selected->sun = second.children()[0];
    rejects([&] { (void)Prefab::capture(root, codecs); });
    selected->sun = {};
    auto unconfigured = Prefab::capture(root, codecs).instantiate(scene);
    check(!unconfigured.get_component<SceneEnvironment>()->sun.valid(), "Null light selection rebound");
    unconfigured.destroy();
    selected->sun = sun;
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    for (unsigned node : {0U, 1U}) {
        auto &data = nodes[node].components[0].state;
        const auto valid = data;
        auto invalids = invalid_component_payloads(valid, node == 0 ? "sun" : "radiance");
        invalids.push_back(std::string(64 * 1024 + 1, ' '));
        if (node == 0) {
            for (const auto bad : {"0", "\"01\"", "\"99999\""})
                invalids.push_back(
                    replace(valid, "\"sun\":\"" + sun.key().string() + "\"", "\"sun\":" + std::string(bad)));
            for (const auto bad : {"-1", "0", "1.5", "1.0", "true", "4294967296", "18446744073709551616"})
                invalids.push_back(replace(valid, "\"resolution\":2048", "\"resolution\":" + std::string(bad)));
            for (const auto bad : {"true", "\"1\"", "-1", "1e100"})
                invalids.push_back(replace(valid, "\"exposure\":1.5", "\"exposure\":" + std::string(bad)));
            invalids.push_back(replace(valid, "\"sky\":true", "\"sky\":1"));
            invalids.push_back(replace(valid, "\"enabled\":true", "\"enabled\":\"true\""));
            invalids.push_back(replace(valid, "\"center\":[4.0,5.0,6.0]", "\"center\":[4,5]"));
            invalids.push_back(replace(valid, "\"slope_bias\":", "\"slope_bias\":0,\"slope_bias\":"));
            invalids.push_back(replace(valid, "\"fog_density\":", "\"unexpected\":"));
        } else {
            for (const auto bad : {"null", "1", "[1,2]", "[1,2,3,4]", "[-1,2,3]", "[true,2,3]", "[1e100,2,3]"})
                invalids.push_back("{\"radiance\":" + std::string(bad) + "}");
        }
        for (const auto &invalid : invalids) {
            data = invalid;
            const auto size = scene.size();
            rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
            check(scene.size() == size, "Invalid lighting payload leaked staged objects");
            equal(accepted, lighting_environment(scene));
        }
        data = valid;
    }
    // Cross-root references use the complete document's object map.
    selected->sun = second.children()[0];
    selected->fill = first.children()[1];
    first.set_active(true);
    first.get_component<SceneEnvironment>().set_enabled(false);
    restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    equal(accepted, lighting_environment(*restored));
    // This later root decodes after the lights it references.
    auto later = environment(scene, sun, fill);
    later->configure(settings);
    selected.set_enabled(false);
    restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    equal(accepted, lighting_environment(*restored));
    later.object().destroy();
    selected.set_enabled(true);
    first.children()[1].destroy();
    rejects([&] { (void)serialize_scene(scene, {}, codecs); });
    ComponentCodecs conflict;
    conflict.add<SceneEnvironment>(
        "anima.scene-environment.v1", [](const SceneEnvironment &, const ObjectReferences &) { return "{}"; },
        [](GameObject o, std::string_view, const ObjectReferences &) { o.add_component<SceneEnvironment>(); });
    rejects([&] { add_lighting_component_codecs(conflict); });
    rejects([&] { (void)Prefab::capture(sun, conflict); });
}
} // namespace
int main() {
    try {
        transforms_and_configuration();
        selection_and_lifetime();
        boundaries();
        persistence();
        std::cout << "PASS scene directional lighting, environment selection, persistence and lifetime\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
