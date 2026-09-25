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
}
} // namespace camera_consumer
