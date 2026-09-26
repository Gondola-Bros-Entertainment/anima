#include <anima/components.hpp>
#include <anima/scene_set.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string_view>

// Each scenario destroys a Scene or SceneSet from inside one of its own callbacks, which must
// terminate during that destruction. Returning, or terminating anywhere else, fails the scenario.
// The idle scenario destroys a different, idle scene from a callback, which must not terminate.
// Every scenario ends its process, so CTest runs each one separately instead of a doctest suite.
namespace {
using namespace anima;

std::shared_ptr<Scene> owned_scene;
std::shared_ptr<Scene> idle_scene;
std::unique_ptr<SceneSet> owned_set;
bool destroying = false;
bool expect_termination = true;

template <class Owner> void destroy(Owner &owner) {
    destroying = true;
    owner.reset();
    destroying = false;
}

struct DestroyOnUpdate {
    void on_update(double) { destroy(owned_scene); }
};
struct DestroyOnLateUpdate {
    void on_late_update(double) { destroy(owned_scene); }
};
struct DestroyOnFixedUpdate {
    void on_fixed_update(double) { destroy(owned_scene); }
};
struct DestroyOnDisable {
    void on_enable() noexcept {}
    void on_disable() noexcept { destroy(owned_scene); }
};
struct DestroyOnConstruction {
    DestroyOnConstruction() { destroy(owned_scene); }
};
struct DestroySetOnUpdate {
    void on_update(double) { destroy(owned_set); }
};
struct DestroyIdleSceneOnUpdate {
    void on_update(double) { destroy(idle_scene); }
};

GameObject enabled_object() {
    auto object = owned_scene->create();
    object.add_component<DestroyOnDisable>();
    owned_scene->synchronize_lifecycle();
    return object;
}

const std::map<std::string_view, void (*)()> scenarios{
    {"update",
     [] {
         owned_scene->create().add_component<DestroyOnUpdate>();
         owned_scene->update(0);
     }},
    {"late_update",
     [] {
         owned_scene->create().add_component<DestroyOnLateUpdate>();
         owned_scene->update(0);
     }},
    {"fixed_update",
     [] {
         owned_scene->create().add_component<DestroyOnFixedUpdate>();
         owned_scene->fixed_update(0);
     }},
    {"disable_on_destroy", [] { enabled_object().destroy(); }},
    {"disable_on_remove", [] { enabled_object().remove_component<DestroyOnDisable>(); }},
    {"construction", [] { owned_scene->create().add_component<DestroyOnConstruction>(); }},
    {"scene_set_update",
     [] {
         owned_set = std::make_unique<SceneSet>();
         owned_set->create("member").get().create().add_component<DestroySetOnUpdate>();
         owned_set->update(0);
     }},
    {"idle",
     [] {
         expect_termination = false;
         idle_scene = std::make_shared<Scene>();
         owned_scene->create().add_component<DestroyIdleSceneOnUpdate>();
         owned_scene->update(0);
     }},
};

[[noreturn]] void on_terminate() noexcept {
    // _Exit skips the destructors and leak checks that the abandoned scene would trip.
    const bool passed = destroying && expect_termination;
    std::fputs(passed ? "PASS terminated during destruction\n" : "FAIL unexpected termination\n", stderr);
    std::_Exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 || !scenarios.contains(argv[1])) {
        std::fputs("usage: anima_scene_destruction_tests <scenario>\n", stderr);
        return EXIT_FAILURE;
    }
    std::set_terminate(on_terminate);
    owned_scene = std::make_shared<Scene>();
    scenarios.at(argv[1])();
    if (expect_termination) {
        std::fputs("FAIL destruction returned instead of terminating\n", stderr);
        return EXIT_FAILURE;
    }
    std::puts("PASS destroying an idle scene from a callback");
}
