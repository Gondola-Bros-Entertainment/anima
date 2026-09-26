#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_orientation.hpp"
#include <anima/camera.hpp>
#include <numbers>

namespace anima {
namespace {
constexpr std::size_t maximum_component_bytes = 64 * 1024;
constexpr float minimum_extent = 1e-4F, maximum_extent = 1e9F;
void validate(const CameraSettings &s) {
    if (s.projection != CameraProjection::perspective && s.projection != CameraProjection::orthographic)
        throw std::invalid_argument("Unknown camera projection");
    if (!std::isfinite(s.vertical_fov_degrees) || s.vertical_fov_degrees < 1 || s.vertical_fov_degrees > 179 ||
        !std::isfinite(s.orthographic_height) || s.orthographic_height < minimum_extent ||
        s.orthographic_height > maximum_extent || !std::isfinite(s.near_plane) || s.near_plane < minimum_extent ||
        !std::isfinite(s.far_plane) || s.far_plane <= s.near_plane || s.far_plane > maximum_extent)
        throw std::invalid_argument("Invalid camera lens settings");
}
bool contains(const Scene &scene, GameObject object) { return scene.contains(object.id()); }
bool contains(const SceneSet &scenes, GameObject object) {
    for (auto scene : scenes.scenes())
        if (scene->contains(object.id()))
            return true;
    return false;
}
Mat4 camera_matrix(GameObject object, const CameraSettings &s, float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0)
        throw std::invalid_argument("Camera aspect must be finite and positive");
    const auto world = object.world_matrix();
    const auto axes = detail::scene_orientation(world, "Camera");
    const Vec3 eye = translation_of(world);
    const auto &x = axes[0], &y = axes[1], &z = axes[2];
    const Mat4 view{x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1};
    const double near = s.near_plane, far = s.far_plane;
    Mat4 projection{};
    if (s.projection == CameraProjection::perspective) {
        const double f = 1 / std::tan(double(s.vertical_fov_degrees) * std::numbers::pi / 360);
        projection[0] = float(f / aspect);
        projection[5] = float(-f);
        projection[10] = float(far / (near - far));
        projection[11] = -1;
        projection[14] = float(near * far / (near - far));
    } else {
        projection[0] = float(2 / (double(s.orthographic_height) * aspect));
        projection[5] = -2 / s.orthographic_height;
        projection[10] = float(1 / (near - far));
        projection[14] = float(near / (near - far));
        projection[15] = 1;
    }
    const auto result = projection * view;
    // Match the renderer's finite/invertible matrix and view-origin contract.
    (void)inverse(result);
    (void)view_origin(result);
    return result;
}
template <class Scenes> Mat4 resolve(Scenes &scenes, float aspect) {
    detail::SceneDriver::check(scenes);
    ComponentRef<CameraView> selected;
    for (auto view : scenes.template components<CameraView>()) {
        if (!view.active())
            continue;
        if (selected)
            throw std::invalid_argument("Camera selection has multiple active views");
        selected = view;
    }
    if (!selected)
        throw std::invalid_argument("Camera selection requires one active view");
    const auto object = selected->camera;
    if (!object.valid() || !contains(scenes, object))
        throw std::invalid_argument("Selected camera must be a live object in the scene selection");
    auto camera = object.template get_component<Camera>();
    if (!camera || !camera.active())
        throw std::invalid_argument("Selected camera must have an active Camera component");
    return camera_matrix(object, camera->settings(), aspect);
}
} // namespace
Camera::Camera(CameraSettings settings) { configure(settings); }
void Camera::configure(CameraSettings settings) {
    validate(settings);
    settings_ = settings;
}
Mat4 view_matrix(Scene &scene, float aspect) { return resolve(scene, aspect); }
Mat4 view_matrix(SceneSet &scenes, float aspect) { return resolve(scenes, aspect); }
void add_camera_component_codecs(ComponentCodecs &codecs) {
    auto pending = codecs;
    using Json = nlohmann::json;
    pending.add<Camera>(
        "anima.camera.v1",
        [](const Camera &camera, const ObjectReferences &) {
            const auto &s = camera.settings();
            return Json{{"projection", s.projection == CameraProjection::perspective ? "perspective" : "orthographic"},
                        {"vertical_fov_degrees", s.vertical_fov_degrees},
                        {"orthographic_height", s.orthographic_height},
                        {"near_plane", s.near_plane},
                        {"far_plane", s.far_plane}}
                .dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(
                j, {"projection", "vertical_fov_degrees", "orthographic_height", "near_plane", "far_plane"});
            CameraSettings s;
            if (j.at("projection") == "perspective")
                s.projection = CameraProjection::perspective;
            else if (j.at("projection") == "orthographic")
                s.projection = CameraProjection::orthographic;
            else
                throw std::invalid_argument("Unknown camera projection");
            const auto number = [&](const char *field) {
                if (!j.at(field).is_number())
                    throw std::invalid_argument("Invalid camera number");
                return detail::json_float(j.at(field));
            };
            s.vertical_fov_degrees = number("vertical_fov_degrees");
            s.orthographic_height = number("orthographic_height");
            s.near_plane = number("near_plane");
            s.far_plane = number("far_plane");
            object.add_component<Camera>(s);
        });
    pending.add<CameraView>(
        "anima.camera-view.v1",
        [](const CameraView &view, const ObjectReferences &references) {
            return Json{{"camera", references.key(view.camera).string()}}.dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &references) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"camera"});
            if (!j.at("camera").is_string())
                throw std::invalid_argument("Camera view requires an object key string");
            const auto camera = references.resolve(ObjectKey::parse(j.at("camera").get<std::string>()));
            object.add_component<CameraView>()->camera = camera;
        });
    codecs = std::move(pending);
}
} // namespace anima
