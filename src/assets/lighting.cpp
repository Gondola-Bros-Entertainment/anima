#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_orientation.hpp"
#include <anima/lighting.hpp>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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
    return {light_direction(object), light->irradiance()};
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
    // The renderer uses the detail region's projection even while the region is disabled.
    (void)detail_shadow_matrix(result);
    return result;
}
float number(const Json &j) {
    if (!j.is_number())
        throw std::invalid_argument("Lighting field requires a number");
    const auto value = detail::json_float(j);
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
// A positive integer within the 32-bit range, named @p name in the messages that reject it.
std::uint32_t positive_integer(const Json &j, const std::string &name) {
    if (!j.is_number_integer() || (!j.is_number_unsigned() && j.get<std::int64_t>() <= 0))
        throw std::invalid_argument(name + " requires a positive integer");
    const auto value = j.get<std::uint64_t>();
    if (!value || value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(name + " exceeds its range");
    return static_cast<std::uint32_t>(value);
}
Json bias_json(const ShadowBias &b) { return Json{{"constant", b.constant}, {"slope", b.slope}}; }
ShadowBias bias(const Json &j) {
    detail::json_fields(j, {"constant", "slope"});
    return {number(j.at("constant")), number(j.at("slope"))};
}
Json cascades_json(const ShadowCascades &c) {
    return Json{{"enabled", c.enabled},     {"count", c.count},
                {"distance", c.distance},   {"logarithmic_split", c.logarithmic_split},
                {"blend", c.blend},         {"resolution", c.resolution},
                {"bias", bias_json(c.bias)}};
}
ShadowCascades cascades(const Json &j) {
    detail::json_fields(j, {"enabled", "count", "distance", "logarithmic_split", "blend", "resolution", "bias"});
    return {boolean(j.at("enabled")), positive_integer(j.at("count"), "Shadow cascade count"),
            number(j.at("distance")), number(j.at("logarithmic_split")),
            number(j.at("blend")),    positive_integer(j.at("resolution"), "Shadow resolution"),
            bias(j.at("bias"))};
}
Json shadow_json(const DirectionalShadow &s) {
    return Json{{"enabled", s.enabled}, {"center", vector_json(s.center)}, {"extent", s.extent},
                {"depth", s.depth},     {"resolution", s.resolution},      {"bias", bias_json(s.bias)}};
}
DirectionalShadow shadow(const Json &j) {
    detail::json_fields(j, {"enabled", "center", "extent", "depth", "resolution", "bias"});
    return {boolean(j.at("enabled")),
            vector(j.at("center")),
            number(j.at("extent")),
            number(j.at("depth")),
            positive_integer(j.at("resolution"), "Shadow resolution"),
            bias(j.at("bias"))};
}
Json fog_json(const HeightFog &f) {
    return Json{{"color", vector_json(f.color)},
                {"density", f.density},
                {"height", f.height},
                {"falloff", f.falloff},
                {"sun_scattering", vector_json(f.sun_scattering)},
                {"sun_anisotropy", f.sun_anisotropy},
                {"sky_distance", f.sky_distance}};
}
HeightFog fog(const Json &j) {
    detail::json_fields(j,
                        {"color", "density", "height", "falloff", "sun_scattering", "sun_anisotropy", "sky_distance"});
    return {vector(j.at("color")),       number(j.at("density")),        number(j.at("height")),
            number(j.at("falloff")),     vector(j.at("sun_scattering")), number(j.at("sun_anisotropy")),
            number(j.at("sky_distance"))};
}
// Each ToneMapping enumerator with the name that the payload stores it under.
constexpr std::array<std::pair<ToneMapping, std::string_view>, 2> tone_mappings{
    {{ToneMapping::none, "none"}, {ToneMapping::reinhard, "reinhard"}}};
Json tone_mapping_json(ToneMapping mapping) {
    for (const auto &[value, name] : tone_mappings)
        if (value == mapping)
            return std::string(name);
    throw std::invalid_argument("Unknown tone mapping");
}
ToneMapping tone_mapping(const Json &j) {
    if (!j.is_string())
        throw std::invalid_argument("Lighting field requires a string");
    const auto &text = j.get_ref<const std::string &>();
    for (const auto &[value, name] : tone_mappings)
        if (text == name)
            return value;
    throw std::invalid_argument("Unknown tone mapping");
}
Json atmosphere_json(const Atmosphere &a) {
    return Json{{"enabled", a.enabled},
                {"ground_height", a.ground_height},
                {"planet_radius", a.planet_radius},
                {"thickness", a.thickness},
                {"rayleigh_scattering", vector_json(a.rayleigh_scattering)},
                {"rayleigh_scale_height", a.rayleigh_scale_height},
                {"mie_scattering", vector_json(a.mie_scattering)},
                {"mie_absorption", vector_json(a.mie_absorption)},
                {"mie_scale_height", a.mie_scale_height},
                {"ozone_absorption", vector_json(a.ozone_absorption)},
                {"ozone_altitude", a.ozone_altitude},
                {"ozone_width", a.ozone_width},
                {"mie_anisotropy", a.mie_anisotropy},
                {"ground_albedo", vector_json(a.ground_albedo)},
                {"sun_angular_radius", a.sun_angular_radius}};
}
Atmosphere atmosphere(const Json &j) {
    detail::json_fields(j, {"enabled", "ground_height", "planet_radius", "thickness", "rayleigh_scattering",
                            "rayleigh_scale_height", "mie_scattering", "mie_absorption", "mie_scale_height",
                            "ozone_absorption", "ozone_altitude", "ozone_width", "mie_anisotropy", "ground_albedo",
                            "sun_angular_radius"});
    Atmosphere a;
    a.enabled = boolean(j.at("enabled"));
    a.ground_height = number(j.at("ground_height"));
    a.planet_radius = number(j.at("planet_radius"));
    a.thickness = number(j.at("thickness"));
    a.rayleigh_scattering = vector(j.at("rayleigh_scattering"));
    a.rayleigh_scale_height = number(j.at("rayleigh_scale_height"));
    a.mie_scattering = vector(j.at("mie_scattering"));
    a.mie_absorption = vector(j.at("mie_absorption"));
    a.mie_scale_height = number(j.at("mie_scale_height"));
    a.ozone_absorption = vector(j.at("ozone_absorption"));
    a.ozone_altitude = number(j.at("ozone_altitude"));
    a.ozone_width = number(j.at("ozone_width"));
    a.mie_anisotropy = number(j.at("mie_anisotropy"));
    a.ground_albedo = vector(j.at("ground_albedo"));
    a.sun_angular_radius = number(j.at("sun_angular_radius"));
    return a;
}
Json settings_json(const EnvironmentSettings &s) {
    return Json{{"ambient_sky", vector_json(s.ambient_sky)},
                {"ambient_ground", vector_json(s.ambient_ground)},
                {"ambient_specular", vector_json(s.ambient_specular)},
                {"atmosphere", atmosphere_json(s.atmosphere)},
                {"background", vector_json(s.background)},
                {"fog", fog_json(s.fog)},
                {"exposure", s.exposure},
                {"tone_mapping", tone_mapping_json(s.tone_mapping)},
                {"shadow_cascades", cascades_json(s.shadow_cascades)},
                {"detail_shadow", shadow_json(s.detail_shadow)}};
}
EnvironmentSettings settings(const Json &j) {
    detail::json_fields(j, {"ambient_sky", "ambient_ground", "ambient_specular", "atmosphere", "background", "fog",
                            "exposure", "tone_mapping", "shadow_cascades", "detail_shadow"});
    EnvironmentSettings result;
    result.ambient_sky = vector(j.at("ambient_sky"));
    result.ambient_ground = vector(j.at("ambient_ground"));
    result.ambient_specular = vector(j.at("ambient_specular"));
    result.atmosphere = atmosphere(j.at("atmosphere"));
    result.background = vector(j.at("background"));
    result.fog = fog(j.at("fog"));
    result.exposure = number(j.at("exposure"));
    result.tone_mapping = tone_mapping(j.at("tone_mapping"));
    result.shadow_cascades = cascades(j.at("shadow_cascades"));
    result.detail_shadow = shadow(j.at("detail_shadow"));
    return result;
}
GameObject link(const Json &j, const ObjectReferences &references) {
    if (!j.is_string())
        throw std::invalid_argument("Lighting selection requires an object key string");
    return references.resolve(ObjectKey::parse(j.get<std::string>()));
}
} // namespace
DirectionalLightComponent::DirectionalLightComponent(Vec3 irradiance) { set_irradiance(irradiance); }
void DirectionalLightComponent::set_irradiance(Vec3 irradiance) {
    if (!std::isfinite(irradiance.x) || !std::isfinite(irradiance.y) || !std::isfinite(irradiance.z) ||
        irradiance.x < 0 || irradiance.y < 0 || irradiance.z < 0)
        throw std::invalid_argument("Directional light irradiance must be finite nonnegative linear RGB");
    irradiance_ = irradiance;
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
        "anima.directional-light.v2",
        [](const DirectionalLightComponent &light, const ObjectReferences &) {
            return Json{{"irradiance", vector_json(light.irradiance())}}.dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"irradiance"});
            object.add_component<DirectionalLightComponent>(vector(j.at("irradiance")));
        });
    pending.add<SceneEnvironment>(
        "anima.scene-environment.v4",
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
        },
        [](SceneEnvironment &environment, ObjectLinks &links) {
            links.add(environment.sun);
            links.add(environment.fill);
        });
    codecs = std::move(pending);
}
} // namespace anima
