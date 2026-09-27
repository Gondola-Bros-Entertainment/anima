// Pins how scene work grows with the objects it touches, through the bytes it allocates. This
// executable replaces the global allocation functions, so it counts every allocation the engine
// makes during a measured operation.
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace {
std::size_t counted_allocation_bytes = 0;
bool counting_allocations = false;

// Bytes allocated while @p operation runs.
template <class Operation> std::size_t allocated_by(Operation &&operation) {
    counted_allocation_bytes = 0;
    counting_allocations = true;
    operation();
    counting_allocations = false;
    return counted_allocation_bytes;
}

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
} // namespace

void *operator new(std::size_t bytes) {
    if (counting_allocations)
        counted_allocation_bytes += bytes;
    if (void *allocation = std::malloc(bytes ? bytes : 1))
        return allocation;
    throw std::bad_alloc();
}
void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }

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
