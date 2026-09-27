#include "consumer/scene_objects.hpp"
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using namespace anima;
constexpr auto parenting_cycle = "GameObject parenting would create a cycle";

// Runtime ids of @p objects, in order.
std::vector<Scene::Id> ids_of(const std::vector<GameObject> &objects) {
    std::vector<Scene::Id> result;
    for (const auto &object : objects)
        result.push_back(object.id());
    return result;
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

TEST_CASE("Creation reuses the lowest free slot through interleaved creation and subtree removal") {
    Scene scene;
    std::vector<GameObject> live;
    std::set<std::size_t> vacant; // The free slots, as a model of the scene's reuse order.
    std::size_t created_slots = 0;
    std::uint32_t sequence = 12345;
    const auto below = [&](std::size_t bound) {
        sequence = sequence * 1664525U + 1013904223U; // A fixed linear congruential sequence.
        return static_cast<std::size_t>(sequence >> 8) % bound;
    };
    for (unsigned round = 0; round < 2000; ++round) {
        CAPTURE(round);
        if (live.empty() || below(4) < 2) {
            const auto expected = vacant.empty() ? created_slots++ : vacant.extract(vacant.begin()).value();
            auto object = scene.create();
            CHECK(object.id().slot == expected);
            // Attach half of them, so removal frees whole subtrees.
            if (!live.empty() && below(2))
                object.set_parent(live[below(live.size())], ReparentMode::keep_local);
            live.push_back(object);
        } else {
            auto removed = live[below(live.size())];
            removed.destroy();
            std::erase_if(live, [&](const GameObject &object) {
                if (object.valid())
                    return false;
                vacant.insert(object.id().slot);
                return true;
            });
        }
    }
    CHECK(scene.size() == live.size());
    CHECK(created_slots > live.size()); // Some slots were free at the end, so both paths ran.
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

TEST_CASE("Children keep their attachment order through removal, reparenting and slot reuse") {
    Scene scene;
    auto parent = scene.create("Parent"), other = scene.create("Other");
    std::vector<GameObject> children;
    for (unsigned i = 0; i < 5; ++i) {
        children.push_back(scene.create());
        children.back().set_parent(parent);
    }
    const auto expect = [&](const GameObject &owner, std::vector<GameObject> expected) {
        CHECK(ids_of(owner.children()) == ids_of(expected));
    };
    expect(parent, children);
    children[2].destroy(); // Middle.
    expect(parent, {children[0], children[1], children[3], children[4]});
    children[0].set_parent(other); // First.
    children[4].clear_parent();    // Last.
    expect(parent, {children[1], children[3]});
    expect(other, {children[0]});
    children[0].set_parent(parent); // Reattached children go last.
    children[4].set_parent(parent);
    expect(parent, {children[1], children[3], children[0], children[4]});
    expect(other, {});
    children[3].destroy();
    children[1].destroy();
    expect(parent, {children[0], children[4]});
    auto reused = scene.create(); // Reuses the lowest destroyed child's slot without its links.
    CHECK(reused.id().slot == children[1].id().slot);
    CHECK(reused.children().empty());
    CHECK_FALSE(reused.parent());
    reused.set_parent(parent);
    expect(parent, {children[0], children[4], reused});
    parent.destroy();
    CHECK(scene.size() == 1);
    CHECK_FALSE(children[0].valid());
    CHECK_FALSE(reused.valid());
    expect(other, {});
}

TEST_CASE("Reparenting rejects a parent inside the moved subtree and changes nothing") {
    Scene scene;
    auto root = scene.create("Root"), branch = scene.create("Branch"), sibling = scene.create("Sibling");
    branch.set_parent(root);
    sibling.set_parent(root);
    std::vector<GameObject> chain{branch};
    for (unsigned i = 0; i < 64; ++i) {
        auto next = scene.create();
        next.set_parent(chain.back());
        chain.push_back(next);
    }
    auto offshoot = scene.create("Offshoot");
    offshoot.set_parent(chain[10]);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        CAPTURE(i);
        CHECK_THROWS_WITH_AS(branch.set_parent(chain[i]), parenting_cycle, std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(branch.set_parent(offshoot), parenting_cycle, std::invalid_argument);
    CHECK_THROWS_WITH_AS(root.set_parent(chain.back()), parenting_cycle, std::invalid_argument);
    CHECK_THROWS_WITH_AS(offshoot.set_parent(offshoot), parenting_cycle, std::invalid_argument);
    CHECK(ids_of(root.children()) == ids_of({branch, sibling}));
    CHECK(ids_of(chain[10].children()) == ids_of({chain[11], offshoot}));
    CHECK_FALSE(root.parent());
    branch.set_parent(sibling, ReparentMode::keep_local);
    CHECK(ids_of(root.children()) == ids_of({sibling}));
    CHECK(ids_of(sibling.children()) == ids_of({branch}));
    CHECK(chain.back().parent()->id() == chain[chain.size() - 2].id());
}

TEST_CASE("The shared consumer scenario for objects, lifetime, terrain, mesh preparation and animation passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    scene_objects_test::run();
}
