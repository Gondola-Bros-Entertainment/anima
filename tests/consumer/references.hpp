#pragma once
#include "rejection.hpp"
#include <anima/prefab.hpp>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace references_test {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
using rejection::rejects;
inline std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    check(at != std::string::npos, "Missing reference test field");
    value.replace(at, from.size(), to);
    return value;
}
struct Link {
    GameObject target;
};
inline ComponentCodecs codecs() {
    ComponentCodecs result;
    result.add<Link>(
        "test.link.v1",
        [](const Link &link, const ObjectReferences &references) { return references.key(link.target).string(); },
        [](GameObject object, std::string_view state, const ObjectReferences &references) {
            object.add_component<Link>(references.resolve(ObjectKey::parse(state)));
        });
    return result;
}
struct Owned {
    int *live;
    explicit Owned(int &value) : live(&value) { ++*live; }
    ~Owned() { --*live; }
};
inline void run() {
    auto registry = codecs();
    Scene scene;
    auto retired = scene.create("same label");
    const auto retired_key = retired.key();
    const auto retired_slot = retired.id().slot;
    retired.destroy();
    auto root = scene.create("same label");
    check(root.key() != retired_key && root.id().slot == retired_slot && !scene.find(retired_key).valid(),
          "Slot reuse recycled persistent identity");
    const auto root_key = root.key();
    auto child = scene.create("same label");
    child.set_parent(root);
    child.set_active(false);
    root.set_name("renamed");
    root.set_position({2, 3, 4});
    check(root.key() == root_key && scene.find(root_key).id() == root.id(), "Ordinary mutation changed identity");
    root.add_component<Link>(child);
    child.add_component<Link>(root);
    auto self = scene.create();
    self.add_component<Link>(self);
    auto null = scene.create();
    null.add_component<Link>(GameObject{});
    const auto self_key = self.key(), child_key = child.key(), null_key = null.key();
    const auto document = serialize_scene(scene, {}, registry);
    auto loaded = load_scene(document, {}, registry);
    check(serialize_scene(*loaded, {}, registry) == document, "Persistent scene identity did not round-trip");
    check(loaded->find(root_key).get_component<Link>()->target.id() == loaded->find(child_key).id() &&
              loaded->find(child_key).get_component<Link>()->target.id() == loaded->find(root_key).id() &&
              loaded->find(self_key).get_component<Link>()->target.id() == loaded->find(self_key).id() &&
              !loaded->find(null_key).get_component<Link>()->target.valid(),
          "Forward/backward/cyclic/self/null references did not bind to the loaded scene");
    check(!loaded->find(child_key).active_in_hierarchy(), "Reference load lost activation");

    // A full scene resolves links between roots; capturing one subtree rejects them.
    null.get_component<Link>()->target = child;
    auto cross = load_scene(serialize_scene(scene, {}, registry), {}, registry);
    check(cross->find(null_key).get_component<Link>()->target.key() == child_key,
          "Cross-root reference was not captured as part of the whole scene");
    rejects<std::invalid_argument>([&] { (void)Prefab::capture(null, registry); },
                                   "Object reference is stale or outside the captured graph");
    null.get_component<Link>()->target = {};
    Scene foreign;
    auto outsider = foreign.create();
    null.get_component<Link>()->target = outsider;
    rejects<std::invalid_argument>([&] { (void)serialize_scene(scene, {}, registry); },
                                   "Object reference is stale or outside the captured graph");
    null.get_component<Link>()->target = retired;
    rejects<std::invalid_argument>([&] { (void)serialize_scene(scene, {}, registry); },
                                   "Object reference is stale or outside the captured graph");
    null.get_component<Link>()->target = {};

    auto prefab = Prefab::deserialize(Prefab::capture(root, registry).serialize({}), {}, registry);
    const auto first = prefab.instantiate(scene), second = prefab.instantiate(scene);
    auto first_child = first.children()[0], second_child = second.children()[0];
    check(first.key() != second.key() && first.key() != root.key() &&
              first.get_component<Link>()->target.id() == first_child.id() &&
              second.get_component<Link>()->target.id() == second_child.id() &&
              first_child.get_component<Link>()->target.id() == first.id() &&
              second_child.get_component<Link>()->target.id() == second.id(),
          "Prefab links escaped their own remapped instances");
    auto parent = scene.create();
    parent.set_active(false);
    auto nested = prefab.instantiate(parent);
    check(!nested.active_in_hierarchy() && nested.get_component<Link>()->target.id() == nested.children()[0].id(),
          "Nested prefab remapping or inherited activation failed");
    auto saved_link = first.get_component<Link>()->target;
    const auto removed_key = first_child.key();
    first_child.destroy();
    auto reuse = scene.create();
    check(!saved_link.valid() && !scene.find(removed_key).valid() && reuse.key() != removed_key,
          "Removed reference regained validity");

    // Explicit contexts are checked, remain weak, and can be retained by value.
    const std::array objects{root, child};
    ObjectReferences references(objects);
    check(references.key({}) == ObjectKey{} && !references.resolve({}).valid(), "Null reference contract failed");
    rejects<std::invalid_argument>([&] { (void)references.key(outsider); },
                                   "Object reference is stale or outside the captured graph");
    rejects<std::invalid_argument>([&] { (void)references.resolve(ObjectKey{99999}); },
                                   "Object reference target is missing or expired");
    rejects<std::invalid_argument>([&] { (void)registry.capture(root, {}); },
                                   "Object reference is stale or outside the captured graph"); // no graph was supplied
    const std::array duplicates{root, root};
    rejects<std::invalid_argument>([&] { (void)ObjectReferences(duplicates); },
                                   "Invalid or duplicate object reference mapping");
    const std::array bad_entries{ObjectReferences::Entry{{1}, root}, ObjectReferences::Entry{{1}, child}};
    rejects<std::invalid_argument>([&] { (void)ObjectReferences(bad_entries); },
                                   "Invalid or duplicate object reference mapping");
    root.destroy();
    rejects<std::invalid_argument>([&] { (void)references.resolve(root_key); },
                                   "Object reference target is missing or expired");
    check(!scene.find(root_key).valid() && !scene.find(child_key).valid(), "Subtree removal left key lookup entries");
    ObjectReferences expired;
    GameObject handle;
    ObjectKey saved_key;
    {
        Scene temporary;
        handle = temporary.create();
        saved_key = handle.key();
        const std::array only{handle};
        expired = ObjectReferences(only);
    }
    check(!handle.valid(), "References kept scene alive");
    rejects<std::invalid_argument>([&] { (void)expired.resolve(saved_key); },
                                   "Object reference target is missing or expired");

    // Deleted high keys remain retired even after saving an empty scene.
    Scene history;
    auto last = history.create();
    const auto last_key = last.key();
    last.destroy();
    auto history_copy = load_scene(serialize_scene(history, {}), {});
    check(history_copy->create().key().value > last_key.value, "Empty scene reload reused a deleted key");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto empty = serialize_scene(history, {});
    auto exhausted =
        load_scene(replace(empty, "\"next_key\": \"2\"", "\"next_key\": \"" + ObjectKey{maximum}.string() + "\""), {});
    auto final = exhausted->create();
    check(final.key().value == maximum, "Maximum persistent key lost precision");
    auto maximum_copy = load_scene(serialize_scene(*exhausted, {}), {});
    check(maximum_copy->find({maximum}).valid(), "Maximum object key did not survive document roundtrip");
    rejects<std::overflow_error>([&] { (void)maximum_copy->create(); }, "Scene object keys exhausted");
    rejects<std::overflow_error>([&] { (void)exhausted->create(); }, "Scene object keys exhausted");
    final.destroy();
    exhausted = load_scene(serialize_scene(*exhausted, {}), {});
    rejects<std::overflow_error>([&] { (void)exhausted->create(); }, "Scene object keys exhausted");
    check(ObjectKey::parse(ObjectKey{maximum}.string()).value == maximum, "Key string roundtrip lost precision");
    for (const auto bad : {"", "01", "-1", "+1", " 1", "1 ", "1.0", "1e0", "18446744073709551616"})
        rejects<std::invalid_argument>([&] { (void)ObjectKey::parse(bad); }, "Invalid object key");

    // Missing links roll back every staged object/resource, preserving the destination.
    int live = 0;
    std::vector<GameObject> staged;
    std::vector<ObjectKey> staged_keys;
    auto with_owner = registry;
    with_owner.add<Owned>(
        "test.owned.v1", [](const Owned &, const ObjectReferences &) { return ""; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            staged.push_back(object);
            staged_keys.push_back(object.key());
            object.add_component<Owned>(live);
        });
    Scene destination;
    auto existing = destination.create();
    existing.add_component<Owned>(live);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes[0].components.push_back({"test.owned.v1", "", true});
    nodes[1].components[0].state = "999999";
    const auto before = destination.size();
    rejects<std::invalid_argument>([&] { (void)Prefab(nodes, with_owner).instantiate(destination); },
                                   "Object reference target is missing or expired");
    check(destination.size() == before && live == 1 && staged.size() == 1 && !staged[0].valid() &&
              !destination.find(staged_keys[0]).valid() && destination.find(existing.key()).id() == existing.id(),
          "Failed reference fixup leaked objects/resources or rebound to existing state");
    existing.destroy();
    rejects<std::invalid_argument>(
        [&] {
            (void)load_scene(replace(document, "\"state\": \"" + child_key.string() + "\"", "\"state\": \"999999\""),
                             {}, registry);
        },
        "Object reference target is missing or expired");
    nodes[1].key = nodes[0].key;
    rejects<std::invalid_argument>([&] { (void)Prefab(nodes, registry); }, "Duplicate prefab object key");

    Scene plain;
    auto a = plain.create(), b = plain.create();
    a.set_active(false);
    b.set_parent(a);
    const auto v3 = serialize_scene(plain, {});
    // Key 0 is well formed but null; the others are not keys at all.
    const std::array<std::pair<const char *, std::string_view>, 4> bad_keys{
        {{"0", "Null or duplicate document object key"},
         {"01", "Invalid object key"},
         {"-1", "Invalid object key"},
         {"18446744073709551616", "Invalid object key"}}};
    for (const auto &bad : bad_keys)
        rejects<std::invalid_argument>(
            [&] { (void)load_scene(replace(v3, "\"key\": \"1\"", "\"key\": \"" + std::string(bad.first) + "\""), {}); },
            bad.second);
    rejects<std::invalid_argument>([&] { (void)load_scene(replace(v3, "\"key\": \"2\"", "\"key\": \"1\""), {}); },
                                   "Null or duplicate document object key");
    rejects<std::invalid_argument>([&] { (void)load_scene(replace(v3, "\"key\": \"1\"", "\"key\": 1"), {}); },
                                   "[json.exception.type_error.302] type must be string, but is number");
    rejects<std::invalid_argument>([&] { (void)load_scene(replace(v3, "\"key\": \"1\",", ""), {}); },
                                   "Missing JSON field: key");
    rejects<std::invalid_argument>(
        [&] { (void)load_scene(replace(v3, "\"key\": \"1\"", "\"key\": \"1\", \"k\\u0065y\": \"2\""), {}); },
        "Duplicate JSON document field");
    rejects<std::invalid_argument>(
        [&] { (void)load_scene(replace(v3, "\"next_key\": \"3\"", "\"next_key\": \"2\""), {}); },
        "Invalid scene next object key");
}
} // namespace references_test
