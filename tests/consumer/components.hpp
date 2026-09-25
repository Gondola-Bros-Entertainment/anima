#pragma once
#include <anima/animation.hpp>
#include <functional>

namespace components_test {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F &&operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid component operation was accepted");
}
struct Health {
    int value;
};
struct OwnedData {
    GameObject owner;
    double speed;
};
struct Driver {
    GameObject object;
    double elapsed{}, simulated{};
    unsigned frames{}, fixed{}, late{};
    explicit Driver(GameObject owner) : object(owner) {}
    void on_update(double seconds) {
        elapsed += seconds;
        ++frames;
        object.set_position({static_cast<float>(elapsed), 0, 0});
    }
    void on_fixed_update(double seconds) {
        simulated += seconds;
        ++fixed;
    }
    void on_late_update(double) {
        check(frames == late + 1, "Late update ran before frame update");
        ++late;
    }
};
struct Callback {
    std::function<void()> callback;
    void on_update(double) { callback(); }
};
struct SelfRemoving {
    GameObject object;
    int *destroyed;
    bool *finished;
    SelfRemoving(GameObject owner, int &count, bool &done) : object(owner), destroyed(&count), finished(&done) {}
    ~SelfRemoving() { ++*destroyed; }
    void remove() {
        object.remove_component<SelfRemoving>();
        check(*destroyed == 0, "Self-removal destroyed a component inside its method");
        *finished = true;
    }
    void on_update(double) { remove(); }
};
struct Throwing {
    explicit Throwing(GameObject) { throw std::runtime_error("Constructor failure"); }
};
struct Recursive {
    explicit Recursive(GameObject owner) { (void)owner.add_component<Recursive>(); }
};
struct DestroyingConstructor {
    explicit DestroyingConstructor(GameObject owner) { owner.destroy(); }
};
struct GrowingConstructor {
    explicit GrowingConstructor(Scene &scene) {
        for (unsigned i = 0; i < 128; ++i)
            (void)scene.create();
    }
};
inline void run(const std::shared_ptr<const Asset> &source, const std::shared_ptr<const Mesh> &mesh) {
    Scene scene;
    auto object = scene.create("Composable object");
    check(object.has_component<ObjectTransform>() && !object.get_component<Health>(), "Missing mandatory transform");
    auto transform = object.get_component<ObjectTransform>();
    auto health = object.add_component<Health>(100);
    check(health->value == 100 && health.object().id() == object.id(), "User data component was not attached");
    auto renderer = object.add_component<MeshRenderer>(mesh);
    check(renderer->mesh() == mesh && scene.instances().size() == 1 && object.component_types().size() == 3,
          "Native mesh component does not use the same component registry");
    auto owned = object.add_component<OwnedData>(2.5);
    check(owned->owner.id() == object.id() && owned->speed == 2.5,
          "Aggregate component did not receive its owner and data");
    renderer.set_enabled(false);
    check(!scene.instance(object.id()).visible, "Disabling a renderer component left it visible");
    renderer->set_visible(true);
    check(renderer.enabled(), "Renderer visibility and component enabled state diverged");
    rejects([&] { transform.set_enabled(false); });
    rejects([&] { (void)object.add_component<Health>(1); });
    rejects([&] { (void)object.add_component<ObjectTransform>(); });
    rejects([&] { (void)object.remove_component<ObjectTransform>(); });
    object.remove_component<MeshRenderer>();
    check(!renderer && !object.has_renderer(), "Removing the mesh component left render membership");
    (void)object.add_mesh(mesh);
    check(!renderer && object.get_component<MeshRenderer>(), "Adding a replacement revived an old component handle");
    auto driver = object.add_component<Driver>();
    scene.fixed_update(.02);
    scene.fixed_update(.02);
    scene.update(.25);
    check(driver->frames == 1 && driver->fixed == 2 && driver->late == 1 && driver->simulated == .04 &&
              object.position().x == .25F,
          "Scene-driven frame/fixed/late phases lost caller timing");
    driver.set_enabled(false);
    scene.fixed_update(.02);
    scene.update(.25);
    check(driver->frames == 1 && driver->fixed == 2, "Disabled component received updates");
    driver.set_enabled(true);
    check(scene.components<Driver>().size() == 1 && scene.components<Health>().size() == 1,
          "Typed component query lost live components");
    rejects([&] { scene.update(-1); });
    rejects([&] { (void)object.add_component<Throwing>(); });
    rejects([&] { (void)object.add_component<Recursive>(); });
    check(!object.has_component<Throwing>() && !object.has_component<Recursive>(),
          "Failed construction retained a type reservation");
    (void)object.add_component<GrowingConstructor>(scene);
    check(object.has_component<GrowingConstructor>(), "Component construction lost its owner after scene growth");
    auto doomed = scene.create();
    rejects([&] { (void)doomed.add_component<DestroyingConstructor>(); });
    check(!doomed.valid(), "Component construction revived its destroyed owner");
    int destroyed = 0;
    bool finished = false;
    auto self = object.add_component<SelfRemoving>(destroyed, finished);
    self->remove();
    check(finished && destroyed == 1 && !self, "Component call did not pin self-removal or release afterward");
    destroyed = 0;
    finished = false;
    self = object.add_component<SelfRemoving>(destroyed, finished);
    scene.update(0);
    check(finished && destroyed == 1 && !self, "Scene update mishandled self-removal");

    Scene mutations;
    auto controller = mutations.create();
    auto victim = mutations.create();
    auto child = mutations.create();
    child.set_parent(victim);
    auto victim_driver = victim.add_component<Driver>();
    auto child_driver = child.add_component<Driver>();
    ComponentRef<Driver> added;
    auto callback = controller.add_component<Callback>(std::function<void()>([&] {
        if (victim.valid()) {
            victim.destroy();
            added = mutations.create().add_component<Driver>();
        }
    }));
    mutations.update(.1);
    check(!victim_driver && !child_driver && added->frames == 0 && added->late == 0,
          "Mutation during update ran removed or newly attached components");
    mutations.update(.1);
    check(added->frames == 1 && added->late == 1, "New component did not start on the next update");
    callback->callback = [&] { mutations.fixed_update(.01); };
    rejects([&] { mutations.update(.1); });
    callback.set_enabled(false);
    mutations.update(.1);
    check(added->frames == 2, "Callback exception left the scene update locked");

    auto animated = scene.create("Native animator", mesh);
    auto animator = animated.add_component<Animator>(source);
    animator->select({"Move", false, {{.25, "step"}}, {}});
    scene.update(.5);
    check(animator->playback().time() == .5 && animator->events().size() == 1 &&
              scene.instance(animated.id()).palette[0][13] == 1,
          "Animator component did not advance or retain clip events");
    scene.fixed_update(.02);
    check(animator->playback().time() == .5, "Fixed update double-advanced frame animation");
    object.destroy();
    check(!health && !transform && !driver, "Destroyed object retained valid component handles");
    rejects([&] { (void)health->value; });
    ComponentRef<Health> expired;
    {
        Scene temporary;
        expired = temporary.create().add_component<Health>(1);
    }
    check(!expired, "Component handle extended scene lifetime");
}
} // namespace components_test
