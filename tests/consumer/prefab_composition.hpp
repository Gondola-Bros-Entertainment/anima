#pragma once
#include <anima/prefab_composition.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prefab_composition_test {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid prefab composition operation was accepted");
}
inline Mat4 translated(float x) {
    auto result = identity();
    result[12] = x;
    return result;
}
inline std::vector<PrefabComposition::Part> parts() {
    return {{"root", "shared", std::nullopt, translated(5)},
            {"middle", "shared", PrefabComposition::Mount{"root", {99}}, translated(7)},
            {"leaf", "shared", PrefabComposition::Mount{"middle", {99}}, translated(11)}};
}
struct Counts {
    int live{}, enabled{};
    Scene *destination{};
    std::size_t expected_objects{}, fail_after_links{}, retirement_start{};
    bool all_native_ready = true, check_retirement = false, all_retired = true;
    std::vector<GameObject> decoded;
};
struct Link {
    GameObject target;
    int binding;
    Counts *counts;
    Link(GameObject object, int service, Counts &values) : target(object), binding(service), counts(&values) {
        ++counts->live;
    }
    ~Link() {
        --counts->live;
        if (counts->check_retirement)
            for (std::size_t i = counts->retirement_start; i < counts->decoded.size(); ++i)
                counts->all_retired = counts->all_retired && !counts->decoded[i].valid();
    }
    void on_enable() noexcept { ++counts->enabled; }
};
struct Marker {};
struct Extra {};
inline ComponentCodecs codecs(const std::shared_ptr<int> &owner, Counts &counts, bool extra = false) {
    ComponentCodecs registry;
    registry.add<Link>(
        "test.a-link.v1",
        [](const Link &link, const ObjectReferences &references) { return references.key(link.target).string(); },
        [service = std::weak_ptr<int>(owner), &counts](GameObject object, std::string_view state,
                                                       const ObjectReferences &references) {
            const auto live = service.lock();
            if (!live)
                throw std::runtime_error("Expired composition fixture service");
            if (counts.destination)
                counts.all_native_ready =
                    counts.all_native_ready && counts.destination->size() == counts.expected_objects;
            object.add_component<Link>(references.resolve(ObjectKey::parse(state)), *live, counts);
            counts.decoded.push_back(object);
        });
    registry.add<Marker>(
        "test.z-marker.v1", [](const Marker &, const ObjectReferences &) { return "ok"; },
        [&counts](GameObject object, std::string_view, const ObjectReferences &) {
            object.add_component<Marker>();
            if (counts.fail_after_links && counts.decoded.size() == counts.fail_after_links)
                throw std::invalid_argument("Late composition fixture decoder failure");
        });
    if (extra)
        registry.add<Extra>(
            "test.extra.v1", [](const Extra &, const ObjectReferences &) { return ""; },
            [](GameObject object, std::string_view, const ObjectReferences &) { object.add_component<Extra>(); });
    return registry;
}
inline std::vector<Prefab::Node> nodes() {
    std::vector<Prefab::Node> result(4);
    const std::array keys{ObjectKey{41}, ObjectKey{99}, ObjectKey{7}, ObjectKey{500}};
    const std::array targets{ObjectKey{99}, ObjectKey{41}, ObjectKey{7}, ObjectKey{0}};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].key = keys[i];
        if (i)
            result[i].parent = 0;
        result[i].components.push_back({"test.a-link.v1", targets[i].string(), i != 2});
    }
    result[0].name = "root";
    result[0].local = translated(2);
    result[0].components.push_back({"test.z-marker.v1", "ok", true});
    result[1].name = "mount";
    result[1].local = translated(3);
    result[1].active = false;
    return result;
}
inline void check_graph(GameObject root, int binding) {
    const auto children = root.children();
    check(children.size() == 3 && root.get_component<Link>()->binding == binding &&
              root.get_component<Link>()->target.id() == children[0].id() &&
              children[0].get_component<Link>()->target.id() == root.id() &&
              children[1].get_component<Link>()->target.id() == children[1].id() &&
              !children[1].get_component<Link>().enabled() && !children[2].get_component<Link>()->target.valid(),
          "Composition lost per-part forward/cyclic/self/null references or destination bindings");
}
inline void linked_parts() {
    Counts source_counts, destination_counts, alternate_counts;
    auto source_owner = std::make_shared<int>(1);
    const auto destination_owner = std::make_shared<int>(2), alternate_owner = std::make_shared<int>(3);
    auto source = codecs(source_owner, source_counts, true),
         destination = codecs(destination_owner, destination_counts),
         alternate = codecs(alternate_owner, alternate_counts);
    auto authored = nodes();
    auto base = std::make_shared<const Prefab>(authored, source);
    const auto original_base = base->serialize({});
    source_owner.reset(); // Composition must borrow the explicit destination registry.
    const PrefabComposition composition(parts());
    const auto document = composition.serialize();
    const auto restored = PrefabComposition::deserialize(document);
    check(restored.parts().size() == 3 && restored.serialize() == document,
          "Composition lost named parts or document state");
    int resolutions = 0;
    const PrefabResolver resolve = [&](std::string_view key) {
        check(key == "shared", "Composition changed an application-owned prefab resource key");
        ++resolutions;
        return base;
    };
    Scene scene, other_scene;
    destination_counts.destination = &scene;
    destination_counts.expected_objects = 12;
    auto first = restored.instantiate(scene, resolve, destination, translated(13));
    auto middle = first.children()[0].children().front();
    auto leaf = middle.children()[0].children().front();
    check_graph(first, 2);
    check_graph(middle, 2);
    check_graph(leaf, 2);
    check(resolutions == 1 && destination_counts.all_native_ready && destination_counts.live == 12 &&
              destination_counts.enabled == 0 && source_counts.live == 0 && source_counts.decoded.empty() &&
              first.position().x == 20 && middle.position().x == 32 && leaf.position().x == 48 &&
              !middle.active_in_hierarchy() && !leaf.active_in_hierarchy() && first.key() != middle.key() &&
              middle.key() != leaf.key() && base->serialize({}) == original_base,
          "Composition failed shared resolution, complete native staging, placement, activation or source isolation");
    scene.synchronize_lifecycle();
    check(destination_counts.enabled == 2, "Composition enabled components beneath an inactive mount");

    authored[1].local = translated(9);
    base = std::make_shared<const Prefab>(authored, source);
    destination_counts.expected_objects = 24;
    auto second = composition.instantiate(scene, resolve, destination, translated(13));
    auto second_middle = second.children()[0].children().front();
    auto second_leaf = second_middle.children()[0].children().front();
    check_graph(second, 2);
    check_graph(second_middle, 2);
    check_graph(second_leaf, 2);
    check(resolutions == 2 && second_middle.position().x == 38 && second_leaf.position().x == 60 &&
              first.position().x == 20 && middle.position().x == 32 && leaf.position().x == 48 &&
              first.get_component<Link>()->target.id() != second.get_component<Link>()->target.id(),
          "A later composition reused a stale base snapshot or rebound an earlier instance");
    auto parent = other_scene.create();
    parent.set_position({17, 0, 0});
    parent.set_active(false);
    alternate_counts.destination = &other_scene;
    alternate_counts.expected_objects = 13;
    auto nested = composition.instantiate(parent, resolve, alternate, translated(13));
    check_graph(nested, 3);
    check(nested.position().x == 37 && !nested.active_in_hierarchy() && nested.parent()->id() == parent.id() &&
              alternate_counts.live == 12 && alternate_counts.all_native_ready && resolutions == 3,
          "Composition ignored its external parent, placement or borrowed destination registry");

    auto sentinel = scene.create();
    const auto existing = scene.size();
    const auto validation_failure = [&](const PrefabComposition &invalid, const PrefabResolver &resolver,
                                        const ComponentCodecs &registry) {
        const auto before_decodes = destination_counts.decoded.size();
        rejects([&] { (void)invalid.instantiate(scene, resolver, registry); });
        check(scene.size() == existing && sentinel.valid() && first.valid() && second.valid() &&
                  destination_counts.live == 24 && destination_counts.decoded.size() == before_decodes,
              "Composition validation created objects, ran decoders or changed destination state");
    };
    validation_failure(composition, {}, destination);
    validation_failure(composition, [](auto) -> std::shared_ptr<const Prefab> { return {}; }, destination);
    validation_failure(composition, resolve, {});
    auto invalid_mount = parts();
    invalid_mount[2].parent->object = {99999};
    validation_failure(PrefabComposition(invalid_mount), resolve, destination);
    auto unknown_nodes = authored;
    unknown_nodes.back().components.push_back({"test.extra.v1", "", true});
    const auto unknown_type = std::make_shared<const Prefab>(unknown_nodes, source);
    auto different_parts = parts();
    different_parts.back().prefab = "different";
    validation_failure(
        PrefabComposition(different_parts),
        [&](std::string_view key) { return key == "different" ? std::shared_ptr<const Prefab>{} : base; }, destination);
    validation_failure(
        PrefabComposition(different_parts),
        [&](std::string_view key) { return key == "different" ? unknown_type : base; }, destination);
    auto next = scene.create();
    check(next.key().value == sentinel.key().value + 1,
          "Composition validation consumed destination identities before rejecting");
    next.destroy();

    const auto failed_decode = [&](const PrefabComposition &invalid, const PrefabResolver &resolver) {
        const auto before_decodes = destination_counts.decoded.size();
        destination_counts.expected_objects = existing + 12;
        destination_counts.retirement_start = before_decodes;
        destination_counts.check_retirement = true;
        rejects([&] { (void)invalid.instantiate(scene, resolver, destination); });
        destination_counts.check_retirement = false;
        check(scene.size() == existing && sentinel.valid() && first.valid() && second.valid() &&
                  destination_counts.live == 24 && destination_counts.all_native_ready &&
                  destination_counts.all_retired && destination_counts.decoded.size() > before_decodes,
              "Composition decoder failure leaked resources or changed the published destination");
        for (std::size_t i = before_decodes; i < destination_counts.decoded.size(); ++i)
            check(!destination_counts.decoded[i].valid(), "Composition rollback left a staged part alive");
    };
    destination_counts.fail_after_links = destination_counts.decoded.size() + 9;
    failed_decode(composition, resolve);
    destination_counts.fail_after_links = 0;
    auto broken_nodes = authored;
    broken_nodes.back().components[0].state = "99999";
    const auto broken = std::make_shared<const Prefab>(broken_nodes, source);
    int shared_calls = 0, different_calls = 0;
    failed_decode(PrefabComposition(different_parts), [&](std::string_view key) {
        if (key == "different") {
            ++different_calls;
            return broken;
        }
        ++shared_calls;
        return base;
    });
    check(shared_calls == 1 && different_calls == 1 && composition.serialize() == document &&
              source_counts.decoded.empty(),
          "Failed composition lost resource caching, changed authored state or invoked source-bound codecs");
    auto stale = other_scene.create();
    stale.destroy();
    rejects([&] { (void)composition.instantiate(stale, resolve, alternate); });
}
inline void composed_bounds() {
    Asset asset;
    asset.nodes.resize(1);
    SourcePrimitive primitive;
    for (const auto position : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    Prefab::Node mount;
    mount.key = {41};
    mount.local[0] = std::numeric_limits<float>::max();
    auto anchor = std::make_shared<const Prefab>(std::vector{mount});
    Prefab::Node renderer;
    renderer.key = {41};
    renderer.mesh = Mesh::compile(asset);
    const auto visual = std::make_shared<const Prefab>(std::vector{renderer});
    const PrefabResolver resolve = [&](std::string_view key) { return key == "anchor" ? anchor : visual; };
    auto small = identity();
    small[0] = 1e-20F;
    std::vector<PrefabComposition::Part> graph{{"anchor", "anchor", std::nullopt, identity()},
                                               {"visual", "visual", PrefabComposition::Mount{"anchor", {41}}, small}};
    Scene scene;
    const auto first = PrefabComposition(graph).instantiate(scene, resolve, {});
    const auto child = first.children().front();
    check(child.renderer().bounds().valid && std::isfinite(child.renderer().bounds().maximum.x) &&
              child.local_matrix()[0] == small[0],
          "Composition applied a mounted mesh before its compensating part placement");

    mount.local = small;
    anchor = std::make_shared<const Prefab>(std::vector{mount});
    graph[1].placement[0] = std::numeric_limits<float>::max();
    const auto second = PrefabComposition(graph).instantiate(scene, resolve, {});
    const auto second_child = second.children().front();
    check(second_child.renderer().bounds().valid && std::isfinite(second_child.renderer().bounds().maximum.x) &&
              second_child.local_matrix()[0] == std::numeric_limits<float>::max(),
          "Composition rejected a finite mounted world because an isolated part scale was large");

    mount.local = identity();
    anchor = std::make_shared<const Prefab>(std::vector{mount});
    const auto before = scene.size();
    rejects([&] { (void)PrefabComposition(graph).instantiate(scene, resolve, {}); });
    check(scene.size() == before && first.valid() && second.valid() && child.valid() && second_child.valid(),
          "Composition accepted overflowing final bounds or failed to retire its partially staged native graph");

    // The same ordering applies when the mount is an existing external parent,
    // including the ordinary Prefab overload sharing the native staging helper.
    Scene external_scene;
    auto parent = external_scene.create();
    auto large = identity();
    large[0] = std::numeric_limits<float>::max();
    parent.set_local_matrix(large);
    const auto direct = visual->instantiate(parent, small);
    const PrefabComposition single({{"visual", "visual", std::nullopt, identity()}});
    const auto composed = single.instantiate(parent, resolve, {}, small);
    check(direct.renderer().bounds().valid && std::isfinite(direct.renderer().bounds().maximum.x) &&
              direct.renderer().bounds().maximum.x == composed.renderer().bounds().maximum.x &&
              direct.parent()->id() == parent.id() && composed.parent()->id() == parent.id(),
          "Prefab or composition staged renderer bounds before applying its external-parent placement");
    const auto external_before = external_scene.size();
    rejects([&] { (void)visual->instantiate(parent); });
    rejects([&] { (void)single.instantiate(parent, resolve, {}); });
    check(external_scene.size() == external_before && parent.valid() && direct.valid() && composed.valid() &&
              parent.children().size() == 2,
          "Uncompensated external-parent overflow leaked staged prefab objects or altered existing children");

    // Here the authored mesh pose supplies the compensation. Staging the rest
    // pose first would overflow despite the valid complete initial renderer.
    renderer.pose = renderer.mesh->rest_pose();
    renderer.pose->world[0][0] = small[0];
    const auto posed_visual = std::make_shared<const Prefab>(std::vector{renderer});
    const auto direct_pose = posed_visual->instantiate(parent);
    const auto composed_pose = single.instantiate(parent, [&](auto) { return posed_visual; }, {});
    const auto direct_bounds = direct_pose.renderer().bounds(),
               composed_pose_bounds = composed_pose.renderer().bounds();
    check(direct_bounds.valid && composed_pose_bounds.valid && std::isfinite(direct_bounds.maximum.x) &&
              direct_bounds.maximum.x == composed_pose_bounds.maximum.x && direct_pose.parent()->id() == parent.id() &&
              composed_pose.parent()->id() == parent.id() && parent.children().size() == 4,
          "Prefab or composition validated an intermediate rest pose instead of the complete authored renderer");
}
inline void run() {
    linked_parts();
    composed_bounds();
}
} // namespace prefab_composition_test
