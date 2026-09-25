#pragma once
#include "components.hpp"
#include <anima/animation.hpp>
#include <anima/prefab.hpp>
#include <anima/terrain.hpp>
#include <limits>
#include <stdexcept>

namespace scene_objects_test {
inline void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid object operation was accepted");
}
inline std::shared_ptr<const anima::Asset> source() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    asset->nodes[0].name = "Root";
    asset->materials.push_back({"Surface", {1, 1, 1}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto p : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex vertex;
        vertex.position = p;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset->primitives.push_back(primitive);
    anima::Animation clip;
    clip.name = "Move";
    clip.duration = 1;
    clip.channels.push_back(
        {0, anima::ChannelPath::translation, anima::Interpolation::linear, {0, 1}, {{0, 0, 0, 0}, {0, 2, 0, 0}}});
    asset->animations.push_back(clip);
    return asset;
}
inline void hierarchy(const std::shared_ptr<const anima::Asset> &asset,
                      const std::shared_ptr<const anima::Mesh> &mesh) {
    using namespace anima;
    Scene scene, foreign;
    auto root = scene.create("Root");
    root.transform().set({.translation = {4, 1, 0}, .scale = {2, 3, 1}});
    auto child = scene.create("Animated", mesh);
    child.set_position({8, 4, 0});
    child.set_parent(root);
    require(child.position().x == 8 && child.local_position().x == 2 && child.local_position().y == 1 &&
                child.parent()->id() == root.id() && root.children().front().id() == child.id() &&
                scene.roots().size() == 1,
            "Parenting did not preserve world placement or expose hierarchy membership");
    Animator animator(child, asset);
    animator.play("Move");
    (void)animator.update(.5);
    auto tip = scene.create("Tip", mesh);
    tip.transform().set_local_position({0, 2, 0});
    tip.set_parent(child, ReparentMode::keep_local);
    const auto local = child.local_matrix();
    root.transform().set_position({10, 1, 0});
    require(child.local_matrix() == local && child.position().x == 14 && tip.position().x == 14 &&
                tip.position().y == 6 && scene.instance(child.id()).palette[0][13] == 5,
            "Moving a parent lost child local placement, animation or descendant propagation");
    require(child.renderer().bounds().minimum.x > 13.9F && tip.renderer().bounds().minimum.x > 13.9F,
            "Hierarchy movement left render bounds behind");
    child.transform().set_position({12, 7, 0});
    require(length(child.local_position() - Vec3{1, 2, 0}) < 1e-5F && tip.position().y == 9,
            "World-space child setter did not update local transform and descendants");
    const auto accepted = child.world_matrix();
    child.set_parent(root);
    require(root.children().size() == 1 && child.world_matrix() == accepted, "Repeated parenting duplicated a child");
    rejects([&] { root.set_parent(tip); });
    rejects([&] { child.set_parent(child); });
    rejects([&] { child.set_parent(foreign.create()); });
    rejects([&] { child.set_parent({}); });
    require(!root.parent() && child.world_matrix() == accepted && root.children().size() == 1,
            "Rejected parent changed accepted hierarchy");
    child.clear_parent();
    require(!child.parent() && child.local_matrix() == accepted && tip.position().y == 9 && root.children().empty(),
            "Unparenting did not preserve the subtree's world placement");
    child.set_parent(root, ReparentMode::keep_local);
    require(child.position().x == 34 && tip.position().x == 34, "Keep-local reparenting did not move the subtree");
    child.clear_parent(ReparentMode::keep_local);
    require(child.position().x == 12 && tip.position().x == 12, "Keep-local detachment changed local placement");
    // Exact matrices support shear and mirrored/nonuniform transforms without TRS decomposition.
    auto shear = identity();
    shear[4] = .5F;
    shear[0] = -2;
    root.set_world_matrix(shear);
    child.set_parent(root);
    require(length(child.position() - Vec3{12, 7, 0}) < 1e-5F, "Sheared reparenting changed world placement");
    const auto composed = root.world_matrix() * child.local_matrix();
    for (std::size_t i = 0; i < 16; ++i)
        require(std::abs(composed[i] - child.world_matrix()[i]) < 1e-5F,
                "Local/world matrices disagree after reparenting");

    auto collapsed = scene.create("Collapsed");
    collapsed.transform().set({.scale = {0, 1, 1}});
    rejects([&] { child.set_parent(collapsed); });
    child.set_parent(collapsed, ReparentMode::keep_local);
    (void)animator.update(.1); // Pose-only changes need no parent inverse.
    rejects([&] { child.set_position({1, 0, 0}); });
    child.transform().set_local_position({3, 2, 0});
    require(child.local_position().x == 3 && child.position().x == 0,
            "Singular parents prevented explicit local-space updates");
    child.clear_parent();

    auto group = scene.create("Atomic group");
    auto large = scene.create("Large child", mesh);
    large.transform().set_local({.scale = {2, 1, 1}});
    large.set_parent(group, ReparentMode::keep_local);
    const auto before = scene.instance(large.id()).palette;
    auto overflow = identity();
    overflow[0] = std::numeric_limits<float>::max();
    rejects([&] { group.set_world_matrix(overflow); });
    require(group.world_matrix() == identity() && large.local_matrix()[0] == 2 &&
                scene.instance(large.id()).palette == before,
            "Descendant overflow partially published a hierarchy transform");
    auto huge_parent = scene.create("Huge parent");
    huge_parent.set_world_matrix(overflow);
    rejects([&] { group.set_parent(huge_parent, ReparentMode::keep_local); });
    require(!group.parent() && huge_parent.children().empty() && group.world_matrix() == identity(),
            "Failed reparenting changed parent links or root placement");
    group.destroy();
    require(!large.valid() && child.valid(), "Destroying a subtree removed peers or retained descendants");
    const auto saved_tip = tip;
    child.destroy();
    require(!saved_tip.valid(), "Destroying a parent retained a child handle");
    const auto recycled = scene.create();
    require(recycled.children().empty() && !recycled.parent() && recycled.local_matrix() == identity(),
            "Reused slot retained hierarchy state");

    Scene deep;
    auto ancestor = deep.create("Deep root");
    auto leaf = ancestor;
    for (unsigned i = 0; i < 2048; ++i) {
        auto next = deep.create();
        next.set_parent(leaf, ReparentMode::keep_local);
        leaf = next;
    }
    ancestor.set_position({7, 0, 0});
    require(leaf.position().x == 7, "Deep hierarchy transform did not reach the leaf");
    ancestor.destroy();
    require(deep.size() == 0 && !leaf.valid(), "Deep hierarchy cleanup did not remove every descendant");
}
inline void templates(const std::shared_ptr<const anima::Asset> &asset,
                      const std::shared_ptr<const anima::Mesh> &mesh) {
    using namespace anima;
    Scene source;
    auto root = source.create("Assembly");
    root.set_position({3, 0, 0});
    auto child = source.create("Part", mesh);
    child.set_parent(root, ReparentMode::keep_local);
    child.set_local_position({2, 0, 0});
    Animator animator(child, asset);
    animator.play("Move");
    (void)animator.update(.5);
    child.renderer().set_material_factor(0, {.2F, .3F, .4F});
    auto marker = source.create("Socket");
    marker.set_parent(child, ReparentMode::keep_local);
    marker.set_local_position({0, 4, 0});
    const auto prefab = Prefab::capture(root);
    Scene target;
    auto first = prefab.instantiate(target);
    auto placement = identity();
    placement[12] = 10;
    auto second = prefab.instantiate(target, placement);
    auto part = first.children().front();
    auto independent = second.children().front();
    require(target.size() == 6 && target.instances().size() == 2 && part.renderer().mesh() == mesh &&
                independent.renderer().mesh() == mesh && part.position().x == 5 && independent.position().x == 15 &&
                target.instance(part.id()).palette[0][13] == 1 && target.instance(part.id()).factors[0].x == .2F &&
                part.children().front().position().y == 4,
            "Prefab instantiation lost hierarchy, pose, overrides or shared resources");
    first.set_position({-5, 0, 0});
    part.renderer().set_material_factor(0, {1, 0, 0});
    part.renderer().set_primitive_visible(0, false);
    require(independent.position().x == 15 && target.instance(independent.id()).factors[0].x == .2F &&
                target.instance(independent.id()).primitive_visible[0],
            "Prefab instances share mutable object state");
    const auto nested = Prefab::capture(child);
    auto nested_copy = nested.instantiate(second);
    require(nested_copy.local_position().x == 2 && nested_copy.position().x == 15,
            "Nested prefab did not use its authored local transform");
    nested_copy.destroy();
    first.set_active(false);
    second.set_active(false);
    require(!target.bounds().valid && !target.instance(independent.id()).active &&
                target.instance(independent.id()).visible,
            "Inactive hierarchy retained bounds or changed authored visibility");
    for (const auto &draw : target.snapshot().primitives)
        require(!draw.visible, "Inactive object remained visible in diagnostic snapshot");
    independent.renderer().set_mesh(mesh);
    require(!target.instance(independent.id()).active, "Mesh replacement reset inherited activation");
    second.set_active(true);
    require(target.bounds().valid && target.instance(independent.id()).active, "Reactivation did not restore bounds");
    first.set_active(true);
    auto empty = target.create("Empty root");
    empty.set_position({0, 0, 7});
    unsigned named = 0, resolved = 0;
    const MeshName name = [&](const auto &resource) {
        require(resource == mesh, "Unexpected mesh in serialized scene");
        ++named;
        return "mesh:triangle";
    };
    const MeshResolver resolve = [&](std::string_view key) {
        require(key == "mesh:triangle", "Unexpected scene mesh key");
        ++resolved;
        return mesh;
    };
    const auto document = serialize_scene(target, name);
    auto loaded = load_scene(document, resolve);
    require(named == 1 && resolved == 1 && loaded->size() == target.size() && loaded->roots().size() == 3 &&
                loaded->instances().size() == 2,
            "Scene roundtrip lost empty objects or duplicated resource resolution");
    require(serialize_scene(*loaded, name) == document, "Scene document did not roundtrip accepted object state");
    const auto saved = prefab.serialize(name);
    const auto restored = Prefab::deserialize(saved, resolve);
    const auto restored_root = restored.instantiate(*loaded);
    require(restored_root.children().front().children().front().name() == "Socket", "Serialized prefab lost nesting");
    rejects([&] { (void)load_scene(saved, resolve); });
    rejects([&] { (void)Prefab::deserialize(document, resolve); });
    rejects([&] { (void)load_scene(document, {}); });
    rejects([&] { (void)load_scene(document, [](auto) { return std::shared_ptr<const Mesh>{}; }); });
    auto invalid_version = document;
    const auto version = invalid_version.find("\"version\": 3");
    require(version != std::string::npos, "Missing serialized scene version");
    invalid_version.replace(version, 12, "\"version\": null");
    rejects([&] { (void)load_scene(invalid_version, resolve); });
    auto duplicate_version = document;
    duplicate_version.replace(version, 12, "\"version\": 3, \"version\": 3");
    rejects([&] { (void)load_scene(duplicate_version, resolve); });
    auto cyclic = saved;
    const auto parent = cyclic.find("\"parent\": null");
    cyclic.replace(parent, 14, "\"parent\": 0");
    rejects([&] { (void)Prefab::deserialize(cyclic, resolve); });
    rejects([&] { (void)load_scene(std::string(20, '[') + std::string(20, ']'), resolve); });
    Scene no_objects;
    require(load_scene(serialize_scene(no_objects, {}), {})->size() == 0, "Empty scene failed to roundtrip");
    auto another_mesh = target.create("Different resource", Mesh::compile(*asset));
    rejects([&] { (void)serialize_scene(target, [](const auto &) { return "duplicate-key"; }); });
    another_mesh.destroy();

    rejects([&] { Prefab invalid({}); });
    Prefab::Node invalid_node;
    invalid_node.parent = 0;
    rejects([&] { Prefab invalid({invalid_node}); });
    invalid_node.parent.reset();
    invalid_node.local[3] = 1;
    rejects([&] { Prefab invalid({invalid_node}); });
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes[1].local[0] = 2;
    const Prefab large(nodes);
    auto huge = target.create("Overflow parent");
    auto huge_matrix = identity();
    huge_matrix[0] = std::numeric_limits<float>::max();
    huge.set_world_matrix(huge_matrix);
    const auto count = target.size();
    const auto palette = target.instance(part.id()).palette;
    rejects([&] { (void)large.instantiate(huge); });
    require(target.size() == count && huge.children().empty() && target.instance(part.id()).palette == palette,
            "Failed prefab instantiation leaked objects or changed the existing scene");
    first.destroy();
    require(!part.valid() && independent.valid(), "Prefab destruction affected another instance");

    auto health = root.add_component<components_test::Health>(42);
    health.set_enabled(false);
    rejects([&] { (void)Prefab::capture(root); });
    rejects([&] { (void)serialize_scene(source, name); });
    ComponentCodecs codecs;
    const auto encode_health = [](const components_test::Health &value, const ObjectReferences &) {
        return std::to_string(value.value);
    };
    const auto decode_health = [](GameObject object, std::string_view value, const ObjectReferences &) {
        (void)object.add_component<components_test::Health>(std::stoi(std::string(value)));
    };
    codecs.add<components_test::Health>("consumer.health.v1", encode_health, decode_health);
    rejects([&] { codecs.add<components_test::Health>("duplicate", encode_health, decode_health); });
    const auto composed = Prefab::capture(root, codecs);
    const auto encoded = composed.serialize(name);
    const auto restored_components = Prefab::deserialize(encoded, resolve, codecs);
    auto custom = restored_components.instantiate(target);
    auto custom_health = custom.get_component<components_test::Health>();
    require(custom_health && custom_health->value == 42 && !custom_health.enabled(),
            "Prefab did not restore user component state and enabled policy");
    custom_health->value = 7;
    require(health->value == 42, "Prefab instances share user component state");
    const auto saved_components = serialize_scene(source, name, codecs);
    auto component_scene = load_scene(saved_components, resolve, codecs);
    require(component_scene->components<components_test::Health>().front()->value == 42,
            "Scene roundtrip dropped a user-defined component");
    rejects([&] { (void)load_scene(saved_components, resolve); });
    ComponentCodecs failing_codecs;
    unsigned decodes = 0;
    failing_codecs.add<components_test::Health>(
        "consumer.health.v1", encode_health,
        [&](GameObject object, std::string_view value, const ObjectReferences &references) {
            ++decodes;
            decode_health(object, value, references);
            throw std::runtime_error("Component decoder failure");
        });
    const auto failing = Prefab::deserialize(encoded, resolve, failing_codecs);
    require(decodes == 0, "Prefab validation ran application component constructors");
    const auto before_failure = target.size();
    rejects([&] { (void)failing.instantiate(target); });
    require(decodes == 1 && target.size() == before_failure && custom_health->value == 7,
            "Failed component restoration leaked a prefab or changed another instance");
}
inline void run() {
    using namespace anima;
    const auto asset = source();
    const auto mesh = Mesh::compile(*asset);
    hierarchy(asset, mesh);
    components_test::run(asset, mesh);
    templates(asset, mesh);
    Scene scene, foreign;
    auto empty = scene.create("Marker");
    require(empty.valid() && !empty.has_renderer() && scene.size() == 1 && scene.instances().empty(),
            "Empty objects must own a transform without creating draws");
    require(empty.world_matrix() == identity(), "Empty object's transform is not identity");
    empty.transform().set({.translation = {4, 0, 0}, .scale = {2, 2, 2}});
    auto renderer = empty.add_mesh(mesh);
    require(renderer.mesh() == mesh && scene.instance(empty.id()).palette[0][12] == 4,
            "Attaching a mesh lost the empty object's transform");
    auto other = scene.create("Marker", mesh);
    renderer.set_material_factor(0, {.2F, .7F, .9F});
    require(scene.instance(other.id()).factors[0].x == 1 && mesh->materials()->material_data[0].factor.x == 1,
            "Per-object material override mutated another object or resource");
    rejects([&] { (void)foreign.object(empty.id()); });
    rejects([&] { (void)empty.add_mesh(mesh); });
    auto broken = identity();
    broken[12] = std::numeric_limits<float>::infinity();
    const auto accepted = scene.instance(empty.id()).palette;
    rejects([&] { empty.set_world_matrix(broken); });
    require(scene.instance(empty.id()).palette == accepted && empty.position().x == 4,
            "Rejected transform changed accepted object state");
    Animator animator(empty, asset), independent(other, asset);
    animator.play("Move", false);
    independent.play("Move");
    (void)animator.update(.5);
    require(scene.instance(empty.id()).palette[0][13] == 2 && scene.instance(other.id()).palette[0][13] == 0,
            "Independent animation/scale was not preserved");
    empty.set_position({8, 0, 0});
    require(scene.instance(empty.id()).palette[0][13] == 2 && scene.instance(empty.id()).palette[0][12] == 8,
            "Moving an animated object reset its pose");
    rejects([&] { animator.play("Missing"); });
    require(animator.playback().time() == .5, "Rejected clip changed playback");
    auto wrong_rig = std::make_shared<Asset>(*asset);
    wrong_rig->nodes[0].name = "Other rig";
    rejects([&] { Animator invalid_animator(empty, wrong_rig); });
    const auto copy = empty;
    empty.remove_mesh();
    require(empty.valid() && !empty.has_renderer() && empty.position().x == 8, "Removing mesh removed the object");
    rejects([&] { renderer.set_visible(false); });
    rejects([&] { (void)animator.update(.1); });
    empty.destroy();
    auto reused = scene.create("Reused", mesh);
    require(!copy.valid() && reused.id().slot == copy.id().slot && reused.id().generation != copy.id().generation,
            "A recycled slot revived a stale object handle");
    rejects([&] { (void)copy.position(); });
    GameObject expired;
    {
        Scene temporary;
        expired = temporary.create("Temporary");
    }
    require(!expired.valid(), "Handle kept a destroyed scene alive");
    rejects([&] { expired.set_position({}); });

    const auto data = std::make_shared<TerrainData>(Heightfield{2, 2, 0, 0, 1, 1, {0, 0, 0, 1}});
    Terrain terrain(data);
    auto ground = scene.create("Ground", terrain.mesh());
    const auto sample = terrain.data().sample(.75F, .25F);
    require(sample && std::abs(sample->height - .25F) < 1e-6F && ground.has_renderer(),
            "Terrain render/query diagonal disagrees");
    const auto &indices = terrain.mesh()->indices();
    const auto &vertices = terrain.mesh()->vertices();
    const auto a = vertices[indices[0]].position, b = vertices[indices[1]].position, c = vertices[indices[2]].position;
    require(cross(b - a, c - a).y > 0 && indices.size() == 6, "Terrain topology/winding changed");
    TerrainAppearance invalid;
    const std::array bad_normals{Vec3{0, 1, 0}};
    invalid.normals = bad_normals;
    rejects([&] { (void)Terrain::compile(data->view(), invalid); });
    const auto heights = data->view().heights;
    rejects([&] { (void)Terrain::compile(TerrainGrid{2, 2, 1e30, 0, 1, 1, heights}); });
    const auto left = Terrain::compile(TerrainGrid{2, 2, -44, -68, 1. / 3., 1. / 3., heights, 1000, 0});
    const auto right = Terrain::compile(TerrainGrid{2, 2, -44, -68, 1. / 3., 1. / 3., heights, 1001, 0});
    require(left->vertices()[left->indices()[2]].position.x == right->vertices()[right->indices()[0]].position.x,
            "Neighbouring terrain cells disagree on their shared sample position");

    Asset static_asset = *asset;
    static_asset.animations.clear();
    static_asset.primitives.push_back(static_asset.primitives.front());
    const auto pieces = Mesh::compile_static(static_asset, {3, 0});
    require(pieces.size() == 2 && pieces[0]->indices().size() == 3 && pieces[1]->indices().size() == 3,
            "Static mesh preparation ignored its caller's geometry limit");
    rejects([&] { (void)Mesh::compile_static(static_asset, {2, 0}); });
    rejects([&] { (void)Mesh::compile_static(*asset, {3, 0}); });
    Asset textured = static_asset;
    textured.textures.resize(2);
    for (auto &texture : textured.textures) {
        texture.width = texture.height = 4;
        texture.rgba.resize(4 * 4 * 4, 255);
        texture.sampler.u = Wrap::mirror;
    }
    textured.materials[0].texture = 1;
    textured.materials[0].normal_texture = 0;
    textured.textures[0].encoding = TextureEncoding::linear;
    const auto reduced = Mesh::compile_static(textured, {3, 2});
    for (const auto &part : reduced) {
        const auto &materials = *part->materials();
        const auto &material = materials.material_data[0];
        require(materials.textures.at(material.texture).width == 2 &&
                    materials.textures.at(material.texture).sampler.u == Wrap::mirror &&
                    materials.textures.at(material.normal_texture).encoding == TextureEncoding::linear,
                "Static resource preparation lost material, sampler or encoding identity");
    }
    require(textured.textures[0].width == 4 && textured.materials[0].texture == 1,
            "Static preparation mutated its source");
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        Scene part;
        auto object = part.create("Part", pieces[i]);
        const auto snapshot = part.snapshot();
        for (std::size_t j = 0; j < snapshot.vertices.size(); ++j)
            require(length(snapshot.vertices[j].position - static_asset.primitives[i].vertices[j].position) < 1e-6F,
                    "Static preparation changed geometry");
        object.destroy();
    }
    ground.destroy();
    other.destroy();
    reused.destroy();
    require(scene.size() == 0 && scene.instances().empty(), "Object removal left live scene membership");
}
} // namespace scene_objects_test
