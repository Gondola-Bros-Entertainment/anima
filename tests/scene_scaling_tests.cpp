// Pins how scene work grows with the objects it touches, through the bytes it allocates, and what a
// failed allocation leaves behind. This executable links allocation_counter.cpp, which replaces the
// global allocation functions, so it counts, and can fail, every allocation the engine makes during a
// measured operation.
#include "allocation_counter.hpp"
#include <anima/input_scene.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
// Bytes allocated while @p operation runs.
template <class Operation> std::size_t allocated_by(Operation &&operation) {
    allocation_counter::bytes = 0;
    allocation_counter::counting = true;
    operation();
    allocation_counter::counting = false;
    return allocation_counter::bytes;
}

// Whether @p operation throws std::bad_alloc when only its first @p allowed allocations succeed.
template <class Operation> bool fails_after(std::size_t allowed, Operation &&operation) {
    allocation_counter::allowed = allowed;
    allocation_counter::failing = true;
    try {
        operation();
    } catch (const std::bad_alloc &) {
        allocation_counter::failing = false;
        return true;
    }
    allocation_counter::failing = false;
    return false;
}
// While MSVC's iterator debugging is on, as in its Debug configuration, its library allocates a container proxy in
// noexcept constructors such as std::vector's default one, so a failure injected there terminates the program.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool noexcept_constructors_allocate = true;
#else
constexpr bool noexcept_constructors_allocate = false;
#endif

// A one-triangle mesh with one node.
std::shared_ptr<const anima::Mesh> triangle() {
    anima::Asset source;
    source.nodes.resize(1);
    anima::SourcePrimitive primitive;
    for (const auto corner : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    source.primitives.push_back(primitive);
    return anima::Mesh::compile(source);
}

struct Tag {
    int value{};
};
struct Ticking {
    unsigned *updates;
    void on_update(double) { ++*updates; }
};

// Bytes allocated by one update of a scene where @p count objects have only a component without
// hooks and one more object has a component with an update hook.
std::size_t sparse_update_bytes(std::size_t count) {
    anima::Scene scene;
    for (std::size_t i = 0; i < count; ++i)
        (void)scene.create().add_component<Tag>();
    unsigned updates = 0;
    (void)scene.create().add_component<Ticking>(&updates);
    scene.update(0); // Delivers the first on_enable, so the measured update only ticks.
    const auto bytes = allocated_by([&] { scene.update(0); });
    REQUIRE(updates == 2);
    return bytes;
}

// Bytes allocated while @p count new objects are attached to one parent.
std::size_t wide_attach_bytes(std::size_t count) {
    anima::Scene scene;
    auto parent = scene.create();
    std::vector<anima::GameObject> children;
    for (std::size_t i = 0; i < count; ++i)
        children.push_back(scene.create());
    return allocated_by([&] {
        for (auto &child : children)
            child.set_parent(parent);
    });
}

// Bytes allocated while two input contexts, whose maps bind @p extra more actions to other keys with a modifier,
// receive a second increment of a frame and a key's press and release.
std::size_t dispatch_bytes(std::size_t extra) {
    namespace i = anima::input;
    i::Map map{{"look", i::ActionType::axis, {{{i::ControlKind::mouse_motion, 0}}}},
               {"jump", i::ActionType::button, {{{i::ControlKind::key, 44}}}}};
    for (std::size_t k = 0; k < extra; ++k)
        map.push_back({"action_" + std::to_string(k),
                       i::ActionType::button,
                       {{{i::ControlKind::key, static_cast<std::uint16_t>(100 + k)},
                         i::Channel::x,
                         1,
                         0,
                         {{i::ControlKind::key, 500}}}}});
    anima::Scene scene;
    auto first = scene.create().add_component<i::ActionInput>(map);
    (void)scene.create().add_component<i::ActionInput>(map);
    const i::Event motion{i::EventType::control, {i::ControlKind::mouse_motion, 0, 0}, 1.5F};
    i::begin_frame(scene);
    i::dispatch(scene, motion); // Starts the frame's sums, so the measured increment adds to them.
    const auto bytes = allocated_by([&] {
        i::dispatch(scene, motion);
        i::dispatch(scene, {i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
        i::dispatch(scene, {i::EventType::control, {i::ControlKind::key, 44, 0}, 0});
    });
    REQUIRE(first->context().state("look").value.x == 3);
    REQUIRE(first->context().state("jump").pressed);
    REQUIRE(first->context().state("jump").released);
    return bytes;
}
} // namespace

TEST_CASE("Attaching children allocates no more per child as their parent grows") {
    // Reallocating the parent's child list on each attach would allocate 16 times as many bytes
    // per child at the larger count.
    const auto small = wide_attach_bytes(256), large = wide_attach_bytes(4096);
    CHECK(large <= 16 * small);
}

TEST_CASE("An update allocates nothing for objects and components without hooks") {
    // Snapshotting every attached component would allocate for each of the 4,096 extra objects'
    // transform and tag.
    const auto small = sparse_update_bytes(16), large = sparse_update_bytes(4096);
    CHECK(large <= small);
}

TEST_CASE("Moving rendered objects and hierarchies allocates nothing after a move as large") {
    const auto mesh = triangle();
    anima::Scene scene;
    auto root = scene.create("Root", mesh);
    std::vector<anima::GameObject> children;
    for (unsigned i = 0; i < 64; ++i) {
        children.push_back(scene.create({}, mesh));
        children.back().set_parent(root);
    }
    root.set_position({1, 0, 0}); // The largest move, which sizes the scene's working storage.
    const auto bytes = allocated_by([&] {
        for (unsigned i = 2; i < 10; ++i) {
            root.set_position({static_cast<float>(i), 0, 0});
            for (auto &child : children)
                child.set_position({static_cast<float>(i), static_cast<float>(i), 0});
        }
    });
    CHECK(bytes == 0);
    // The moves were published: the last child's triangle starts at (9, 9).
    CHECK(children.back().renderer().bounds().minimum.x > 8.9F);
    CHECK(children.back().renderer().bounds().minimum.y > 8.9F);
}

TEST_CASE("Dispatching input allocates no more for a larger action map") {
    // Staging each event on a copy of every context would allocate every action's bindings and modifiers per event.
    const auto small = dispatch_bytes(0), large = dispatch_bytes(126);
    CHECK(large <= small);
}

TEST_CASE("Dispatching input changes no context when an allocation fails" *
          doctest::skip(noexcept_constructors_allocate)) {
    namespace i = anima::input;
    const i::Map map{{"look", i::ActionType::axis, {{{i::ControlKind::mouse_motion, 0}}}},
                     {"jump", i::ActionType::button, {{{i::ControlKind::key, 44}}}}};
    anima::Scene scene;
    auto first = scene.create().add_component<i::ActionInput>(map);
    auto second = scene.create().add_component<i::ActionInput>(map);
    i::begin_frame(scene);
    // Each dispatch fails at its first allocation, then its second, and so on, until it succeeds.
    std::size_t failures = 0;
    const i::Event motion{i::EventType::control, {i::ControlKind::mouse_motion, 0, 0}, 1.5F};
    for (std::size_t allowed = 0; fails_after(allowed, [&] { i::dispatch(scene, motion); }); ++allowed) {
        ++failures;
        CHECK(first->context().state("look").value.x == 0);
        CHECK(second->context().state("look").value.x == 0);
    }
    CHECK(failures > 0);
    // The failed dispatches started no sum, so the increment counts once.
    CHECK(first->context().state("look").value.x == 1.5F);
    CHECK(second->context().state("look").value.x == 1.5F);
    failures = 0;
    const i::Event press{i::EventType::control, {i::ControlKind::key, 44, 0}, 1};
    for (std::size_t allowed = 0; fails_after(allowed, [&] { i::dispatch(scene, press); }); ++allowed) {
        ++failures;
        CHECK_FALSE(first->context().state("jump").pressed);
        CHECK_FALSE(second->context().state("jump").pressed);
    }
    CHECK(failures > 0);
    CHECK(first->context().state("jump").pressed);
    CHECK(second->context().state("jump").pressed);
}
