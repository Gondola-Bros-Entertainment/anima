#pragma once
#include <anima/lighting.hpp>
#include <anima/scene_set.hpp>
#include <stdexcept>

namespace lighting_consumer {
inline void run() {
    anima::ComponentCodecs codecs;
    anima::add_lighting_component_codecs(codecs);
    anima::Scene authored;
    auto rig = authored.create("lighting rig");
    anima::EnvironmentSettings settings;
    settings.sky = true;
    settings.fog_density = .025F;
    settings.exposure = 1.25F;
    auto environment = rig.add_component<anima::SceneEnvironment>(settings);
    auto sun = authored.create("sun"), fill = authored.create("fill");
    sun.set_parent(rig, anima::ReparentMode::keep_local);
    fill.set_parent(rig, anima::ReparentMode::keep_local);
    sun.set_transform({.rotation = {0, 1, 0, 0}, .scale = {2, 3, 4}});
    sun.add_component<anima::DirectionalLightComponent>(anima::Vec3{2, 1, .5F});
    fill.add_component<anima::DirectionalLightComponent>(anima::Vec3{});
    environment->sun = sun;
    environment->fill = fill;
    const auto prefab = anima::Prefab::deserialize(anima::Prefab::capture(rig, codecs).serialize({}), {}, codecs);
    anima::SceneSet scenes;
    auto level = scenes.create("level");
    auto active = prefab.instantiate(level.get()), alternate = prefab.instantiate(level.get());
    alternate.set_active(false);
    const auto light = active.get_component<anima::SceneEnvironment>()->sun;
    if (light.id() != active.children()[0].id() || light.id() == sun.id())
        throw std::runtime_error("Independent lighting prefab did not remap its source");
    const auto result = anima::lighting_environment(scenes);
    if (result.sun.direction.z != -1 || result.sun.radiance.x != 2 || result.fill.radiance.x != 0 || !result.sky ||
        result.fog_density != .025F || result.exposure != 1.25F)
        throw std::runtime_error("Independent scene lighting changed authored settings");
    level = scenes.replace(level, anima::serialize_scene(level.get(), {}, codecs), {}, codecs);
    const auto restored = anima::lighting_environment(scenes);
    if (light.valid() || restored.sun.direction.z != result.sun.direction.z || restored.exposure != result.exposure)
        throw std::runtime_error("Lighting replacement retained handles or changed its result");
    auto selected = level->components<anima::SceneEnvironment>().front();
    selected->sun.destroy();
    bool rejected = false;
    try {
        (void)anima::lighting_environment(scenes);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Independent lighting silently repaired a stale light");
}
} // namespace lighting_consumer
