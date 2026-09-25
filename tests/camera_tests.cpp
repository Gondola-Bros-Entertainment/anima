#include "component_payloads.hpp"
#include <anima/assets/render_visibility.hpp>
#include <anima/camera.hpp>
#include <anima/scene_set.hpp>
#include <iostream>
#include <limits>

using namespace anima;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(float a, float b) { check(std::abs(a - b) < 2e-5F, "Camera numeric mismatch"); }
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Expected camera rejection");
}
void equal(const Mat4 &a, const Mat4 &b) {
    for (unsigned i = 0; i < 16; ++i)
        near(a[i], b[i]);
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
void projection_and_pose() {
    Scene scene;
    auto eye = camera(scene);
    auto selected = view(scene, eye);
    auto lens = eye.get_component<Camera>();
    CameraSettings settings;
    settings.vertical_fov_degrees = 90;
    settings.near_plane = 1;
    settings.far_plane = 11;
    lens->configure(settings);
    auto m = view_matrix(scene, 2);
    near(project(m, {0, 0, -1}).z, 0);
    near(project(m, {0, 0, -11}).z, 1);
    near(project(m, {2, 1, -1}).x, 1);
    near(project(m, {2, 1, -1}).y, -1);
    near(view_origin(m)[3], 1);
    check(RenderFrustum(m).intersects({{-.1F, -.1F, -3}, {.1F, .1F, -2}, true}), "Camera culling lost visible box");
    check(!RenderFrustum(m).intersects({{20, 0, -3}, {21, 1, -2}, true}), "Camera culling retained outside box");
    settings.projection = CameraProjection::orthographic;
    settings.orthographic_height = 4;
    lens->configure(settings);
    m = view_matrix(scene, 2);
    near(project(m, {4, 2, -1}).x, 1);
    near(project(m, {4, 2, -1}).y, -1);
    near(project(m, {4, 2, -1}).z, 0);
    near(project(m, {4, 2, -11}).z, 1);
    near(view_origin(m)[3], 0);
    auto parent = scene.create();
    parent.set_transform({.translation = {10, 3, 4}, .rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    eye.set_parent(parent, ReparentMode::keep_local);
    eye.set_local_position({1, 0, 0});
    m = view_matrix(scene, 2);
    near(project(m, {8, 3, 5}).z, 0);
    near(project(m, {8, 3, 15}).z, 1);
    const auto scaled = m;
    eye.clear_parent();
    eye.set_transform({.translation = {8, 3, 4}, .rotation = {0, 1, 0, 0}});
    equal(scaled, view_matrix(scene, 2));
    auto invalid = eye.world_matrix();
    invalid[4] = .3F;
    eye.set_world_matrix(invalid);
    rejects([&] { (void)view_matrix(scene, 2); });
    for (float scale : {0.F, -1.F}) {
        eye.set_transform({.scale = {scale, 1, 1}});
        rejects([&] { (void)view_matrix(scene, 2); });
    }
    eye.set_world_matrix(identity());
    for (float aspect : {0.F, -1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::denorm_min()})
        rejects([&] { (void)view_matrix(scene, aspect); });
    const auto accepted = view_matrix(scene, 2);
    for (unsigned field = 0; field < 5; ++field) {
        auto bad = settings;
        if (field == 0)
            bad.vertical_fov_degrees = 180;
        if (field == 1)
            bad.orthographic_height = 0;
        if (field == 2)
            bad.near_plane = 0;
        if (field == 3)
            bad.far_plane = bad.near_plane;
        if (field == 4)
            bad.projection = static_cast<CameraProjection>(99);
        rejects([&] { lens->configure(bad); });
        equal(accepted, view_matrix(scene, 2));
    }
    (void)selected;
}
void selection_and_lifetime() {
    SceneSet set;
    auto a = set.create("a"), b = set.create("b");
    auto eye = camera(a.get());
    auto selected = view(b.get(), eye);
    const auto accepted = view_matrix(set, 1);
    set.set_active(b);
    equal(accepted, view_matrix(set, 1));
    rejects([&] { (void)view_matrix(b.get(), 1); }); // Foreign to this standalone selection.
    auto other = camera(b.get());
    other.set_position({4, 0, 0});
    selected->camera = other;
    near(view_origin(view_matrix(set, 1))[0], 4);
    auto extra = view(a.get(), eye);
    rejects([&] { (void)view_matrix(set, 1); });
    extra.set_enabled(false);
    (void)view_matrix(set, 1);
    selected.set_enabled(false);
    rejects([&] { (void)view_matrix(set, 1); });
    selected.set_enabled(true);
    auto group = b->create();
    other.set_parent(group);
    group.set_active(false);
    rejects([&] { (void)view_matrix(set, 1); });
    group.set_active(true);
    other.get_component<Camera>().set_enabled(false);
    rejects([&] { (void)view_matrix(set, 1); });
    other.remove_component<Camera>();
    rejects([&] { (void)view_matrix(set, 1); });
    other.add_component<Camera>(); // Link deliberately selects the object's current attachment.
    (void)view_matrix(set, 1);
    other.destroy();
    (void)camera(b.get());
    rejects([&] { (void)view_matrix(set, 1); });
    selected->camera = {};
    rejects([&] { (void)view_matrix(set, 1); });
    selected->camera = eye;
    set.unload(a);
    a = set.create("a");
    (void)camera(a.get());
    rejects([&] { (void)view_matrix(set, 1); });
    set.clear();
    check(!selected, "View handle retained unloaded scene");
    rejects([&] { (void)view_matrix(set, 1); });
}
struct Reentry {
    SceneSet *scenes;
    Scene *scene;
    unsigned *calls;
    void on_update(double) {
        rejects([&] { (void)view_matrix(*scene, 1); });
        rejects([&] { (void)view_matrix(*scenes, 1); });
        ++*calls;
    }
};
struct Construction {
    Construction(Scene &scene) {
        rejects([&] { (void)view_matrix(scene, 1); });
    }
};
void boundaries() {
    SceneSet set;
    auto scene = set.create("level");
    auto eye = camera(scene.get());
    auto selected = view(scene.get(), eye);
    unsigned calls = 0;
    eye.add_component<Reentry>(&set, &scene.get(), &calls);
    eye.add_component<Construction>(scene.get());
    set.update(0);
    check(calls == 1, "Camera test callback did not run");
    (void)view_matrix(set, 1);
    selected.object().set_active(false);
    rejects([&] { (void)view_matrix(set, 1); });
}
void persistence() {
    ComponentCodecs codecs;
    add_camera_component_codecs(codecs);
    Scene scene;
    auto root = scene.create("rig");
    auto selected = root.add_component<CameraView>();
    auto eye = camera(scene);
    eye.set_parent(root, ReparentMode::keep_local);
    eye.set_position({3, 2, 1});
    selected->camera = eye; // View decodes before its child's Camera.
    CameraSettings settings{CameraProjection::orthographic, 73, 8, .2F, 500};
    eye.get_component<Camera>()->configure(settings);
    auto prefab = Prefab::deserialize(Prefab::capture(root, codecs).serialize({}), {}, codecs);
    auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    check(first.get_component<CameraView>()->camera.id() == first.children()[0].id() &&
              second.get_component<CameraView>()->camera.id() == second.children()[0].id(),
          "Prefab camera link not remapped");
    first.set_active(false);
    second.get_component<CameraView>().set_enabled(false);
    const auto accepted = view_matrix(scene, 2);
    auto restored = load_scene(serialize_scene(scene, {}, codecs), {}, codecs);
    equal(accepted, view_matrix(*restored, 2));
    check(!restored->find(first.key()).active_self() &&
              !restored->find(second.key()).get_component<CameraView>().enabled(),
          "Camera view activation lost");
    const auto &lens = restored->find(eye.key()).get_component<Camera>()->settings();
    check(lens.projection == settings.projection && lens.vertical_fov_degrees == 73 && lens.orthographic_height == 8 &&
              lens.near_plane == .2F && lens.far_plane == 500,
          "Camera lens persistence lost settings");
    rejects([&] { add_camera_component_codecs(codecs); });
    equal(accepted, view_matrix(*load_scene(serialize_scene(scene, {}, codecs), {}, codecs), 2));
    selected->camera = second.children()[0];
    rejects([&] { (void)Prefab::capture(root, codecs); });
    selected->camera = {};
    auto empty = Prefab::capture(root, codecs).instantiate(scene);
    check(!empty.get_component<CameraView>()->camera.valid(), "Null selection was rebound");
    empty.destroy();
    selected->camera = eye;
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    for (unsigned node : {0U, 1U}) {
        auto &data = nodes[node].components[0].state;
        const auto valid = data;
        const auto field = node == 0 ? "camera" : "near_plane";
        auto invalids = invalid_component_payloads(valid, field);
        invalids.push_back(std::string(64 * 1024 + 1, ' '));
        if (node == 0) {
            for (auto invalid : {"{\"camera\":0}", "{\"camera\":\"01\"}", "{\"camera\":\"99999\"}"})
                invalids.emplace_back(invalid);
        } else {
            for (const auto bad : {"true", "\"1\"", "-1", "1e100"}) {
                auto invalid = valid;
                const auto begin = invalid.find("\"near_plane\":") + 13;
                const auto end = invalid.find_first_of(",}", begin);
                invalid.replace(begin, end - begin, bad);
                invalids.push_back(invalid);
            }
        }
        for (const auto &invalid : invalids) {
            data = invalid;
            const auto size = scene.size();
            rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
            check(scene.size() == size, "Invalid camera payload leaked staged objects");
            equal(accepted, view_matrix(scene, 2));
        }
        data = valid;
    }
    // Failing the second codec registration must not publish the first.
    ComponentCodecs conflict;
    conflict.add<CameraView>(
        "anima.camera-view.v1", [](const CameraView &, const ObjectReferences &) { return "{}"; },
        [](GameObject o, std::string_view, const ObjectReferences &) { o.add_component<CameraView>(); });
    rejects([&] { add_camera_component_codecs(conflict); });
    rejects([&] { (void)Prefab::capture(eye, conflict); });
}
} // namespace
int main() {
    try {
        projection_and_pose();
        selection_and_lifetime();
        boundaries();
        persistence();
        std::cout << "PASS camera projection, selection, persistence and lifetime\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
