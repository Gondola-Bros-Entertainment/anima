#pragma once
#include <anima/scene_set.hpp>

namespace anima::detail {
// Built-in drivers run between scene phases, never from an incomplete attachment
// or another component's callback. Drivers that dispatch external callbacks hold
// a Scope through publication to prevent nested scheduling and membership changes.
struct SceneDriver {
    static void check(const Scene &scene) {
        if (scene.updating_ || scene.constructing_ || !scene.lifetime_->scene)
            throw std::logic_error("Scene drivers require an idle live scene");
    }
    static void check(const SceneSet &scenes) {
        scenes.require_idle();
        for (auto scene : scenes.scenes())
            check(scene.get());
    }
    class Scope {
      public:
        explicit Scope(Scene &scene) : scenes_{&scene} {
            check(scene);
            scene.updating_ = true;
        }
        // SceneSet::serialize holds its set as a driver does, naming its own activity.
        explicit Scope(SceneSet &scenes, SceneSet::Activity activity = SceneSet::Activity::driving) : set_(&scenes) {
            check(scenes);
            scenes_.reserve(scenes.scenes_.size());
            for (const auto &record : scenes.scenes_)
                scenes_.push_back(record->scene.get());
            scenes.activity_ = activity;
            for (auto *scene : scenes_)
                scene->updating_ = true;
        }
        ~Scope() {
            for (auto *scene : scenes_)
                scene->updating_ = false;
            if (set_)
                set_->activity_ = SceneSet::Activity::idle;
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

      private:
        std::vector<Scene *> scenes_;
        SceneSet *set_{};
    };
};
} // namespace anima::detail
