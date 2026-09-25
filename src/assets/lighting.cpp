#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_orientation.hpp"
#include <anima/lighting.hpp>
#include <limits>

namespace anima {
namespace {
constexpr std::size_t maximum_component_bytes = 64 * 1024;
using Json = nlohmann::json;
bool contains(const Scene &scene, GameObject object) { return scene.contains(object.id()); }
bool contains(const SceneSet &scenes, GameObject object) {
    for (auto scene : scenes.scenes())
        if (scene->contains(object.id()))
            return true;
    return false;
}
Vec3 light_direction(GameObject object) {
    return detail::scene_orientation(object.world_matrix(), "Directional light")[2];
}
template <class Scenes> DirectionalLight resolve_light(const Scenes &scenes, GameObject object) {
    if (!object.valid() || !contains(scenes, object))
        throw std::invalid_argument("Selected light must be a live object in the scene selection");
    auto light = object.get_component<DirectionalLightComponent>();
    if (!light || !light.active())
        throw std::invalid_argument("Selected light must have an active DirectionalLightComponent");
    return {light_direction(object), light->radiance()};
}
template <class Scenes> Environment resolve(Scenes &scenes) {
    detail::SceneDriver::check(scenes);
    ComponentRef<SceneEnvironment> selected;
    for (auto environment : scenes.template components<SceneEnvironment>()) {
        if (!environment.active())
            continue;
        if (selected)
            throw std::invalid_argument("Lighting selection has multiple active environments");
        selected = environment;
    }
    if (!selected)
        throw std::invalid_argument("Lighting selection requires one active environment");
    Environment result;
    static_cast<EnvironmentSettings &>(result) = selected->settings();
    result.sun = resolve_light(scenes, selected->sun);
    result.fill = resolve_light(scenes, selected->fill);
    validate_environment(result);
    // The renderer uses both projections even when their regions are disabled.
    (void)directional_shadow_matrix(result);
    (void)directional_shadow_matrix(result, true);
    return result;
}
float number(const Json &j) {
    if (!j.is_number())
        throw std::invalid_argument("Lighting field requires a number");
    const auto value = j.get<float>();
    if (!std::isfinite(value))
        throw std::invalid_argument("Lighting number must be finite");
    return value;
}
bool boolean(const Json &j) {
    if (!j.is_boolean())
        throw std::invalid_argument("Lighting field requires a boolean");
    return j.get<bool>();
}
Json vector_json(Vec3 value) { return Json::array({value.x, value.y, value.z}); }
Vec3 vector(const Json &j) {
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("Lighting vector requires three numbers");
    return {number(j[0]), number(j[1]), number(j[2])};
}
Json shadow_json(const DirectionalShadow &s) {
    return Json{{"enabled", s.enabled},      {"center", vector_json(s.center)}, {"extent", s.extent},
                {"depth", s.depth},          {"resolution", s.resolution},      {"constant_bias", s.constant_bias},
                {"slope_bias", s.slope_bias}};
}
DirectionalShadow shadow(const Json &j) {
    detail::json_fields(j, {"enabled", "center", "extent", "depth", "resolution", "constant_bias", "slope_bias"});
    const auto &size = j.at("resolution");
    if (!size.is_number_integer() || (!size.is_number_unsigned() && size.get<std::int64_t>() <= 0))
        throw std::invalid_argument("Shadow resolution requires a positive integer");
    const auto resolution = size.get<std::uint64_t>();
    if (!resolution || resolution > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Shadow resolution exceeds its range");
    return {boolean(j.at("enabled")),
            vector(j.at("center")),
            number(j.at("extent")),
            number(j.at("depth")),
            static_cast<std::uint32_t>(resolution),
            number(j.at("constant_bias")),
            number(j.at("slope_bias"))};
}
Json settings_json(const EnvironmentSettings &s) {
    return Json{{"ambient_sky", vector_json(s.ambient_sky)},
                {"ambient_ground", vector_json(s.ambient_ground)},
                {"ambient_specular", vector_json(s.ambient_specular)},
                {"sky", s.sky},
                {"sky_zenith", vector_json(s.sky_zenith)},
                {"sky_horizon", vector_json(s.sky_horizon)},
                {"sky_ground", vector_json(s.sky_ground)},
                {"fog_color", vector_json(s.fog_color)},
                {"fog_density", s.fog_density},
                {"exposure", s.exposure},
                {"tone_mapping", s.tone_mapping},
                {"shadow", shadow_json(s.shadow)},
                {"detail_shadow", shadow_json(s.detail_shadow)}};
}
EnvironmentSettings settings(const Json &j) {
    detail::json_fields(j, {"ambient_sky", "ambient_ground", "ambient_specular", "sky", "sky_zenith", "sky_horizon",
                            "sky_ground", "fog_color", "fog_density", "exposure", "tone_mapping", "shadow",
                            "detail_shadow"});
    EnvironmentSettings result;
    result.ambient_sky = vector(j.at("ambient_sky"));
    result.ambient_ground = vector(j.at("ambient_ground"));
    result.ambient_specular = vector(j.at("ambient_specular"));
    result.sky = boolean(j.at("sky"));
    result.sky_zenith = vector(j.at("sky_zenith"));
    result.sky_horizon = vector(j.at("sky_horizon"));
    result.sky_ground = vector(j.at("sky_ground"));
    result.fog_color = vector(j.at("fog_color"));
    result.fog_density = number(j.at("fog_density"));
    result.exposure = number(j.at("exposure"));
    result.tone_mapping = boolean(j.at("tone_mapping"));
    result.shadow = shadow(j.at("shadow"));
    result.detail_shadow = shadow(j.at("detail_shadow"));
    return result;
}
GameObject link(const Json &j, const ObjectReferences &references) {
    if (!j.is_string())
        throw std::invalid_argument("Lighting selection requires an object key string");
    return references.resolve(ObjectKey::parse(j.get<std::string>()));
}
} // namespace
DirectionalLightComponent::DirectionalLightComponent(Vec3 radiance) { set_radiance(radiance); }
void DirectionalLightComponent::set_radiance(Vec3 radiance) {
    if (!std::isfinite(radiance.x) || !std::isfinite(radiance.y) || !std::isfinite(radiance.z) || radiance.x < 0 ||
        radiance.y < 0 || radiance.z < 0)
        throw std::invalid_argument("Directional light radiance must be finite nonnegative linear RGB");
    radiance_ = radiance;
}
SceneEnvironment::SceneEnvironment(EnvironmentSettings settings) { configure(settings); }
void SceneEnvironment::configure(EnvironmentSettings settings) {
    validate_environment_settings(settings);
    settings_ = settings;
}
Environment lighting_environment(Scene &scene) { return resolve(scene); }
Environment lighting_environment(SceneSet &scenes) { return resolve(scenes); }
void add_lighting_component_codecs(ComponentCodecs &codecs) {
    auto pending = codecs;
    pending.add<DirectionalLightComponent>(
        "anima.directional-light.v1",
        [](const DirectionalLightComponent &light, const ObjectReferences &) {
            return Json{{"radiance", vector_json(light.radiance())}}.dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"radiance"});
            object.add_component<DirectionalLightComponent>(vector(j.at("radiance")));
        });
    pending.add<SceneEnvironment>(
        "anima.scene-environment.v1",
        [](const SceneEnvironment &environment, const ObjectReferences &references) {
            return Json{{"sun", references.key(environment.sun).string()},
                        {"fill", references.key(environment.fill).string()},
                        {"settings", settings_json(environment.settings())}}
                .dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &references) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"sun", "fill", "settings"});
            const auto sun = link(j.at("sun"), references), fill = link(j.at("fill"), references);
            auto environment = object.add_component<SceneEnvironment>(settings(j.at("settings")));
            environment->sun = sun;
            environment->fill = fill;
        });
    codecs = std::move(pending);
}
} // namespace anima
