// Scene operations whose cost the scaling checks compare at two object counts: scene_scaling_tests
// counts the bytes each allocates, and scene_scaling_instructions.cmake the instructions each runs
// under Callgrind. Each scenario builds its own scene, passes only the operation it names to
// @p measure, which runs it once and returns a measurement, checks the result and returns that
// measurement. A failed check throws `std::logic_error`.
#pragma once
#include <anima/scene.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

namespace scene_scaling {
// Throws `std::logic_error` with @p failure unless @p accepted.
inline void require(bool accepted, const char *failure) {
    if (!accepted)
        throw std::logic_error(failure);
}

struct Tag {
    int value{};
};
struct Ticking {
    unsigned *updates;
    void on_update(double) { ++*updates; }
};

// A one-triangle mesh with one node.
inline std::shared_ptr<const anima::Mesh> triangle() {
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

// A root with @p count - 1 descendants, each a child of the root or, when @p deep, of the object
// created before it.
inline anima::GameObject hierarchy(anima::Scene &scene, std::size_t count, bool deep) {
    auto root = scene.create(), parent = root;
    for (std::size_t i = 1; i < count; ++i) {
        auto child = scene.create();
        child.set_parent(parent, anima::ReparentMode::keep_local);
        if (deep)
            parent = child;
    }
    return root;
}

// Attaches each of @p count new objects to the one created before it.
template <class Measure> auto chain(std::size_t count, Measure &&measure) {
    anima::Scene scene;
    std::vector<anima::GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create());
    const auto measured = measure([&] {
        for (std::size_t i = 1; i < count; ++i)
            objects[i].set_parent(objects[i - 1], anima::ReparentMode::keep_local);
    });
    require(objects.back().parent()->id() == objects[count - 2].id(), "Attaching built the wrong chain");
    return measured;
}

// Destroys the @p count - 1 children of one parent one at a time.
template <class Measure> auto destroy_children(std::size_t count, Measure &&measure) {
    anima::Scene scene;
    auto children = hierarchy(scene, count, false).children();
    const auto measured = measure([&] {
        for (auto &child : children)
            child.destroy();
    });
    require(scene.size() == 1, "Destroying children left objects behind");
    return measured;
}

// Destroys @p count root objects that render @p mesh one at a time.
template <class Measure>
auto destroy_renderers(std::size_t count, const std::shared_ptr<const anima::Mesh> &mesh, Measure &&measure) {
    anima::Scene scene;
    std::vector<anima::GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create({}, mesh));
    const auto measured = measure([&] {
        for (auto &object : objects)
            object.destroy();
    });
    require(scene.size() == 0 && scene.instances().empty(), "Destroying renderers left objects behind");
    return measured;
}

// Destroys the root of a hierarchy of @p count objects, a chain when @p deep.
template <class Measure> auto destroy_subtree(std::size_t count, bool deep, Measure &&measure) {
    anima::Scene scene;
    auto root = hierarchy(scene, count, deep);
    const auto measured = measure([&] { root.destroy(); });
    require(scene.size() == 0, "Destroying a subtree left objects behind");
    return measured;
}

// Destroys and replaces the objects in the lowest and highest of @p count slots @p count / 2 times.
// The replacements reuse those slots, the second after every slot between them.
template <class Measure> auto reuse_slots(std::size_t count, Measure &&measure) {
    anima::Scene scene;
    std::vector<anima::GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create());
    const auto measured = measure([&] {
        for (std::size_t i = 0; i < count; i += 2) {
            objects.front().destroy();
            objects.back().destroy();
            objects.front() = scene.create();
            objects.back() = scene.create();
        }
    });
    require(scene.size() == count && objects.front().id().slot == 0 && objects.back().id().slot == count - 1,
            "Slot reuse changed the scene or skipped a free slot");
    return measured;
}

// Queries the Ticking components of a scene of @p count objects, a multiple of 64, which all have a
// Tag and 64 of which, spread across the scene, also have a Ticking.
template <class Measure> auto query(std::size_t count, Measure &&measure) {
    anima::Scene scene;
    unsigned updates = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto object = scene.create();
        (void)object.add_component<Tag>();
        if (i % (count / 64) == 0)
            (void)object.add_component<Ticking>(&updates);
    }
    std::vector<anima::ComponentRef<Ticking>> found;
    const auto measured = measure([&] { found = scene.components<Ticking>(); });
    require(found.size() == 64, "The query missed a component");
    return measured;
}
} // namespace scene_scaling
