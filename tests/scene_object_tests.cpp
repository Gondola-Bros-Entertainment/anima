#include "consumer/scene_objects.hpp"
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace anima;
using scene_objects_test::rejects;
using scene_objects_test::require;

void append_and_reuse_slots() {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < 256; ++i) {
        auto object = scene.create();
        require(object.id().slot == i && scene.find(object.key()).id() == object.id(),
                "Appending objects skipped a slot or lost its key");
        objects.push_back(object);
    }
    const std::array<std::size_t, 3> holes{23, 70, 89};
    const std::array retired_ids{objects[23].id(), objects[70].id(), objects[89].id()};
    const std::array retired_keys{objects[23].key(), objects[70].key(), objects[89].key()};
    for (auto index : {89U, 23U, 70U})
        objects[index].destroy();
    for (std::size_t i = 0; i < holes.size(); ++i) {
        auto replacement = scene.create();
        require(replacement.id().slot == holes[i] && replacement.id().generation == retired_ids[i].generation + 1 &&
                    !objects[holes[i]].valid() && !scene.find(retired_keys[i]).valid(),
                "Recycling holes changed first-free order or revived an old identity");
    }
    require(scene.create().id().slot == 256 && scene.size() == 257,
            "Object growth did not resume after filling all holes");
}

struct CleanupState {
    Scene *scene{};
    std::vector<GameObject> retired, created;
    bool failed{};
};
struct CreateOnDisable {
    CleanupState *state;
    void on_disable() noexcept {
        try {
            for (auto object : state->retired)
                if (object.valid())
                    state->failed = true;
            state->created.push_back(state->scene->create("Cleanup first"));
            state->created.push_back(state->scene->create("Cleanup second"));
        } catch (...) {
            state->failed = true;
        }
    }
};
void subtree_cleanup_reuses_slots() {
    CleanupState cleanup; // Application state outlives scene component teardown.
    Scene scene;
    cleanup.scene = &scene;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 12; ++i)
        objects.push_back(scene.create());
    objects[2].set_parent(objects[7], ReparentMode::keep_local);
    objects[9].set_parent(objects[7], ReparentMode::keep_local);
    objects[5].set_parent(objects[2], ReparentMode::keep_local);
    cleanup.retired = {objects[7], objects[2], objects[9], objects[5]};
    objects[7].add_component<CreateOnDisable>(&cleanup);
    scene.synchronize_lifecycle();
    objects[7].destroy();
    require(!cleanup.failed && cleanup.created.size() == 2 && cleanup.created[0].id().slot == 2 &&
                cleanup.created[1].id().slot == 5,
            "Cleanup callbacks did not see the fully retired subtree or reuse its first slots");
    require(scene.create().id().slot == 7 && scene.create().id().slot == 9 && scene.create().id().slot == 12,
            "Subtree retirement lost holes after reentrant creation");
    for (auto object : cleanup.retired)
        require(!object.valid(), "Reentrant creation revived a removed subtree handle");
}

void failed_creation_reuses_slot() {
    auto source = std::make_shared<Asset>(*scene_objects_test::source());
    // Compiling accepts finite authored geometry; adding the renderer rejects
    // overflow when its conservative world bounds are padded.
    source->primitives[0].vertices[0].position.x = std::numeric_limits<float>::max();
    const auto oversized_mesh = Mesh::compile(*source);
    Scene scene;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 4; ++i)
        objects.push_back(scene.create());
    const auto retired = objects[1].id();
    objects[1].destroy();
    const ObjectKey failed_key{objects.back().key().value + 1};
    rejects([&] { (void)scene.create("Overflow", oversized_mesh); });
    require(scene.size() == 3 && scene.instances().empty() && !scene.find(failed_key).valid(),
            "Failed creation published an object, renderer or key");
    const auto replacement = scene.create();
    require(replacement.id().slot == retired.slot && replacement.id().generation == retired.generation + 2 &&
                replacement.key().value == failed_key.value + 1 && !objects[1].valid(),
            "Failed creation lost its free slot or reused a consumed identity");
    require(scene.create().id().slot == 4, "Failed creation corrupted later append allocation");
}
} // namespace

int main() {
    try {
        append_and_reuse_slots();
        subtree_cleanup_reuses_slots();
        failed_creation_reuses_slot();
        scene_objects_test::run();
        std::cout << "PASS scene objects, lifetime, terrain, mesh preparation and independent animation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
