#pragma once
#include <anima/camera.hpp>
#include <anima/scene_set.hpp>
#include <stdexcept>

namespace camera_consumer {
inline void run() {
    anima::ComponentCodecs codecs;
    anima::add_camera_component_codecs(codecs);
    anima::Scene authored;
    auto rig = authored.create("player view");
    auto selection = rig.add_component<anima::CameraView>();
    auto eye = authored.create("eye");
    eye.set_parent(rig, anima::ReparentMode::keep_local);
    eye.set_local_position({0, 2, 5});
    eye.add_component<anima::Camera>();
    selection->camera = eye;
    const auto prefab = anima::Prefab::deserialize(anima::Prefab::capture(rig, codecs).serialize({}), {}, codecs);
    anima::SceneSet scenes;
    auto level = scenes.create("level");
    auto player = prefab.instantiate(level.get());
    auto preview = prefab.instantiate(level.get());
    preview.set_active(false);
    const auto selected_eye = player.get_component<anima::CameraView>()->camera;
    if (selected_eye.id() != player.children()[0].id() || selected_eye.id() == eye.id())
        throw std::runtime_error("Independent camera prefab link remapping failed");
    const auto origin = anima::view_origin(anima::view_matrix(scenes, 16.F / 9));
    if (std::abs(origin[1] - 2) > 1e-5F || std::abs(origin[2] - 5) > 1e-5F)
        throw std::runtime_error("Independent camera did not follow its object");
    const auto document = anima::serialize_scene(level.get(), {}, codecs);
    const auto before = anima::view_matrix(scenes, 1);
    level = scenes.replace(level, document, {}, codecs);
    if (selected_eye.valid() || before != anima::view_matrix(scenes, 1))
        throw std::runtime_error("Camera scene replacement retained handles or changed its view");

    anima::SceneSet linked;
    auto cameras = linked.create("cameras"), views = linked.create("views");
    auto lens = cameras->create("lens");
    lens.add_component<anima::Camera>();
    lens.set_position({0, 1, 4});
    (void)views->create("remote view").add_component<anima::CameraView>(lens);
    const auto remote = anima::view_matrix(linked, 1);
    cameras = linked.replace(cameras, anima::serialize_scene(cameras.get(), {}, codecs), {}, codecs);
    if (lens.valid() || remote != anima::view_matrix(linked, 1))
        throw std::runtime_error("Camera replacement lost the view in another member");
    const auto cleared = linked.unload(cameras, codecs);
    if (cleared.size() != 1 || views->components<anima::CameraView>().front()->camera.valid())
        throw std::runtime_error("Camera unload did not clear the view in another member");
}
} // namespace camera_consumer
