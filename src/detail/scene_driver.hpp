#pragma once
#include <anima/scene_set.hpp>

namespace anima::detail {
// Built-in drivers publish state without callbacks. They must run after scene
// phases, never from an incomplete attachment or another component's callback.
struct SceneDriver {
    static void check(const Scene &scene) {
        if (scene.updating_ || scene.constructing_ || !scene.lifetime_->scene)
            throw std::logic_error("Scene drivers require an idle live scene");
    }
    static void check(const SceneSet &scenes) {
        if (scenes.mutating_)
            throw std::logic_error("Scene drivers cannot run during set mutation or scheduling");
        for (auto scene : scenes.scenes())
            check(scene.get());
    }
};
} // namespace anima::detail
