#include "consumer/lifecycle.hpp"
#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

namespace {
using anima::GameObject;
using anima::Scene;

// Records its object's slot when it updates.
struct SlotLog {
    std::vector<std::size_t> *visits;
    std::size_t slot;
    void on_update(double) { visits->push_back(slot); }
};
struct Counted {
    int *destroyed;
    ~Counted() { ++*destroyed; }
};
struct CountedHooks {
    int *destroyed;
    ~CountedHooks() { ++*destroyed; }
    void on_update(double) {}
};
// Destructions of each counted component in total and when Remover removed it.
struct Removal {
    int plain{}, hooked{};
    int plain_at_removal = -1, hooked_at_removal = -1;
};
// Removes both counted components from @p target during its first update.
struct Remover {
    GameObject target;
    Removal *removal;
    void on_update(double) {
        if (removal->plain_at_removal >= 0)
            return;
        (void)target.remove_component<Counted>();
        removal->plain_at_removal = removal->plain;
        (void)target.remove_component<CountedHooks>();
        removal->hooked_at_removal = removal->hooked;
    }
};
} // namespace

TEST_CASE("The shared consumer scenario for inherited activation, lifecycle, scheduling and persistence passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    lifecycle_test::run();
}

TEST_CASE("Queries and updates visit components in slot order through removal and slot reuse") {
    Scene scene;
    std::vector<std::size_t> visits;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 16; ++i)
        objects.push_back(scene.create());
    const auto attach = [&](GameObject object) { (void)object.add_component<SlotLog>(&visits, object.id().slot); };
    for (auto index : {9U, 3U, 12U, 0U, 7U, 15U, 5U}) // Out of slot order.
        attach(objects[index]);
    const auto expect = [&](const std::vector<std::size_t> &slots) {
        std::vector<std::size_t> queried;
        for (const auto &component : scene.components<SlotLog>())
            queried.push_back(component.object().id().slot);
        CHECK(queried == slots);
        visits.clear();
        scene.update(0);
        CHECK(visits == slots);
    };
    expect({0, 3, 5, 7, 9, 12, 15});
    objects[3].destroy();
    CHECK(objects[12].remove_component<SlotLog>());
    expect({0, 5, 7, 9, 15});
    attach(scene.create()); // Reuses slot 3.
    attach(objects[12]);
    expect({0, 3, 5, 7, 9, 12, 15});
}

TEST_CASE("An update pins removed components with hooks until it ends and destroys others at once") {
    Removal removal;
    Scene scene;
    auto target = scene.create();
    (void)target.add_component<Counted>(&removal.plain);
    (void)target.add_component<CountedHooks>(&removal.hooked);
    (void)scene.create().add_component<Remover>(target, &removal);
    scene.update(0);
    CHECK(removal.plain_at_removal == 1);
    CHECK(removal.hooked_at_removal == 0);
    CHECK(removal.hooked == 1);
    CHECK(scene.components<Counted>().empty());
    CHECK(scene.components<CountedHooks>().empty());
}
