#pragma once
#include <algorithm>
#include <anima/prefab_variant.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prefab_variant_test {
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
    throw std::runtime_error("Invalid prefab variant operation was accepted");
}
inline std::shared_ptr<const Mesh> mesh(float extent = 1) {
    Asset asset;
    asset.nodes.resize(1);
    asset.materials.resize(1);
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto position : {Vec3{0, 0, 0}, Vec3{extent, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return Mesh::compile(asset);
}
inline const Prefab::Node &node(const Prefab &prefab, ObjectKey key) {
    const auto found =
        std::find_if(prefab.nodes().begin(), prefab.nodes().end(), [&](const auto &value) { return value.key == key; });
    check(found != prefab.nodes().end(), "Resolved prefab lost an authored key");
    return *found;
}
inline void native_and_resources() {
    const auto original_mesh = mesh(), replacement_mesh = mesh();
    std::vector<Prefab::Node> nodes(3);
    nodes[0].key = {41};
    nodes[0].name = "base root";
    nodes[0].local[12] = 2;
    nodes[0].active = false;
    nodes[1].key = {99};
    nodes[1].name = "base child";
    nodes[1].parent = 0;
    nodes[1].local[12] = 3;
    nodes[2].key = {700};
    nodes[2].parent = 0;
    for (auto &value : nodes)
        value.mesh = original_mesh;
    auto base = std::make_shared<const Prefab>(nodes);
    const auto before = base->serialize([](const auto &) { return "base-mesh"; });
    PrefabVariant::Renderer renderer;
    renderer.mesh = replacement_mesh;
    renderer.pose = replacement_mesh->rest_pose();
    renderer.pose->world[0][12] = 4;
    renderer.visible = false;
    renderer.material_factors = {{0.2F, 0.3F, 0.4F}};
    renderer.primitive_visible = {false};
    PrefabVariant::Override root;
    root.key = {41};
    root.name = "variant root";
    root.local = identity();
    (*root.local)[12] = 5;
    root.active = true;
    root.renderer = renderer;
    PrefabVariant::Override child;
    child.key = {99};
    child.renderer.emplace(); // A complete empty renderer explicitly clears it.
    PrefabVariant::Override other;
    other.key = {700};
    other.renderer = renderer;
    const PrefabVariant variant("props/base", {root, child, other});
    check(variant.base_key() == "props/base" && variant.overrides().size() == 3,
          "Variant lost its explicit base identity or overrides");
    int names = 0, meshes = 0, bases = 0;
    const MeshName name = [&](const auto &resource) {
        check(resource == replacement_mesh, "Variant serialized an inherited mesh");
        ++names;
        return "replacement";
    };
    const auto document = variant.serialize(name);
    check(names == 1, "Variant named one shared mesh more than once");
    const auto restored = PrefabVariant::deserialize(document, [&](std::string_view key) {
        check(key == "replacement", "Variant changed a replacement mesh key");
        ++meshes;
        return replacement_mesh;
    });
    check(meshes == 1 && restored.serialize(name) == document,
          "Variant resource sharing or document round-trip failed");
    const PrefabResolver resolve = [&](std::string_view key) {
        check(key == "props/base", "Variant changed its base resource key");
        ++bases;
        return base;
    };
    const auto result = restored.resolve(resolve, {});
    check(bases == 1 && base->serialize([](const auto &) { return "base-mesh"; }) == before &&
              result.nodes().size() == nodes.size() && node(result, {41}).name == "variant root" &&
              node(result, {41}).local[12] == 5 && node(result, {41}).active &&
              node(result, {41}).mesh == replacement_mesh && node(result, {41}).pose &&
              node(result, {41}).pose->world[0][12] == 4 && !node(result, {99}).mesh &&
              node(result, {99}).name == "base child" && node(result, {99}).parent == nodes[1].parent &&
              node(result, {700}).parent == nodes[2].parent,
          "Variant changed its base or lost typed overrides, authored keys or topology");
    Scene scene;
    auto placement = identity();
    placement[12] = 11;
    auto instance = result.instantiate(scene, placement);
    const auto children = instance.children();
    const auto &rendered = scene.instance(instance.id());
    check(instance.position().x == 16 && children[0].position().x == 19 && instance.active_self() &&
              instance.renderer().mesh() == replacement_mesh && !children[0].has_component<MeshRenderer>() &&
              !rendered.visible && !rendered.primitive_visible[0] && rendered.factors[0].x == 0.2F,
          "Resolved variant did not instantiate its native state or placement");
    rejects([&] { (void)children[0].renderer(); });

    nodes[0].name = "edited base root";
    nodes[0].local[12] = 12;
    nodes[1].name = "edited base child";
    nodes[1].local[12] = 9;
    base = std::make_shared<const Prefab>(nodes);
    const auto rebased = restored.resolve(resolve, {});
    check(node(rebased, {41}).name == "variant root" && node(rebased, {41}).local[12] == 5 &&
              node(rebased, {99}).name == "edited base child" && node(rebased, {99}).local[12] == 9,
          "Variant froze inherited fields or discarded explicit overrides after a base edit");
    const PrefabVariant inherited("props/base", {});
    check(inherited.resolve(resolve, {}).serialize([](const auto &) { return "base-mesh"; }) ==
              base->serialize([](const auto &) { return "base-mesh"; }),
          "Empty variant failed to inherit its entire base");
    nodes[1].key = {100};
    base = std::make_shared<const Prefab>(nodes);
    rejects([&] { (void)restored.resolve(resolve, {}); });
    rejects([&] { (void)restored.resolve({}, {}); });
    rejects([&] { (void)restored.resolve([](auto) -> std::shared_ptr<const Prefab> { return {}; }, {}); });
    rejects([&] { (void)PrefabVariant::deserialize(document, {}); });
    rejects(
        [&] { (void)PrefabVariant::deserialize(document, [](auto) -> std::shared_ptr<const Mesh> { return {}; }); });
    rejects([&] { (void)variant.serialize([](const auto &) { return ""; }); });
    other.renderer->mesh = mesh();
    const PrefabVariant collision("props/base", {root, other});
    rejects([&] { (void)collision.serialize([](const auto &) { return "same-key"; }); });
}
struct Counts {
    int live{}, enabled{};
    std::vector<GameObject> decoded;
};
struct Link {
    GameObject target;
    int binding;
    Counts *counts;
    Link(GameObject object, int session, Counts &values) : target(object), binding(session), counts(&values) {
        ++counts->live;
    }
    ~Link() { --counts->live; }
    void on_enable() noexcept { ++counts->enabled; }
};
struct Marker {
    std::string value;
};
inline ComponentCodecs codecs(int binding, Counts &counts) {
    ComponentCodecs registry;
    registry.add<Link>(
        "test.a-link.v1",
        [](const Link &link, const ObjectReferences &references) { return references.key(link.target).string(); },
        [binding, &counts](GameObject object, std::string_view state, const ObjectReferences &references) {
            object.add_component<Link>(references.resolve(ObjectKey::parse(state)), binding, counts);
            counts.decoded.push_back(object);
        });
    registry.add<Marker>(
        "test.z-marker.v1", [](const Marker &marker, const ObjectReferences &) { return marker.value; },
        [](GameObject object, std::string_view state, const ObjectReferences &) {
            object.add_component<Marker>(std::string(state));
            if (state == "fail")
                throw std::invalid_argument("Variant fixture rejected a late component payload");
        });
    return registry;
}
inline void links_and_bindings() {
    Counts source_counts, destination_counts, alternate_counts;
    auto source = codecs(1, source_counts), destination = codecs(2, destination_counts),
         alternate = codecs(3, alternate_counts);
    std::vector<Prefab::Node> nodes(4);
    const std::array keys{ObjectKey{41}, ObjectKey{99}, ObjectKey{7}, ObjectKey{500}};
    const std::array targets{ObjectKey{99}, ObjectKey{41}, ObjectKey{7}, ObjectKey{0}};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = keys[i];
        if (i)
            nodes[i].parent = 0;
        nodes[i].components.push_back({"test.a-link.v1", targets[i].string(), i != 2});
    }
    nodes[0].components.push_back({"test.z-marker.v1", "base", true});
    nodes[1].components.push_back({"test.z-marker.v1", "removed", true});
    const auto base = std::make_shared<const Prefab>(nodes, source);
    const auto before = base->serialize({});
    PrefabVariant::Override root, child, last;
    root.key = keys[0];
    root.set_components = {{"test.z-marker.v1", "variant", false}};
    child.key = keys[1];
    child.remove_components = {"test.z-marker.v1"};
    last.key = keys[3];
    last.set_components = {{"test.z-marker.v1", "added", true}};
    const PrefabVariant authored("linked/base", {root, child, last});
    const auto variant = PrefabVariant::deserialize(authored.serialize({}), {});
    const PrefabResolver resolve = [&](auto) { return base; };
    const auto result = variant.resolve(resolve, destination);
    check(source_counts.live == 0 && destination_counts.live == 0 && destination_counts.decoded.empty() &&
              base->serialize({}) == before,
          "Variant resolution executed codecs, retained source bindings or changed the base");
    rejects([&] { (void)variant.resolve(resolve, {}); });
    Scene scene, other_scene;
    auto first = result.instantiate(scene), second = result.instantiate(scene);
    const auto first_children = first.children(), second_children = second.children();
    check(first.key() != second.key() && first.get_component<Link>()->binding == 2 &&
              first.get_component<Link>()->target.id() == first_children[0].id() &&
              first_children[0].get_component<Link>()->target.id() == first.id() &&
              first_children[1].get_component<Link>()->target.id() == first_children[1].id() &&
              !first_children[1].get_component<Link>().enabled() &&
              !first_children[2].get_component<Link>()->target.valid() &&
              second.get_component<Link>()->target.id() == second_children[0].id() &&
              second_children[0].get_component<Link>()->target.id() == second.id() &&
              first.get_component<Marker>()->value == "variant" && !first.get_component<Marker>().enabled() &&
              !first_children[0].has_component<Marker>() &&
              first_children[2].get_component<Marker>()->value == "added" && destination_counts.live == 8 &&
              destination_counts.enabled == 0 && source_counts.live == 0,
          "Variant lost component deltas, destination bindings or per-instance reference remapping");
    auto parent = other_scene.create();
    parent.set_active(false);
    auto nested = result.instantiate(parent, identity(), alternate);
    check(nested.get_component<Link>()->binding == 3 && !nested.active_in_hierarchy() &&
              nested.get_component<Link>()->target.id() == nested.children()[0].id() && alternate_counts.live == 4,
          "Variant prefab failed its alternate destination or parent instantiation overload");
    scene.synchronize_lifecycle();
    check(destination_counts.enabled == 6, "Variant changed inherited component enablement");
    auto sentinel = scene.create();
    const auto count_before = scene.size();
    const auto failed_instance = [&](const PrefabVariant &bad) {
        const auto decoded_before = destination_counts.decoded.size();
        const auto prefab = bad.resolve(resolve, destination);
        rejects([&] { (void)prefab.instantiate(scene); });
        check(scene.size() == count_before && sentinel.valid() && first.valid() && second.valid() &&
                  destination_counts.live == 8,
              "Failed variant decoder leaked objects/resources or changed existing instances");
        for (std::size_t i = decoded_before; i < destination_counts.decoded.size(); ++i)
            check(!destination_counts.decoded[i].valid(), "Variant rollback retained a decoded object handle");
        check(destination_counts.decoded.size() > decoded_before,
              "Variant rollback fixture did not decode any resource");
    };
    auto bad_link = last;
    bad_link.set_components.push_back({"test.a-link.v1", "99999", true});
    failed_instance(PrefabVariant("linked/base", {root, child, bad_link}));
    auto late_failure = child;
    late_failure.remove_components.clear();
    late_failure.set_components = {{"test.z-marker.v1", "fail", true}};
    failed_instance(PrefabVariant("linked/base", {root, late_failure, last}));
    auto unknown_type = root;
    unknown_type.set_components = {{"test.unknown.v1", "", true}};
    rejects([&] { (void)PrefabVariant("linked/base", {unknown_type}).resolve(resolve, destination); });
    auto unknown_removed = child;
    unknown_removed.remove_components = {"test.absent.v1"};
    rejects([&] { (void)PrefabVariant("linked/base", {unknown_removed}).resolve(resolve, destination); });
    auto unknown_target = root;
    unknown_target.key = {9999};
    rejects([&] { (void)PrefabVariant("linked/base", {unknown_target}).resolve(resolve, destination); });
    check(base->serialize({}) == before && authored.serialize({}) == variant.serialize({}),
          "Resolving or instantiating variants mutated authored state");
}
struct First {};
struct Second {};
struct Added {};
inline void component_order() {
    std::vector<std::string> order;
    ComponentCodecs registry;
    registry.add<First>(
        "z-first.v1", [](const First &, const ObjectReferences &) { return ""; },
        [&](GameObject object, std::string_view state, const ObjectReferences &) {
            order.emplace_back(state);
            object.add_component<First>();
        });
    registry.add<Second>(
        "a-second.v1", [](const Second &, const ObjectReferences &) { return ""; },
        [&](GameObject object, std::string_view state, const ObjectReferences &) {
            order.emplace_back(state);
            object.add_component<Second>();
        });
    registry.add<Added>(
        "m-added.v1", [](const Added &, const ObjectReferences &) { return ""; },
        [&](GameObject object, std::string_view state, const ObjectReferences &) {
            order.emplace_back(state);
            object.add_component<Added>();
        });
    Prefab::Node root;
    root.key = {41};
    root.components = {{"z-first.v1", "original", true}, {"a-second.v1", "inherited", true}};
    const auto base = std::make_shared<const Prefab>(std::vector{root}, registry);
    PrefabVariant::Override change;
    change.key = root.key;
    change.set_components = {{"m-added.v1", "appended", true}, {"z-first.v1", "replaced", true}};
    const auto resolved = PrefabVariant("base", {change}).resolve([&](auto) { return base; }, registry);
    check(order.empty(), "Resolving a variant ran component decoders");
    Scene scene;
    (void)resolved.instantiate(scene);
    check(order == std::vector<std::string>{"replaced", "inherited", "appended"},
          "Variant reordered inherited component decoders or moved a replacement from its base position");
}
inline void composed_transforms() {
    std::vector<Prefab::Node> nodes(2);
    nodes[0].key = {41};
    nodes[0].local[0] = 1e-20F;
    nodes[1].key = {99};
    nodes[1].parent = 0;
    const auto base = std::make_shared<const Prefab>(nodes);
    PrefabVariant::Override child;
    child.key = {99};
    child.local = identity();
    (*child.local)[0] = std::numeric_limits<float>::max();
    child.renderer.emplace();
    child.renderer->mesh = mesh();

    // The tiny inherited parent makes the large authored child scale finite in
    // world space. A root-level renderer validation would reject its bounds.
    nodes[1].local = *child.local;
    nodes[1].mesh = child.renderer->mesh;
    const Prefab expected(nodes);
    Scene direct_scene, variant_scene;
    const auto direct = expected.instantiate(direct_scene);
    const auto expected_bounds = direct.children()[0].renderer().bounds();
    const auto resolved = PrefabVariant("base", {child}).resolve([&](auto) { return base; }, {});
    const auto instance = resolved.instantiate(variant_scene);
    const auto actual = instance.children()[0];
    const auto bounds = actual.renderer().bounds();
    check(bounds.valid && std::isfinite(bounds.maximum.x) && bounds.maximum.x == expected_bounds.maximum.x &&
              actual.local_matrix()[0] == std::numeric_limits<float>::max(),
          "Variant validated a child transform outside its inherited hierarchy or changed its authored scale");

    // Authored mesh poses use the same composed parent world as local transforms.
    child.local.reset();
    child.renderer->pose = child.renderer->mesh->rest_pose();
    child.renderer->pose->world[0][0] = std::numeric_limits<float>::max();
    nodes[1].local = identity();
    nodes[1].pose = child.renderer->pose;
    const Prefab expected_pose(nodes);
    const auto direct_pose = expected_pose.instantiate(direct_scene);
    const auto expected_pose_bounds = direct_pose.children()[0].renderer().bounds();
    const auto posed = PrefabVariant("base", {child}).resolve([&](auto) { return base; }, {});
    const auto posed_instance = posed.instantiate(variant_scene);
    const auto posed_bounds = posed_instance.children()[0].renderer().bounds();
    check(posed_bounds.valid && std::isfinite(posed_bounds.maximum.x) &&
              posed_bounds.maximum.x == expected_pose_bounds.maximum.x && node(posed, {99}).pose &&
              node(posed, {99}).pose->world[0][0] == std::numeric_limits<float>::max(),
          "Variant validated a mesh pose outside its inherited hierarchy or changed its authored scale");
    auto unscaled_nodes = std::vector<Prefab::Node>(base->nodes().begin(), base->nodes().end());
    unscaled_nodes[0].local = identity();
    const auto unscaled = std::make_shared<const Prefab>(unscaled_nodes);
    const auto unscaled_before = unscaled->serialize({});
    const PrefabVariant overflowing_pose("base", {child});
    rejects([&] { (void)overflowing_pose.resolve([&](auto) { return unscaled; }, {}); });
    check(unscaled->serialize({}) == unscaled_before, "Failed world-space pose validation changed the resolved base");
}
inline void deferred_renderer_bounds() {
    const auto wide = mesh(std::numeric_limits<float>::max());
    Prefab::Node root;
    root.key = {41};
    const auto base = std::make_shared<const Prefab>(std::vector{root});
    PrefabVariant::Override change;
    change.key = root.key;
    change.renderer.emplace();
    change.renderer->mesh = wide;
    change.renderer->pose = wide->rest_pose();
    change.renderer->pose->world[0][0] = 0.1F;
    const PrefabVariant variant("base", {change});
    const auto resolved = variant.resolve([&](auto) { return base; }, {});
    Scene scene;
    const auto instance = resolved.instantiate(scene);
    const auto bounds = instance.renderer().bounds();
    check(bounds.valid && std::isfinite(bounds.maximum.x) && bounds.maximum.x > 0 && instance.renderer().mesh() == wide,
          "Variant rejected a finite authored pose because the mesh rest-pose bounds overflowed");

    change.renderer->pose->world[0] = identity();
    const PrefabVariant overflowing("base", {change});
    rejects([&] { (void)overflowing.resolve([&](auto) { return base; }, {}); });
    check(instance.valid() && scene.size() == 1 && !base->nodes()[0].mesh,
          "Deferred variant bounds validation accepted overflow or changed existing authored/runtime state");
}
inline void run() {
    native_and_resources();
    links_and_bindings();
    component_order();
    composed_transforms();
    deferred_renderer_bounds();
}
} // namespace prefab_variant_test
