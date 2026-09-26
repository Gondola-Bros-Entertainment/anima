#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "limits.hpp"
#include <anima/physics2d_scene.hpp>

#include <numbers>

namespace anima::physics2d {
namespace {
Pose planar_pose(GameObject object, Motion motion) {
    if (motion == Motion::dynamic && object.parent())
        throw std::invalid_argument("Dynamic 2D bodies must be scene roots");
    const auto m = object.world_matrix();
    const auto position = translation_of(m);
    for (float value : {position.x, position.y})
        if (!std::isfinite(value) || std::abs(value) > detail::maximum_position)
            throw std::invalid_argument("2D physics position outside supported range");
    const auto near = [](float a, float b) { return std::isfinite(a) && std::abs(a - b) < detail::planar_tolerance; };
    const auto x = axis_x(m), y = axis_y(m), z = axis_z(m);
    if (!near(x.x * x.x + x.y * x.y, 1) || !near(y.x * y.x + y.y * y.y, 1) || !near(x.x * y.x + x.y * y.y, 0) ||
        !near(x.x * y.y - x.y * y.x, 1) || !near(x.z, 0) || !near(y.z, 0) || !near(z.x, 0) || !near(z.y, 0) ||
        !near(z.z, 1))
        throw std::invalid_argument("2D physics requires unit scale, XY translation and Z rotation without tilt/shear");
    return {{position.x, position.y}, std::atan2(x.y, x.x)};
}
using Json = nlohmann::json;
Json vec(Vec2 v) { return Json::array({v.x, v.y}); }
Vec2 vec(const Json &j) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
        throw std::invalid_argument("2D physics vector requires two numbers");
    return {anima::detail::json_float(j[0]), anima::detail::json_float(j[1])};
}
} // namespace
RigidBody::RigidBody(GameObject object, World &world, BodySettings settings) : settings_(std::move(settings)) {
    settings_.pose = planar_pose(object, settings_.motion);
    body_ = world.create(settings_);
}
RigidBody::~RigidBody() { body_.remove(); }
namespace {
template <class Scenes> void step_scenes(Scenes &scenes, World &world, double seconds) {
    if (!std::isfinite(seconds) || seconds < detail::minimum_step_seconds || seconds > detail::maximum_step_seconds)
        throw std::invalid_argument("Invalid 2D physics fixed step");
    anima::detail::SceneDriver::check(scenes);
    auto components = scenes.template components<RigidBody>();
    std::vector<Pose> poses;
    for (const auto &component : components) {
        if (!world.owns(component->body()))
            throw std::invalid_argument("2D body belongs to another or expired world");
        poses.push_back(planar_pose(component.object(), component->settings().motion));
        if (component->settings().motion == Motion::kinematic && component->settings().fixed_rotation &&
            std::abs(std::remainder(poses.back().angle - component->body().pose().angle,
                                    2 * std::numbers::pi_v<float>)) >= detail::fixed_rotation_tolerance)
            throw std::invalid_argument("Fixed-rotation kinematic object changed angle");
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        auto &component = components[i];
        auto body = component->body();
        body.set_enabled(component.active());
        if (!component.active())
            continue;
        const auto motion = component->settings().motion;
        if (motion == Motion::stationary) {
            const auto old = body.pose();
            if (old.position.x != poses[i].position.x || old.position.y != poses[i].position.y ||
                old.angle != poses[i].angle)
                body.teleport(poses[i]);
        } else if (motion == Motion::kinematic)
            body.move_kinematic(poses[i], seconds);
    }
    world.step(seconds);
    for (auto &component : components) {
        if (component.active() && component->settings().motion == Motion::dynamic) {
            const auto pose = component->body().pose();
            component.object().set_transform({{pose.position.x, pose.position.y, component.object().position().z},
                                              {0, 0, std::sin(pose.angle / 2), std::cos(pose.angle / 2)},
                                              {1, 1, 1}});
        }
    }
}
} // namespace
void step(Scene &scene, World &world, double seconds) { step_scenes(scene, world, seconds); }
void step(SceneSet &scenes, World &world, double seconds) { step_scenes(scenes, world, seconds); }
void add_component_codec(ComponentCodecs &codecs, World &world) {
    const std::weak_ptr<int> lifetime = world.lifetime_;
    codecs.add<RigidBody>(
        "anima.rigid-body-2d.v1",
        [](const RigidBody &component, const ObjectReferences &) {
            const auto &s = component.settings();
            return Json{{"shape", static_cast<int>(s.collider.shape)},
                        {"extent", vec(s.collider.half_extent)},
                        {"radius", s.collider.radius},
                        {"half_height", s.collider.half_height},
                        {"motion", static_cast<int>(s.motion)},
                        {"velocity", vec(component.body().velocity())},
                        {"angular_velocity", component.body().angular_velocity()},
                        {"density", s.density},
                        {"friction", s.friction},
                        {"restitution", s.restitution},
                        {"layer", s.layer},
                        {"sensor", s.sensor},
                        {"continuous", s.continuous},
                        {"fixed_rotation", s.fixed_rotation}}
                .dump();
        },
        [&world, lifetime](GameObject object, std::string_view data, const ObjectReferences &) {
            if (lifetime.expired())
                throw std::out_of_range("2D rigid body codec world expired");
            const auto j = anima::detail::parse_json(data, 8192);
            anima::detail::json_fields(j, {"shape", "extent", "radius", "half_height", "motion", "velocity",
                                           "angular_velocity", "density", "friction", "restitution", "layer", "sensor",
                                           "continuous", "fixed_rotation"});
            const auto integer = [&](const char *key, unsigned maximum) {
                const auto &v = j.at(key);
                if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::uint64_t>() > maximum)
                    throw std::invalid_argument("Invalid 2D rigid body enum/layer");
                return v.get<unsigned>();
            };
            const auto number = [&](const char *key) {
                if (!j.at(key).is_number())
                    throw std::invalid_argument("Invalid 2D physics scalar");
                return anima::detail::json_float(j.at(key));
            };
            const auto boolean = [&](const char *key) {
                if (!j.at(key).is_boolean())
                    throw std::invalid_argument("Invalid 2D physics flag");
                return j.at(key).get<bool>();
            };
            BodySettings s;
            s.collider.shape = static_cast<Shape>(integer("shape", 2));
            s.collider.half_extent = vec(j.at("extent"));
            s.collider.radius = number("radius");
            s.collider.half_height = number("half_height");
            s.motion = static_cast<Motion>(integer("motion", 2));
            s.velocity = vec(j.at("velocity"));
            s.angular_velocity = number("angular_velocity");
            s.density = number("density");
            s.friction = number("friction");
            s.restitution = number("restitution");
            s.layer = static_cast<std::uint8_t>(integer("layer", detail::collision_layer_count - 1));
            s.sensor = boolean("sensor");
            s.continuous = boolean("continuous");
            s.fixed_rotation = boolean("fixed_rotation");
            object.add_component<RigidBody>(world, s);
        });
}
} // namespace anima::physics2d
