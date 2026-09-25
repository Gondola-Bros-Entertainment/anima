#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "validation.hpp"
#include <anima/physics_scene.hpp>

namespace anima::physics {
namespace {
constexpr float rigid_pose_tolerance = .0001F;

Pose rigid_pose(GameObject object, Motion motion) {
    if (motion == Motion::dynamic && object.parent())
        throw std::invalid_argument("Dynamic rigid bodies must be scene roots");
    const auto m = object.world_matrix();
    for (float value : {m[12], m[13], m[14]})
        if (!std::isfinite(value) || std::abs(value) > detail::maximum_vector_component)
            throw std::invalid_argument("Physics position outside supported range");
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    if (std::abs(dot(x, x) - 1) > rigid_pose_tolerance || std::abs(dot(y, y) - 1) > rigid_pose_tolerance ||
        std::abs(dot(z, z) - 1) > rigid_pose_tolerance || std::abs(dot(x, y)) > rigid_pose_tolerance ||
        std::abs(dot(x, z)) > rigid_pose_tolerance || std::abs(dot(y, z)) > rigid_pose_tolerance ||
        dot(cross(x, y), z) < 1 - rigid_pose_tolerance || m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
        throw std::invalid_argument("Physics transforms require unit scale and no shear/reflection");
    // Matrix-to-quaternion using the largest diagonal component for stable 180-degree rotations.
    Quat q;
    const float trace = m[0] + m[5] + m[10];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s, s / 4};
    } else {
        const unsigned i = m[0] > m[5] ? (m[0] > m[10] ? 0 : 2) : (m[5] > m[10] ? 1 : 2);
        const unsigned j = (i + 1) % 3, k = (i + 2) % 3;
        const float s = std::sqrt(1 + m[i * 4 + i] - m[j * 4 + j] - m[k * 4 + k]) * 2;
        q[i] = s / 4;
        q[j] = (m[j * 4 + i] + m[i * 4 + j]) / s;
        q[k] = (m[k * 4 + i] + m[i * 4 + k]) / s;
        q[3] = (m[j * 4 + k] - m[k * 4 + j]) / s;
    }
    return {{m[12], m[13], m[14]}, unit_quaternion(q)};
}
using Json = nlohmann::json;
Json vec(Vec3 v) { return Json::array({v.x, v.y, v.z}); }
Vec3 vec(const Json &j) {
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("Physics vector requires three numbers");
    Vec3 v;
    float *values[] = {&v.x, &v.y, &v.z};
    for (unsigned i = 0; i < 3; ++i) {
        if (!j[i].is_number())
            throw std::invalid_argument("Physics vector must be numeric");
        *values[i] = j[i].get<float>();
    }
    return v;
}
int integer(const Json &j, const char *key, unsigned maximum) {
    if (!j.at(key).is_number_integer() || j.at(key).get<std::int64_t>() < 0 || j.at(key).get<std::uint64_t>() > maximum)
        throw std::invalid_argument("Invalid rigid body enum/layer");
    return j.at(key).get<int>();
}
float number(const Json &j, const char *key) {
    if (!j.at(key).is_number())
        throw std::invalid_argument("Invalid physics scalar");
    return j.at(key).get<float>();
}
Json collider_data(const Collider &c) {
    Json vertices = Json::array();
    for (auto v : c.vertices)
        vertices.push_back(vec(v));
    return {{"shape", static_cast<int>(c.shape)},
            {"extent", vec(c.half_extent)},
            {"radius", c.radius},
            {"half_height", c.half_height},
            {"vertices", vertices}};
}
Collider collider(const Json &j, bool child) {
    Collider c;
    c.shape = static_cast<Shape>(integer(j, "shape", static_cast<unsigned>(Shape::compound)));
    if (child && (c.shape == Shape::mesh || c.shape == Shape::compound))
        throw std::invalid_argument("Compound child must be a primitive or hull");
    c.half_extent = vec(j.at("extent"));
    c.radius = number(j, "radius");
    c.half_height = number(j, "half_height");
    const auto &vertices = j.at("vertices");
    const auto maximum = c.shape == Shape::mesh ? detail::maximum_mesh_vertices : detail::maximum_hull_points;
    if (!vertices.is_array() || vertices.size() > maximum)
        throw std::invalid_argument("Invalid collider vertex array");
    for (const auto &v : vertices)
        c.vertices.push_back(vec(v));
    return c;
}
} // namespace
RigidBody::RigidBody(GameObject object, World &world, BodySettings settings) : settings_(std::move(settings)) {
    settings_.pose = rigid_pose(object, settings_.motion);
    body_ = world.create(settings_);
}
RigidBody::~RigidBody() { body_.remove(); }
namespace {
template <class Scenes> void step_scenes(Scenes &scenes, World &world, double seconds) {
    if (!std::isfinite(seconds) || seconds < detail::minimum_step_seconds || seconds > detail::maximum_step_seconds)
        throw std::invalid_argument("Invalid physics fixed step");
    anima::detail::SceneDriver::check(scenes);
    auto components = scenes.template components<RigidBody>();
    // Validate the full snapshot before changing simulation state.
    std::vector<Pose> poses;
    for (const auto &component : components) {
        if (!world.owns(component->body()))
            throw std::invalid_argument("Rigid body belongs to another or expired world");
        poses.push_back(rigid_pose(component.object(), component->settings().motion));
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        auto &component = components[i];
        auto body = component->body();
        body.set_enabled(component.active());
        if (!component.active())
            continue;
        const auto motion = component->settings().motion;
        if (motion == Motion::stationary)
            body.teleport(poses[i]);
        else if (motion == Motion::kinematic)
            body.move_kinematic(poses[i], seconds);
    }
    world.step(seconds);
    for (auto &component : components) {
        if (component.active() && component->settings().motion == Motion::dynamic) {
            const auto pose = component->body().pose();
            component.object().set_transform({pose.position, pose.rotation, {1, 1, 1}});
        }
    }
}
} // namespace
void step(Scene &scene, World &world, double seconds) { step_scenes(scene, world, seconds); }
void step(SceneSet &scenes, World &world, double seconds) { step_scenes(scenes, world, seconds); }
void add_component_codec(ComponentCodecs &codecs, World &world) {
    const std::weak_ptr<int> lifetime = world.lifetime_;
    codecs.add<RigidBody>(
        "anima.rigid-body.v2",
        [](const RigidBody &component, const ObjectReferences &) {
            const auto &s = component.settings();
            const auto &c = s.collider;
            Json children = Json::array();
            for (const auto &part : c.children) {
                auto child = collider_data(part.collider);
                child["position"] = vec(part.pose.position);
                child["rotation"] = part.pose.rotation;
                children.push_back(std::move(child));
            }
            auto data = collider_data(c);
            data.update(Json{{"indices", c.indices},
                             {"children", children},
                             {"center_of_mass", s.center_of_mass ? vec(*s.center_of_mass) : Json(nullptr)},
                             {"motion", static_cast<int>(s.motion)},
                             {"velocity", vec(component.body().velocity())},
                             {"angular_velocity", vec(component.body().angular_velocity())},
                             {"mass", s.mass},
                             {"friction", s.friction},
                             {"restitution", s.restitution},
                             {"layer", s.layer},
                             {"sensor", s.sensor},
                             {"continuous", s.continuous}});
            return data.dump();
        },
        [&world, lifetime](GameObject object, std::string_view data, const ObjectReferences &) {
            if (lifetime.expired())
                throw std::out_of_range("Rigid body codec world expired");
            const auto j = anima::detail::parse_json(data, detail::maximum_component_bytes);
            anima::detail::json_fields(j, {"shape", "extent", "radius", "half_height", "vertices", "indices", "motion",
                                           "velocity", "angular_velocity", "mass", "friction", "restitution", "layer",
                                           "sensor", "continuous", "children", "center_of_mass"});
            BodySettings s;
            s.collider = collider(j, false);
            auto &c = s.collider;
            s.motion = static_cast<Motion>(integer(j, "motion", static_cast<unsigned>(Motion::dynamic)));
            s.layer = static_cast<std::uint8_t>(integer(j, "layer", detail::collision_layer_count - 1));
            s.velocity = vec(j.at("velocity"));
            s.angular_velocity = vec(j.at("angular_velocity"));
            s.mass = number(j, "mass");
            s.friction = number(j, "friction");
            s.restitution = number(j, "restitution");
            if (!j.at("center_of_mass").is_null())
                s.center_of_mass = vec(j.at("center_of_mass"));
            if (!j.at("sensor").is_boolean() || !j.at("continuous").is_boolean() || !j.at("indices").is_array() ||
                j.at("indices").size() > detail::maximum_mesh_indices || !j.at("children").is_array() ||
                j.at("children").size() > detail::maximum_compound_children)
                throw std::invalid_argument("Invalid rigid body arrays/flags");
            s.sensor = j.at("sensor").get<bool>();
            s.continuous = j.at("continuous").get<bool>();
            for (const auto &index : j.at("indices")) {
                if (!index.is_number_integer() || index.get<std::int64_t>() < 0 ||
                    index.get<std::uint64_t>() > UINT32_MAX)
                    throw std::invalid_argument("Invalid collider index");
                c.indices.push_back(index.get<std::uint32_t>());
            }
            for (const auto &child : j.at("children")) {
                anima::detail::json_fields(
                    child, {"position", "rotation", "shape", "extent", "radius", "half_height", "vertices"});
                ColliderChild part;
                part.pose.position = vec(child.at("position"));
                const auto &rotation = child.at("rotation");
                if (!rotation.is_array() || rotation.size() != 4)
                    throw std::invalid_argument("Compound rotation requires four numbers");
                for (unsigned i = 0; i < 4; ++i) {
                    if (!rotation[i].is_number())
                        throw std::invalid_argument("Compound rotation must be numeric");
                    part.pose.rotation[i] = rotation[i].get<float>();
                }
                part.collider = collider(child, true);
                c.children.push_back(std::move(part));
            }
            object.add_component<RigidBody>(world, std::move(s));
        });
}
} // namespace anima::physics
