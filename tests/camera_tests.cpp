#include "component_payloads.hpp"
#include "near.hpp"
#include <anima/assets/render_visibility.hpp>
#include <anima/camera.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr float tolerance = 2e-5F; // Clip coordinates, view origins and view-projection elements.
constexpr auto busy_scene = "Scene drivers require an idle live scene";
constexpr auto no_view = "Camera selection requires one active view";
constexpr auto dead_camera = "Selected camera must be a live object in the scene selection";
constexpr auto inactive_camera = "Selected camera must have an active Camera component";
constexpr auto skewed_axes = "Camera world axes must be orthogonal and right-handed";
constexpr auto invalid_lens = "Invalid camera lens settings";
constexpr auto duplicate_codec = "Duplicate component codec";
// Whether each element of @p a is within the tolerance of the one of @p b.
bool same(const Mat4 &a, const Mat4 &b) {
    return std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) { return x == Near{y, tolerance}; });
}
Vec3 project(const Mat4 &m, Vec3 p) {
    const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    return point(m, p) * (1 / w);
}
GameObject camera(Scene &scene) {
    auto object = scene.create("camera");
    object.add_component<Camera>();
    return object;
}
auto view(Scene &scene, GameObject selected) {
    auto result = scene.create("view").add_component<CameraView>();
    result->camera = selected;
    return result;
}
// A lens that sees from 1 to 11 units ahead, with a 90 degree field or an orthographic height of 4.
CameraSettings lens_settings(CameraProjection projection) {
    CameraSettings settings;
    settings.projection = projection;
    settings.vertical_fov_degrees = 90;
    settings.orthographic_height = 4;
    settings.near_plane = 1;
    settings.far_plane = 11;
    return settings;
}
// A scene whose one view selects a camera with lens_settings().
struct Viewed {
    Scene scene;
    GameObject eye = camera(scene);
    ComponentRef<CameraView> selected = view(scene, eye);
    explicit Viewed(CameraProjection projection) { eye.get_component<Camera>()->configure(lens_settings(projection)); }
};
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
        CHECK_THROWS_WITH_AS(view_matrix(*scene, 1), busy_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(view_matrix(*scenes, 1), "Scene set is updating", std::logic_error);
        ++*calls;
    }
};
struct Construction {
    Construction(Scene &scene) { CHECK_THROWS_WITH_AS(view_matrix(scene, 1), busy_scene, std::logic_error); }
};
// A rig whose view selects its child camera, with the camera codecs registered.
struct Rig {
    ComponentCodecs codecs;
    Scene scene;
    GameObject root = scene.create("rig");
    ComponentRef<CameraView> selected = root.add_component<CameraView>();
    GameObject eye = camera(scene);
    CameraSettings lens{CameraProjection::orthographic, 73, 8, .2F, 500};
    Rig() {
        add_camera_component_codecs(codecs);
        eye.set_parent(root, ReparentMode::keep_local);
        eye.set_position({3, 2, 1});
        selected->camera = eye; // The view decodes before its child's Camera.
        eye.get_component<Camera>()->configure(lens);
    }
};
} // namespace

TEST_CASE("Perspective and orthographic lenses map the view volume to Vulkan clip space with reversed depth") {
    Viewed viewed(CameraProjection::perspective);
    auto m = view_matrix(viewed.scene, 2);
    CHECK(project(m, {0, 0, -1}).z == Near{1, tolerance});
    CHECK(project(m, {0, 0, -11}).z == Near{0, tolerance});
    CHECK(project(m, {2, 1, -1}).x == Near{1, tolerance});
    CHECK(project(m, {2, 1, -1}).y == Near{-1, tolerance});
    CHECK(view_origin(m)[3] == Near{1, tolerance});
    CHECK(RenderFrustum(m).intersects({{-.1F, -.1F, -3}, {.1F, .1F, -2}, true}));
    CHECK_FALSE(RenderFrustum(m).intersects({{20, 0, -3}, {21, 1, -2}, true}));
    viewed.eye.get_component<Camera>()->configure(lens_settings(CameraProjection::orthographic));
    m = view_matrix(viewed.scene, 2);
    CHECK(project(m, {4, 2, -1}).x == Near{1, tolerance});
    CHECK(project(m, {4, 2, -1}).y == Near{-1, tolerance});
    CHECK(project(m, {4, 2, -1}).z == Near{1, tolerance});
    CHECK(project(m, {4, 2, -11}).z == Near{0, tolerance});
    CHECK(view_origin(m)[2] == Near{1, tolerance}); // Toward the camera, which looks down -Z.
    CHECK(view_origin(m)[3] == Near{0, tolerance});
}

TEST_CASE("orthographic() maps a box to Vulkan clip space with reversed depth, as an orthographic camera does") {
    // 4 units high and 8 wide, from 1 to 11 units ahead.
    const auto m = orthographic(2, 4, 1, 11);
    CHECK(project(m, {4, 2, -1}).x == Near{1, tolerance});
    CHECK(project(m, {4, 2, -1}).y == Near{-1, tolerance}); // Framebuffer Y points down.
    CHECK(project(m, {-4, -2, -11}).x == Near{-1, tolerance});
    CHECK(project(m, {-4, -2, -11}).y == Near{1, tolerance});
    CHECK(project(m, {0, 0, -1}).z == Near{1, tolerance});
    CHECK(project(m, {0, 0, -6}).z == Near{.5F, tolerance}); // Linear in distance.
    CHECK(project(m, {0, 0, -11}).z == Near{0, tolerance});
    // Composed with a view, it yields the unit direction toward the camera, with W 0.
    const Vec3 eye{3, 4, 5}, target{1, 2, -3};
    const auto origin = view_origin(m * look_at(eye, target));
    const auto toward = normalized(eye - target);
    CHECK(origin[0] == Near{toward.x, tolerance});
    CHECK(origin[1] == Near{toward.y, tolerance});
    CHECK(origin[2] == Near{toward.z, tolerance});
    CHECK(origin[3] == Near{0, tolerance});
    // A camera at the origin with the same lens resolves to exactly this projection.
    Viewed viewed(CameraProjection::orthographic);
    const auto camera_view = view_matrix(viewed.scene, 2);
    CHECK(std::equal(camera_view.begin(), camera_view.end(), m.begin()));
    // The planes may lie at or behind the eye.
    const auto around = orthographic(1, 2, -5, 5);
    CHECK(project(around, {0, 0, 5}).z == Near{1, tolerance});
    CHECK(project(around, {0, 0, 0}).z == Near{.5F, tolerance});
    CHECK(project(around, {0, 0, -5}).z == Near{0, tolerance});

    const auto invalid = math_error_message(MathErrorCode::invalid_orthographic);
    CHECK(std::string_view(invalid) == "Invalid orthographic volume");
    CHECK(math_error_name(MathErrorCode::invalid_orthographic) == "invalid_orthographic");
    constexpr float infinity = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    // Aspect, height and the near and far planes.
    for (const auto &bad :
         {std::array{0.F, 4.F, 1.F, 11.F}, std::array{-1.F, 4.F, 1.F, 11.F}, std::array{2.F, 0.F, 1.F, 11.F},
          std::array{2.F, 4.F, 11.F, 11.F}, std::array{2.F, 4.F, 11.F, 1.F}, std::array{infinity, 4.F, 1.F, 11.F},
          std::array{2.F, infinity, 1.F, 11.F}, std::array{2.F, 4.F, -infinity, 11.F},
          std::array{2.F, 4.F, 1.F, infinity}, std::array{nan, 4.F, 1.F, 11.F}, std::array{2.F, 4.F, nan, 11.F}}) {
        CAPTURE(bad[0]);
        CAPTURE(bad[1]);
        CAPTURE(bad[2]);
        CAPTURE(bad[3]);
        CHECK_THROWS_WITH_AS(orthographic(bad[0], bad[1], bad[2], bad[3]), invalid, MathError);
    }
}

TEST_CASE("The view follows the camera's world pose and ignores its positive scale") {
    Viewed viewed(CameraProjection::orthographic);
    auto parent = viewed.scene.create();
    parent.set_transform({.translation = {10, 3, 4}, .rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    viewed.eye.set_parent(parent, ReparentMode::keep_local);
    viewed.eye.set_local_position({1, 0, 0});
    // The parent's half turn about Y puts the eye at (8, 3, 4), looking along +Z.
    const auto scaled = view_matrix(viewed.scene, 2);
    CHECK(project(scaled, {8, 3, 5}).z == Near{1, tolerance});
    CHECK(project(scaled, {8, 3, 15}).z == Near{0, tolerance});
    viewed.eye.clear_parent();
    viewed.eye.set_transform({.translation = {8, 3, 4}, .rotation = {0, 1, 0, 0}});
    CHECK(same(scaled, view_matrix(viewed.scene, 2)));
}

TEST_CASE("Sheared, collapsed and mirrored camera axes are rejected") {
    Viewed viewed(CameraProjection::orthographic);
    auto sheared = identity();
    sheared[4] = .3F;
    viewed.eye.set_world_matrix(sheared);
    CHECK_THROWS_WITH_AS(view_matrix(viewed.scene, 2), skewed_axes, std::invalid_argument);
    viewed.eye.set_transform({.scale = {0, 1, 1}});
    CHECK_THROWS_WITH_AS(view_matrix(viewed.scene, 2), "Camera world axes must be nonzero", std::invalid_argument);
    viewed.eye.set_transform({.scale = {-1, 1, 1}});
    CHECK_THROWS_WITH_AS(view_matrix(viewed.scene, 2), skewed_axes, std::invalid_argument);
}

TEST_CASE("An aspect that is not finite and positive, or that overflows the projection, is rejected") {
    Viewed viewed(CameraProjection::orthographic);
    for (float aspect : {0.F, -1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(aspect);
        CHECK_THROWS_WITH_AS(view_matrix(viewed.scene, aspect), "Camera aspect must be finite and positive",
                             std::invalid_argument);
    }
    // Finite and positive, but the orthographic width 2 / (height * aspect) is not a finite float.
    CHECK_THROWS_WITH_AS(view_matrix(viewed.scene, std::numeric_limits<float>::denorm_min()),
                         math_error_message(MathErrorCode::nonfinite_matrix), MathError);
}

TEST_CASE("Invalid lens settings are rejected and keep the accepted view") {
    Viewed viewed(CameraProjection::orthographic);
    auto lens = viewed.eye.get_component<Camera>();
    const auto settings = lens->settings();
    const auto accepted = view_matrix(viewed.scene, 2);
    for (unsigned field = 0; field < 5; ++field) {
        CAPTURE(field);
        auto bad = settings;
        const char *error = invalid_lens;
        if (field == 0)
            bad.vertical_fov_degrees = 180;
        if (field == 1)
            bad.orthographic_height = 0;
        if (field == 2)
            bad.near_plane = 0;
        if (field == 3)
            bad.far_plane = bad.near_plane;
        if (field == 4) {
            bad.projection = static_cast<CameraProjection>(99);
            error = "Unknown camera projection";
        }
        CHECK_THROWS_WITH_AS(lens->configure(bad), error, std::invalid_argument);
        CHECK(same(accepted, view_matrix(viewed.scene, 2)));
    }
}

TEST_CASE("View resolution requires one active view of a live camera with an active lens") {
    SceneSet set;
    auto a = set.create("a"), b = set.create("b");
    auto eye = camera(a.get());
    auto selected = view(b.get(), eye);
    const auto accepted = view_matrix(set, 1);
    set.set_active(b);
    CHECK(same(accepted, view_matrix(set, 1)));
    // The view's camera is foreign to b on its own.
    CHECK_THROWS_WITH_AS(view_matrix(b.get(), 1), dead_camera, std::invalid_argument);
    auto other = camera(b.get());
    other.set_position({4, 0, 0});
    selected->camera = other;
    CHECK(view_origin(view_matrix(set, 1))[0] == Near{4, tolerance});
    auto extra = view(a.get(), eye);
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), "Camera selection has multiple active views", std::invalid_argument);
    extra.set_enabled(false);
    CHECK_NOTHROW(view_matrix(set, 1));
    selected.set_enabled(false);
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), no_view, std::invalid_argument);
    selected.set_enabled(true);
    auto group = b->create();
    other.set_parent(group);
    group.set_active(false);
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), inactive_camera, std::invalid_argument);
    group.set_active(true);
    other.get_component<Camera>().set_enabled(false);
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), inactive_camera, std::invalid_argument);
    other.remove_component<Camera>();
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), inactive_camera, std::invalid_argument);
    other.add_component<Camera>(); // Link deliberately selects the object's current attachment.
    CHECK_NOTHROW(view_matrix(set, 1));
    other.destroy();
    (void)camera(b.get());
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), dead_camera, std::invalid_argument);
    selected->camera = {};
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), dead_camera, std::invalid_argument);
    selected->camera = eye;
    // Without the camera codecs nothing repairs the link.
    CHECK(set.unload(a).empty());
    a = set.create("a");
    (void)camera(a.get());
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), dead_camera, std::invalid_argument);
    set.clear();
    CHECK_FALSE(selected.valid());
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), no_view, std::invalid_argument);
}

TEST_CASE("A view in one member follows its camera through a replacement and is cleared by an unload") {
    ComponentCodecs codecs;
    add_camera_component_codecs(codecs);
    SceneSet set;
    auto views = set.create("views"), cameras = set.create("cameras");
    auto eye = camera(cameras.get());
    eye.set_position({3, 0, 0});
    auto selected = view(views.get(), eye);
    const auto accepted = view_matrix(set, 1);
    const auto eye_key = eye.key();
    const auto document = serialize_scene(cameras.get(), {}, codecs);
    cameras = set.replace(cameras, document, {}, codecs);
    CHECK_FALSE(eye.valid());
    CHECK(selected->camera.id() == cameras->find(eye_key).id());
    CHECK(same(accepted, view_matrix(set, 1)));
    // A replacement without the camera's key fails and keeps the link.
    Scene vacant;
    CHECK_THROWS_WITH_AS((void)set.replace(cameras, serialize_scene(vacant, {}), {}, codecs),
                         "Replacement lacks a linked object key", std::invalid_argument);
    CHECK(same(accepted, view_matrix(set, 1)));
    // The view's own member reloads from a set document; its link resolves by address.
    const auto saved = set.serialize({}, codecs);
    views = set.replace(views, saved, {}, codecs);
    CHECK_FALSE(selected.valid());
    selected = views->components<CameraView>().front();
    CHECK(selected->camera.id() == cameras->find(eye_key).id());
    CHECK(same(accepted, view_matrix(set, 1)));
    const auto cleared = set.unload(cameras, codecs);
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0].owner.id() == selected.object().id());
    CHECK(cleared[0].component == "anima.camera-view.v1");
    CHECK(cleared[0].target == SceneAddress{"cameras", eye_key});
    CHECK(selected->camera.id() == Scene::Id{});
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), dead_camera, std::invalid_argument);
}

TEST_CASE("Views do not resolve from component hooks or construction") {
    SceneSet set;
    auto scene = set.create("level");
    auto eye = camera(scene.get());
    auto selected = view(scene.get(), eye);
    unsigned calls = 0;
    eye.add_component<Reentry>(&set, &scene.get(), &calls);
    eye.add_component<Construction>(scene.get());
    set.update(0);
    CHECK(calls == 1);
    CHECK_NOTHROW(view_matrix(set, 1));
    selected.object().set_active(false);
    CHECK_THROWS_WITH_AS(view_matrix(set, 1), no_view, std::invalid_argument);
}

TEST_CASE_FIXTURE(Rig, "Camera links remap per prefab instance, and lenses and activation persist") {
    const auto prefab = Prefab::deserialize(Prefab::capture(root, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    CHECK(first.get_component<CameraView>()->camera.id() == first.children()[0].id());
    CHECK(second.get_component<CameraView>()->camera.id() == second.children()[0].id());
    first.set_active(false);
    second.get_component<CameraView>().set_enabled(false);
    const auto accepted = view_matrix(scene, 2);
    const auto restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    CHECK(same(accepted, view_matrix(*restored, 2)));
    CHECK_FALSE(restored->find(first.key()).active_self());
    CHECK_FALSE(restored->find(second.key()).get_component<CameraView>().enabled());
    const auto &restored_lens = restored->find(eye.key()).get_component<Camera>()->settings();
    CHECK(restored_lens.projection == lens.projection);
    CHECK(restored_lens.vertical_fov_degrees == 73);
    CHECK(restored_lens.orthographic_height == 8);
    CHECK(restored_lens.near_plane == .2F);
    CHECK(restored_lens.far_plane == 500);
    CHECK_THROWS_WITH_AS(add_camera_component_codecs(codecs), duplicate_codec, std::invalid_argument);
    // The failed registration left the codecs as they were.
    CHECK(same(accepted, view_matrix(*load_scene(serialize_scene(scene, {}, codecs), {}, codecs), 2)));
}

TEST_CASE_FIXTURE(Rig, "Capturing a link outside the prefab is rejected, and a null link stays null") {
    selected->camera = camera(scene);
    CHECK_THROWS_WITH_AS(Prefab::capture(root, codecs), "Object reference is stale or outside the captured graph",
                         std::invalid_argument);
    selected->camera = {};
    const auto unlinked = Prefab::capture(root, codecs).instantiate(scene);
    CHECK_FALSE(unlinked.get_component<CameraView>()->camera.valid());
}

TEST_CASE_FIXTURE(Rig, "Invalid camera payloads are rejected without leaking staged objects") {
    const auto accepted = view_matrix(scene, 2);
    const auto prefab = Prefab::capture(root, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    for (unsigned node : {0U, 1U}) {
        CAPTURE(node);
        auto &data = nodes[node].components[0].state;
        const auto valid = data;
        std::vector<Payload> payloads;
        if (node == 0) {
            payloads = invalid_payloads(valid, "camera", "camera");
            payloads.push_back({R"({"camera":0})", "Camera view requires an object key string"});
            payloads.push_back({R"({"camera":"01"})", "Invalid object key"});
            payloads.push_back({R"({"camera":"99999"})", "Object reference target is missing or expired"});
        } else {
            payloads = invalid_payloads(valid, "near_plane", "projection");
            for (const auto &[bad, error] :
                 {std::pair{"true", "Invalid camera number"}, std::pair{"\"1\"", "Invalid camera number"},
                  std::pair{"-1", invalid_lens}, std::pair{"1e100", "JSON number outside the float range"}}) {
                auto invalid = valid;
                const auto begin = invalid.find("\"near_plane\":") + 13;
                const auto end = invalid.find_first_of(",}", begin);
                invalid.replace(begin, end - begin, bad);
                payloads.push_back({invalid, error});
            }
        }
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            CAPTURE(index);
            data = payloads[index].state;
            const auto size = scene.size();
            CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), payloads[index].error.c_str(),
                                 std::invalid_argument);
            CHECK(scene.size() == size);
            CHECK(same(accepted, view_matrix(scene, 2)));
        }
        data = valid;
    }
}

TEST_CASE("A failed codec registration publishes neither codec") {
    ComponentCodecs conflict;
    conflict.add<CameraView>(
        "anima.camera-view.v1", [](const CameraView &, const ObjectReferences &) { return "{}"; },
        [](GameObject o, std::string_view, const ObjectReferences &) { o.add_component<CameraView>(); });
    CHECK_THROWS_WITH_AS(add_camera_component_codecs(conflict), duplicate_codec, std::invalid_argument);
    Scene scene;
    const auto eye = camera(scene);
    // The camera codec, registered before the conflict, was not published either.
    CHECK_THROWS_WITH_AS(Prefab::capture(eye, conflict), "Component has no persistence codec", std::invalid_argument);
}
