#include "component_payloads.hpp"
#include "near.hpp"
#include <anima/lighting.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr float tolerance = 2e-5F; // Light directions and radiance, settings and shadow depths.
constexpr auto busy_scene = "Scene drivers require an idle live scene";
constexpr auto no_environment = "Lighting selection requires one active environment";
constexpr auto dead_light = "Selected light must be a live object in the scene selection";
constexpr auto inactive_light = "Selected light must have an active DirectionalLightComponent";
constexpr auto collapsed_axes = "Directional light world axes must be nonzero";
constexpr auto skewed_axes = "Directional light world axes must be orthogonal and right-handed";
constexpr auto invalid_radiance = "Directional light radiance must be finite nonnegative linear RGB";
constexpr auto invalid_exposure = "Invalid environment exposure or fog density";
constexpr auto duplicate_codec = "Duplicate component codec";
constexpr auto outside_graph = "Object reference is stale or outside the captured graph";
constexpr auto needs_number = "Lighting field requires a number";
constexpr auto needs_boolean = "Lighting field requires a boolean";
constexpr auto needs_vector = "Lighting vector requires three numbers";
constexpr auto float_range = "JSON number outside the float range";
constexpr auto integer_resolution = "Shadow resolution requires a positive integer";
constexpr auto resolution_range = "Shadow resolution exceeds its range";
bool near(float a, float b) { return a == Near{b, tolerance}; }
bool near(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }
bool same(const DirectionalShadow &a, const DirectionalShadow &b) {
    return a.enabled == b.enabled && a.resolution == b.resolution && near(a.center, b.center) &&
           near(a.extent, b.extent) && near(a.depth, b.depth) && near(a.constant_bias, b.constant_bias) &&
           near(a.slope_bias, b.slope_bias);
}
bool same(const ShadowCascades &a, const ShadowCascades &b) {
    return a.enabled == b.enabled && a.count == b.count && a.resolution == b.resolution &&
           near(a.distance, b.distance) && near(a.logarithmic_split, b.logarithmic_split) && near(a.blend, b.blend) &&
           near(a.constant_bias, b.constant_bias) && near(a.slope_bias, b.slope_bias);
}
bool same(const Environment &a, const Environment &b) {
    return near(a.sun.direction, b.sun.direction) && near(a.fill.direction, b.fill.direction) &&
           near(a.sun.radiance, b.sun.radiance) && near(a.fill.radiance, b.fill.radiance) &&
           near(a.ambient_sky, b.ambient_sky) && near(a.ambient_ground, b.ambient_ground) &&
           near(a.ambient_specular, b.ambient_specular) && near(a.sky_zenith, b.sky_zenith) &&
           near(a.sky_horizon, b.sky_horizon) && near(a.sky_ground, b.sky_ground) && near(a.fog_color, b.fog_color) &&
           near(a.fog_density, b.fog_density) && near(a.exposure, b.exposure) && a.sky == b.sky &&
           a.tone_mapping == b.tone_mapping && same(a.shadow_cascades, b.shadow_cascades) &&
           same(a.detail_shadow, b.detail_shadow);
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
// @p value with its first @p from replaced by @p to.
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    REQUIRE(position != std::string::npos);
    value.replace(position, from.size(), to);
    return value;
}
// A component payload and the message its decoding throws.
struct Payload {
    std::string state, error;
};
// invalid_component_payloads(valid, field) with their errors, in its order: an empty object, which lacks @p first,
// the codec's first field, then @p field renamed, an unknown field, @p field duplicated, @p field duplicated through
// an escape and nesting past the limit. A payload over the 64 KiB limit follows.
std::vector<Payload> invalid_payloads(const std::string &valid, const std::string &field, const std::string &first) {
    const auto states = invalid_component_payloads(valid, field);
    const std::string errors[]{"Missing JSON field: " + first,   "Missing JSON field: " + field,
                               "Unknown JSON field: unexpected", "Duplicate JSON document field",
                               "Duplicate JSON document field",  "JSON document exceeds nesting limit"};
    REQUIRE(states.size() == std::size(errors));
    std::vector<Payload> result;
    for (std::size_t i = 0; i < states.size(); ++i)
        result.push_back({states[i], errors[i]});
    result.push_back({std::string(64 * 1024 + 1, ' '), "JSON document exceeds byte limit"});
    return result;
}
struct Reentry {
    SceneSet *scenes;
    Scene *scene;
    unsigned *calls;
    void on_update(double) {
        CHECK_THROWS_WITH_AS(lighting_environment(*scene), busy_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(lighting_environment(*scenes), "Scene set is updating", std::logic_error);
        ++*calls;
    }
};
struct Construction {
    Construction(Scene &scene) { CHECK_THROWS_WITH_AS(lighting_environment(scene), busy_scene, std::logic_error); }
};
// Settings with every field changed from its default.
EnvironmentSettings changed_settings() {
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
    settings.shadow_cascades = {true, 3, 150, .6F, .2F, 1024, 1.5F, 2.5F};
    settings.detail_shadow = {true, {4, 5, 6}, 4, 40, 2048, .004F, .005F};
    return settings;
}
// A rig whose environment selects its two child lights, with the lighting codecs registered.
struct Rig {
    ComponentCodecs codecs;
    Scene scene;
    GameObject root = scene.create("rig");
    ComponentRef<SceneEnvironment> selected = root.add_component<SceneEnvironment>();
    GameObject sun = light(scene), fill = light(scene, {.25F, .5F, .75F});
    EnvironmentSettings settings = changed_settings();
    Rig() {
        add_lighting_component_codecs(codecs);
        sun.set_parent(root, ReparentMode::keep_local);
        fill.set_parent(root, ReparentMode::keep_local);
        sun.set_transform({.rotation = {0, 1, 0, 0}, .scale = {3, 4, 5}});
        selected->sun = sun;
        selected->fill = fill;
        selected->configure(settings);
    }
};
} // namespace

TEST_CASE("A light shines along its world -Z axis, whatever its translation and positive scale") {
    Scene scene;
    auto sun = light(scene), fill = light(scene, {});
    (void)environment(scene, sun, fill);
    const auto result = lighting_environment(scene);
    CHECK(near(result.sun.direction, {0, 0, 1}));
    CHECK(near(result.sun.radiance, {1, 2, 3}));
    CHECK(near(result.fill.radiance, {}));
    auto parent = scene.create();
    parent.set_transform({.translation = {8, 5, 3}, .rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    sun.set_parent(parent, ReparentMode::keep_local);
    sun.set_local_position({300, 200, 100});
    CHECK(near(lighting_environment(scene).sun.direction, {0, 0, -1}));
    sun.clear_parent();
    sun.set_transform({.rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    const auto accepted = lighting_environment(scene);
    sun.set_transform({.rotation = {0, 1, 0, 0}});
    CHECK(same(accepted, lighting_environment(scene)));
}

TEST_CASE("Collapsed, mirrored and sheared light axes are rejected") {
    Scene scene;
    auto sun = light(scene);
    (void)environment(scene, sun, light(scene, {}));
    sun.set_transform({.scale = {1, 0, 1}});
    CHECK_THROWS_WITH_AS(lighting_environment(scene), collapsed_axes, std::invalid_argument);
    sun.set_transform({.scale = {1, -1, 1}});
    CHECK_THROWS_WITH_AS(lighting_environment(scene), skewed_axes, std::invalid_argument);
    sun.set_transform({.scale = {1, 1e-5F, 1}}); // Shorter than the 1e-4 minimum axis.
    CHECK_THROWS_WITH_AS(lighting_environment(scene), collapsed_axes, std::invalid_argument);
    auto shear = identity();
    shear[4] = .3F;
    sun.set_world_matrix(shear);
    CHECK_THROWS_WITH_AS(lighting_environment(scene), skewed_axes, std::invalid_argument);
}

TEST_CASE("Invalid radiance is rejected and keeps the previous value") {
    Scene scene;
    auto component = light(scene).get_component<DirectionalLightComponent>();
    for (float invalid : {-1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        for (auto value : {Vec3{invalid, 1, 1}, Vec3{1, invalid, 1}, Vec3{1, 1, invalid}}) {
            INFO("radiance (", value.x, ", ", value.y, ", ", value.z, ")");
            CHECK_THROWS_WITH_AS(component->set_radiance(value), invalid_radiance, std::invalid_argument);
            CHECK(near(component->radiance(), {1, 2, 3}));
        }
}

TEST_CASE("Invalid environment settings are rejected and keep the accepted environment") {
    Scene scene;
    auto sun = light(scene);
    auto selected = environment(scene, sun, light(scene, {}));
    auto settings = selected->settings();
    settings.sky = true;
    settings.tone_mapping = true;
    settings.exposure = 1.5F;
    settings.shadow_cascades.enabled = true;
    settings.detail_shadow.extent = 8;
    selected->configure(settings);
    const auto configured = lighting_environment(scene);
    CHECK(point(detail_shadow_matrix(configured), {0, 0, 0}).z == Near{.5F, tolerance});
    constexpr auto colours = "Environment colours must be finite nonnegative linear RGB";
    constexpr auto cascades = "Invalid shadow cascades", region = "Invalid directional shadow region";
    constexpr auto infinite = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::pair<std::function<void(EnvironmentSettings &)>, const char *>> breaks{
        {[](auto &bad) { bad.exposure = 0; }, invalid_exposure},
        {[](auto &bad) { bad.fog_density = -1; }, invalid_exposure},
        {[](auto &bad) { bad.ambient_specular.x = -1; }, colours},
        {[](auto &bad) { bad.sky_zenith.y = infinite; }, colours},
        {[](auto &bad) { bad.shadow_cascades.count = 0; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.count = 5; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.distance = 0; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.distance = 2e9F; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.distance = nan; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.logarithmic_split = 1.5F; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.logarithmic_split = nan; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.blend = -.5F; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.resolution = 15; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.constant_bias = infinite; }, cascades},
        {[](auto &bad) { bad.shadow_cascades.slope_bias = -1; }, cascades},
        {[](auto &bad) { bad.detail_shadow.extent = 0; }, region},
        {[](auto &bad) { bad.detail_shadow.depth = 0; }, region},
        {[](auto &bad) { bad.detail_shadow.center.z = nan; }, region},
        {[](auto &bad) { bad.detail_shadow.resolution = 0; }, region},
        {[](auto &bad) { bad.detail_shadow.slope_bias = -1; }, region}};
    for (std::size_t index = 0; index < breaks.size(); ++index) {
        CAPTURE(index);
        auto bad = settings;
        breaks[index].first(bad);
        CHECK_THROWS_WITH_AS(selected->configure(bad), breaks[index].second, std::invalid_argument);
        CHECK(same(configured, lighting_environment(scene)));
    }
    settings.detail_shadow.extent = std::numeric_limits<float>::denorm_min();
    selected->configure(settings); // Scalar-valid but cannot form a float projection.
    CHECK_THROWS_WITH_AS(lighting_environment(scene), "Shadow texel size exceeds finite range", std::invalid_argument);
}

TEST_CASE("Lighting resolution requires one active environment with live lights that have active components") {
    SceneSet scenes;
    auto a = scenes.create("a"), b = scenes.create("b");
    auto sun = light(a.get()), fill = light(b.get());
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), no_environment, std::invalid_argument);
    auto selected = environment(b.get(), sun, fill);
    const auto accepted = lighting_environment(scenes);
    scenes.set_active(a);
    CHECK(same(accepted, lighting_environment(scenes)));
    scenes.set_active(b);
    CHECK(same(accepted, lighting_environment(scenes)));
    // The sun is foreign to b on its own.
    CHECK_THROWS_WITH_AS(lighting_environment(b.get()), dead_light, std::invalid_argument);
    auto extra = environment(a.get(), sun, fill);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), "Lighting selection has multiple active environments",
                         std::invalid_argument);
    extra.set_enabled(false);
    CHECK(same(accepted, lighting_environment(scenes)));
    selected.set_enabled(false);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), no_environment, std::invalid_argument);
    selected.set_enabled(true);
    selected.object().set_active(false);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), no_environment, std::invalid_argument);
    selected.object().set_active(true);
    auto group = a->create();
    sun.set_parent(group);
    group.set_active(false);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), inactive_light, std::invalid_argument);
    group.set_active(true);
    sun.get_component<DirectionalLightComponent>().set_enabled(false);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), inactive_light, std::invalid_argument);
    sun.remove_component<DirectionalLightComponent>();
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), inactive_light, std::invalid_argument);
    sun.add_component<DirectionalLightComponent>(Vec3{1, 2, 3});
    CHECK(same(accepted, lighting_environment(scenes)));
    selected->fill = sun; // One authored light may deliberately fill both slots.
    CHECK(same(accepted, lighting_environment(scenes)));
    selected->fill = fill;
    selected->sun = {};
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), dead_light, std::invalid_argument);
    Scene foreign;
    selected->sun = light(foreign);
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), dead_light, std::invalid_argument);
    selected->sun = a->create("not a light");
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), inactive_light, std::invalid_argument);
    selected->sun = sun;
    fill.destroy();
    (void)light(b.get());
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), dead_light, std::invalid_argument);
    selected->fill = sun;
    // Without the lighting codecs nothing repairs the links.
    CHECK(scenes.unload(a).empty());
    a = scenes.create("a");
    (void)light(a.get());
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), dead_light, std::invalid_argument);
    scenes.clear();
    CHECK_FALSE(selected.valid());
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), no_environment, std::invalid_argument);
}

TEST_CASE("An environment in one member follows its lights through a replacement and is cleared by an unload") {
    ComponentCodecs codecs;
    add_lighting_component_codecs(codecs);
    SceneSet scenes;
    auto settings = scenes.create("settings"), lights = scenes.create("lights");
    auto sun = light(lights.get(), {3, 2, 1});
    auto selected = environment(settings.get(), sun, sun);
    const auto accepted = lighting_environment(scenes);
    const auto sun_key = sun.key();
    lights = scenes.replace(lights, serialize_scene(lights.get(), {}, codecs), {}, codecs);
    CHECK_FALSE(sun.valid());
    CHECK(selected->sun.id() == lights->find(sun_key).id());
    CHECK(selected->fill.id() == lights->find(sun_key).id());
    CHECK(same(accepted, lighting_environment(scenes)));
    // A replacement without the light's key fails and keeps both links.
    Scene vacant;
    CHECK_THROWS_WITH_AS((void)scenes.replace(lights, serialize_scene(vacant, {}), {}, codecs),
                         "Replacement lacks a linked object key", std::invalid_argument);
    CHECK(same(accepted, lighting_environment(scenes)));
    // The environment's own member reloads from a set document; its links resolve by address.
    settings = scenes.replace(settings, scenes.serialize({}, codecs), {}, codecs);
    CHECK_FALSE(selected.valid());
    selected = settings->components<SceneEnvironment>().front();
    CHECK(selected->sun.id() == lights->find(sun_key).id());
    CHECK(same(accepted, lighting_environment(scenes)));
    const auto cleared = scenes.unload(lights, codecs);
    REQUIRE(cleared.size() == 2);
    for (const auto &link : cleared) {
        CHECK(link.owner.id() == selected.object().id());
        CHECK(link.component == "anima.scene-environment.v2");
        CHECK(link.target == SceneAddress{"lights", sun_key});
    }
    CHECK(selected->sun.id() == Scene::Id{});
    CHECK(selected->fill.id() == Scene::Id{});
    CHECK_THROWS_WITH_AS(lighting_environment(scenes), dead_light, std::invalid_argument);
}

TEST_CASE("Lighting does not resolve from component hooks or construction") {
    SceneSet scenes;
    auto scene = scenes.create("level");
    auto sun = light(scene.get());
    (void)environment(scene.get(), sun, sun);
    unsigned calls = 0;
    sun.add_component<Reentry>(&scenes, &scene.get(), &calls);
    sun.add_component<Construction>(scene.get());
    scenes.update(0);
    CHECK(calls == 1);
    CHECK_NOTHROW(lighting_environment(scenes));
}

TEST_CASE_FIXTURE(Rig, "Lighting links remap per prefab instance, and settings and activation persist") {
    const auto prefab = Prefab::deserialize(Prefab::capture(root, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    CHECK(first.get_component<SceneEnvironment>()->sun.id() == first.children()[0].id());
    CHECK(first.get_component<SceneEnvironment>()->fill.id() == first.children()[1].id());
    CHECK(second.get_component<SceneEnvironment>()->sun.id() == second.children()[0].id());
    first.set_active(false);
    second.get_component<SceneEnvironment>().set_enabled(false);
    const auto accepted = lighting_environment(scene);
    const auto restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    CHECK(same(accepted, lighting_environment(*restored)));
    CHECK_FALSE(restored->find(first.key()).active_self());
    CHECK_FALSE(restored->find(second.key()).get_component<SceneEnvironment>().enabled());
    CHECK_THROWS_WITH_AS(add_lighting_component_codecs(codecs), duplicate_codec, std::invalid_argument);
    // The failed registration left the codecs as they were.
    CHECK(same(accepted, lighting_environment(*load_scene(serialize_scene(scene, {}, codecs), {}, codecs))));
}

TEST_CASE_FIXTURE(Rig, "Capturing a light outside the prefab is rejected, and a null link stays null") {
    selected->sun = light(scene);
    CHECK_THROWS_WITH_AS(Prefab::capture(root, codecs), outside_graph, std::invalid_argument);
    selected->sun = {};
    const auto unlinked = Prefab::capture(root, codecs).instantiate(scene);
    CHECK_FALSE(unlinked.get_component<SceneEnvironment>()->sun.valid());
}

TEST_CASE_FIXTURE(Rig, "Invalid lighting payloads are rejected without leaking staged objects") {
    const auto accepted = lighting_environment(scene);
    const auto prefab = Prefab::capture(root, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    for (unsigned node : {0U, 1U}) {
        CAPTURE(node);
        auto &data = nodes[node].components[0].state;
        const auto valid = data;
        std::vector<Payload> payloads;
        if (node == 0) {
            payloads = invalid_payloads(valid, "sun", "sun");
            const auto link = "\"sun\":\"" + sun.key().string() + "\"";
            for (const auto &[bad, error] : {std::pair{"0", "Lighting selection requires an object key string"},
                                             std::pair{"\"01\"", "Invalid object key"},
                                             std::pair{"\"99999\"", "Object reference target is missing or expired"}})
                payloads.push_back({replace(valid, link, "\"sun\":" + std::string(bad)), error});
            // Only the detail region has a resolution of 2048.
            for (const auto &[bad, error] :
                 {std::pair{"-1", integer_resolution}, std::pair{"0", resolution_range},
                  std::pair{"1.5", integer_resolution}, std::pair{"1.0", integer_resolution},
                  std::pair{"true", integer_resolution}, std::pair{"4294967296", resolution_range},
                  std::pair{"18446744073709551616", integer_resolution}})
                payloads.push_back(
                    {replace(valid, "\"resolution\":2048", "\"resolution\":" + std::string(bad)), error});
            for (const auto &[bad, error] : {std::pair{"true", needs_number}, std::pair{"\"1\"", needs_number},
                                             std::pair{"-1", invalid_exposure}, std::pair{"1e100", float_range}})
                payloads.push_back({replace(valid, "\"exposure\":1.5", "\"exposure\":" + std::string(bad)), error});
            payloads.push_back({replace(valid, "\"sky\":true", "\"sky\":1"), needs_boolean});
            payloads.push_back({replace(valid, "\"enabled\":true", "\"enabled\":\"true\""), needs_boolean});
            payloads.push_back({replace(valid, "\"center\":[4.0,5.0,6.0]", "\"center\":[4,5]"), needs_vector});
            payloads.push_back({replace(valid, "\"slope_bias\":", "\"slope_bias\":0,\"slope_bias\":"),
                                "Duplicate JSON document field"});
            payloads.push_back(
                {replace(valid, "\"fog_density\":", "\"unexpected\":"), "Missing JSON field: fog_density"});
            // The cascades are the only object with a count, of 3.
            for (const auto &[bad, error] : {std::pair{"0", "Shadow cascade count exceeds its range"},
                                             std::pair{"2.5", "Shadow cascade count requires a positive integer"},
                                             std::pair{"-2", "Shadow cascade count requires a positive integer"},
                                             std::pair{"5", "Invalid shadow cascades"}})
                payloads.push_back({replace(valid, "\"count\":3", "\"count\":" + std::string(bad)), error});
            payloads.push_back({replace(valid, "\"blend\":", "\"unexpected\":"), "Missing JSON field: blend"});
            payloads.push_back(
                {replace(valid, "\"shadow_cascades\":", "\"shadow\":"), "Missing JSON field: shadow_cascades"});
        } else {
            payloads = invalid_payloads(valid, "radiance", "radiance");
            for (const auto &[bad, error] :
                 {std::pair{"null", needs_vector}, std::pair{"1", needs_vector}, std::pair{"[1,2]", needs_vector},
                  std::pair{"[1,2,3,4]", needs_vector}, std::pair{"[-1,2,3]", invalid_radiance},
                  std::pair{"[true,2,3]", needs_number}, std::pair{"[1e100,2,3]", float_range}})
                payloads.push_back({"{\"radiance\":" + std::string(bad) + "}", error});
        }
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            CAPTURE(index);
            data = payloads[index].state;
            const auto size = scene.size();
            CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), payloads[index].error.c_str(),
                                 std::invalid_argument);
            CHECK(scene.size() == size);
            CHECK(same(accepted, lighting_environment(scene)));
        }
        data = valid;
    }
}

TEST_CASE_FIXTURE(Rig, "Links into other roots persist whichever root decodes first, and a stale link fails to save") {
    const auto prefab = Prefab::capture(root, codecs);
    auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    first.get_component<SceneEnvironment>().set_enabled(false);
    second.get_component<SceneEnvironment>().set_enabled(false);
    const auto accepted = lighting_environment(scene);
    // Cross-root references use the complete document's object map.
    selected->sun = second.children()[0];
    selected->fill = first.children()[1];
    auto restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    CHECK(same(accepted, lighting_environment(*restored)));
    // This later root decodes after the lights it references.
    auto later = environment(scene, sun, fill);
    later->configure(settings);
    selected.set_enabled(false);
    restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    CHECK(same(accepted, lighting_environment(*restored)));
    later.object().destroy();
    selected.set_enabled(true);
    first.children()[1].destroy();
    CHECK_THROWS_WITH_AS(serialize_scene(scene, {}, codecs), outside_graph, std::invalid_argument);
}

TEST_CASE("A failed codec registration publishes neither codec") {
    ComponentCodecs conflict;
    conflict.add<SceneEnvironment>(
        "anima.scene-environment.v2", [](const SceneEnvironment &, const ObjectReferences &) { return "{}"; },
        [](GameObject o, std::string_view, const ObjectReferences &) { o.add_component<SceneEnvironment>(); });
    CHECK_THROWS_WITH_AS(add_lighting_component_codecs(conflict), duplicate_codec, std::invalid_argument);
    Scene scene;
    const auto sun = light(scene);
    // The light codec, registered before the conflict, was not published either.
    CHECK_THROWS_WITH_AS(Prefab::capture(sun, conflict), "Component has no persistence codec", std::invalid_argument);
}
