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
    const auto near = [](float a, float b) {
        return std::isfinite(a) && std::abs(a - b) < detail::rigid_transform_tolerance;
    };
    const auto x = axis_x(m), y = axis_y(m), z = axis_z(m);
    if (!near(x.x * x.x + x.y * x.y, 1) || !near(y.x * y.x + y.y * y.y, 1) || !near(x.x * y.x + x.y * y.y, 0) ||
        !near(x.x * y.y - x.y * y.x, 1) || !near(x.z, 0) || !near(y.z, 0) || !near(z.x, 0) || !near(z.y, 0) ||
        !near(z.z, 1))
        throw std::invalid_argument("2D physics requires unit scale, XY translation and Z rotation without tilt/shear");
    return {{position.x, position.y}, std::atan2(x.y, x.x)};
}
// Whether two world matrices differ in anything but their Z translation, which is presentation depth.
bool planar_change(const Mat4 &a, const Mat4 &b) {
    constexpr std::size_t depth = 14;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (i != depth && a[i] != b[i])
            return true;
    return false;
}
using Json = nlohmann::json;
constexpr auto unknown_shape = "Unknown 2D rigid body shape";
constexpr auto unknown_motion = "Unknown 2D rigid body motion";
constexpr anima::detail::JsonNames<Shape, 3> shape_names{
    {{Shape::box, "box"}, {Shape::circle, "circle"}, {Shape::capsule, "capsule"}}};
static_assert(shape_names.size() == static_cast<std::size_t>(Shape::capsule) + 1, "Name every shape");
constexpr anima::detail::JsonNames<Motion, 3> motion_names{
    {{Motion::stationary, "stationary"}, {Motion::kinematic, "kinematic"}, {Motion::dynamic, "dynamic"}}};
static_assert(motion_names.size() == static_cast<std::size_t>(Motion::dynamic) + 1, "Name every motion");
Json vec(Vec2 v) { return Json::array({v.x, v.y}); }
Vec2 vec(const Json &j) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
        throw std::invalid_argument("2D physics vector requires two numbers");
    return {anima::detail::json_float(j[0]), anima::detail::json_float(j[1])};
}
} // namespace
RigidBody::RigidBody(GameObject object, World &world, BodySettings settings)
    : settings_(std::move(settings)), published_(object.world_matrix()) {
    settings_.pose = planar_pose(object, settings_.motion);
    body_ = world.create(settings_);
}
RigidBody::~RigidBody() { body_.remove(); }
namespace detail {
struct RigidBodyAccess {
    static Mat4 &published(RigidBody &component) { return component.published_; }
};
} // namespace detail
namespace {
template <class Scenes> void step_scenes(Scenes &scenes, World &world, double seconds) {
    if (!std::isfinite(seconds) || seconds < detail::minimum_step_seconds || seconds > detail::maximum_step_seconds)
        throw std::invalid_argument("Invalid 2D physics fixed step");
    anima::detail::SceneDriver::check(scenes);
    auto components = scenes.template components<RigidBody>();
    // Validate the full snapshot before changing simulation state. The driver never uses an inactive component's
    // pose, so its object meets the transform rules only once it is active again.
    std::vector<Pose> poses(components.size());
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto &component = components[i];
        if (!world.owns(component->body()))
            throw std::invalid_argument("2D body belongs to another or expired world");
        if (!component.active())
            continue;
        poses[i] = planar_pose(component.object(), component->settings().motion);
        if (component->settings().motion == Motion::kinematic && component->settings().fixed_rotation &&
            std::abs(std::remainder(poses[i].angle - component->body().pose().angle, 2 * std::numbers::pi_v<float>)) >=
                detail::fixed_rotation_tolerance)
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
        } else if (motion == Motion::kinematic) {
            body.move_kinematic(poses[i], seconds);
        } else if (planar_change(component.object().world_matrix(), detail::RigidBodyAccess::published(*component))) {
            // The object was edited since construction or the last write-back. Comparing with Body::pose() instead
            // would undo a teleport through body(), and the written angle need not survive the round trip through
            // the object's matrix bit for bit.
            body.teleport(poses[i]);
        }
    }
    world.step(seconds);
    for (auto &component : components) {
        if (component.active() && component->settings().motion == Motion::dynamic) {
            const auto pose = component->body().pose();
            auto object = component.object();
            object.set_transform({{pose.position.x, pose.position.y, object.position().z},
                                  {0, 0, std::sin(pose.angle / 2), std::cos(pose.angle / 2)},
                                  {1, 1, 1}});
            detail::RigidBodyAccess::published(*component) = object.world_matrix();
        }
    }
}
template <class Scenes> ComponentRef<RigidBody> find_owner(Scenes &scenes, const Body &body) {
    for (auto &component : scenes.template components<RigidBody>())
        if (component->body() == body)
            return component;
    return {};
}
} // namespace
void step(Scene &scene, World &world, double seconds) { step_scenes(scene, world, seconds); }
void step(SceneSet &scenes, World &world, double seconds) { step_scenes(scenes, world, seconds); }
ComponentRef<RigidBody> find_rigid_body(Scene &scene, const Body &body) { return find_owner(scene, body); }
ComponentRef<RigidBody> find_rigid_body(SceneSet &scenes, const Body &body) { return find_owner(scenes, body); }
void add_component_codec(ComponentCodecs &codecs, World &world) {
    const std::weak_ptr<int> lifetime = world.lifetime_;
    codecs.add<RigidBody>(
        "anima.rigid-body-2d.v2",
        [](const RigidBody &component, const ObjectReferences &) {
            const auto &s = component.settings();
            return Json{{"shape", anima::detail::json_name(shape_names, s.collider.shape, unknown_shape)},
                        {"half_extent", vec(s.collider.half_extent)},
                        {"radius", s.collider.radius},
                        {"half_height", s.collider.half_height},
                        {"motion", anima::detail::json_name(motion_names, s.motion, unknown_motion)},
                        {"velocity", vec(component.body().velocity())},
                        {"angular_velocity", component.body().angular_velocity()},
                        {"mass", s.mass},
                        {"linear_damping", s.linear_damping},
                        {"angular_damping", s.angular_damping},
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
            const auto j = anima::detail::parse_json(data, detail::maximum_component_bytes);
            anima::detail::json_fields(j, {"shape", "half_extent", "radius", "half_height", "motion", "velocity",
                                           "angular_velocity", "mass", "linear_damping", "angular_damping", "friction",
                                           "restitution", "layer", "sensor", "continuous", "fixed_rotation"});
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
            s.collider.shape = anima::detail::json_enumerator(shape_names, j.at("shape"), unknown_shape);
            s.collider.half_extent = vec(j.at("half_extent"));
            s.collider.radius = number("radius");
            s.collider.half_height = number("half_height");
            s.motion = anima::detail::json_enumerator(motion_names, j.at("motion"), unknown_motion);
            s.velocity = vec(j.at("velocity"));
            s.angular_velocity = number("angular_velocity");
            s.mass = number("mass");
            s.linear_damping = number("linear_damping");
            s.angular_damping = number("angular_damping");
            s.friction = number("friction");
            s.restitution = number("restitution");
            const auto &layer = j.at("layer");
            if (!layer.is_number_integer() || layer.get<std::int64_t>() < 0 ||
                layer.get<std::uint64_t>() >= detail::collision_layer_count)
                throw std::invalid_argument("Invalid 2D rigid body layer");
            s.layer = layer.get<std::uint8_t>();
            s.sensor = boolean("sensor");
            s.continuous = boolean("continuous");
            s.fixed_rotation = boolean("fixed_rotation");
            object.add_component<RigidBody>(world, s);
        });
}
} // namespace anima::physics2d
