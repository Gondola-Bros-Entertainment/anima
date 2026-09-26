#include "consumer/scene_objects.hpp"
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace anima;

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
} // namespace

TEST_CASE("Objects append in slot order, and recreated objects fill holes first-free with new generations") {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < 256; ++i) {
        CAPTURE(i);
        auto object = scene.create();
        CHECK(object.id().slot == i);
        CHECK(scene.find(object.key()).id() == object.id());
        objects.push_back(object);
    }
    const std::array<std::size_t, 3> holes{23, 70, 89};
    const std::array retired_ids{objects[23].id(), objects[70].id(), objects[89].id()};
    const std::array retired_keys{objects[23].key(), objects[70].key(), objects[89].key()};
    for (auto index : {89U, 23U, 70U})
        objects[index].destroy();
    for (std::size_t i = 0; i < holes.size(); ++i) {
        CAPTURE(i);
        auto replacement = scene.create();
        CHECK(replacement.id().slot == holes[i]);
        CHECK(replacement.id().generation == retired_ids[i].generation + 1);
        CHECK_FALSE(objects[holes[i]].valid());
        CHECK_FALSE(scene.find(retired_keys[i]).valid());
    }
    CHECK(scene.create().id().slot == 256);
    CHECK(scene.size() == 257);
}

TEST_CASE("Cleanup callbacks see a fully retired subtree, and objects they create reuse its first slots") {
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
    CHECK_FALSE(cleanup.failed);
    REQUIRE(cleanup.created.size() == 2);
    CHECK(cleanup.created[0].id().slot == 2);
    CHECK(cleanup.created[1].id().slot == 5);
    // The remaining holes, then growth.
    CHECK(scene.create().id().slot == 7);
    CHECK(scene.create().id().slot == 9);
    CHECK(scene.create().id().slot == 12);
    for (auto object : cleanup.retired)
        CHECK_FALSE(object.valid());
}

TEST_CASE("A failed creation publishes nothing, keeps its free slot and consumes its identity") {
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
    CHECK_THROWS_WITH_AS(scene.create("Overflow", oversized_mesh), "Non-finite render bounds", std::invalid_argument);
    CHECK(scene.size() == 3);
    CHECK(scene.instances().empty());
    CHECK_FALSE(scene.find(failed_key).valid());
    const auto replacement = scene.create();
    CHECK(replacement.id().slot == retired.slot);
    CHECK(replacement.id().generation == retired.generation + 2);
    CHECK(replacement.key().value == failed_key.value + 1);
    CHECK_FALSE(objects[1].valid());
    CHECK(scene.create().id().slot == 4);
}

TEST_CASE("The shared consumer scenario for objects, lifetime, terrain, mesh preparation and animation passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    scene_objects_test::run();
}
