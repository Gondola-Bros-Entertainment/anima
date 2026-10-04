#include "../detail/component_data.hpp"
#include "../detail/document_json.hpp"
#include "../detail/document_limits.hpp"
#include "../detail/json.hpp"
#include "../detail/prefab_instantiation.hpp"
#include "../detail/renderer_state.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_persistence.hpp"
#include "../detail/staging.hpp"
#include <algorithm>
#include <anima/prefab.hpp>
#include <map>
#include <set>

namespace anima {
namespace detail {
// A decoded scene document, or a member of a scene set document.
struct SceneStage {
    std::vector<Prefab::Node> nodes;
    std::uint64_t next_key = 1;
};
// A validated scene set document before any scene exists.
struct SceneSetStage {
    struct Member {
        std::string key;
        SceneStage content;
    };
    struct Reference {
        ObjectKey key, object;
        std::size_t scene;
    };
    std::vector<Member> scenes;
    std::optional<std::size_t> active;
    std::vector<Reference> references;
};
struct ScenePersistence {
    static GameObject create(Scene &scene, ObjectKey key, const std::string &name,
                             const std::shared_ptr<const Mesh> &mesh) {
        return scene.create_with_key(key, name, mesh);
    }
    static std::uint64_t next_key(const Scene &scene) { return scene.next_key_; }
    static void next_key(Scene &scene, std::uint64_t value) { scene.next_key_ = value; }
    static void assign_mesh(Scene &scene, Scene::Id object, const std::shared_ptr<const Mesh> &mesh,
                            const std::optional<Pose> &pose) {
        scene.assign_mesh(object, mesh, pose ? &*pose : nullptr);
    }
    static Prefab::Node capture(GameObject object) {
        const auto &source = object.scene().slot(object.id());
        Prefab::Node node;
        node.name = source.name;
        node.key = source.key;
        node.active = source.active_self;
        node.local = source.local;
        auto &renderer = node.renderer;
        renderer.mesh = source.value.asset;
        renderer.pose = source.pose;
        if (renderer.mesh) {
            renderer.visible = source.value.visible;
            renderer.material_factors = source.value.factors;
            const auto &custom = source.value.custom_materials;
            if (std::any_of(custom.begin(), custom.end(), [](const auto &material) { return bool(material); }))
                renderer.custom_materials = custom;
            renderer.primitive_visible = source.value.primitive_visible;
            renderer.casts_shadows = source.value.casts_shadows;
            renderer.placements = source.value.placements;
            renderer.visibility_range = source.value.visibility_range;
        }
        return node;
    }
    static StagedScene staged(SceneStage content) {
        return StagedScene(std::make_shared<const SceneStage>(std::move(content)));
    }
    static const SceneStage &stage(const StagedScene &document) { return *document.data_; }
    static StagedSceneSet staged(SceneSetStage content) {
        return StagedSceneSet(std::make_shared<const SceneSetStage>(std::move(content)));
    }
    static const SceneSetStage &stage(const StagedSceneSet &document) { return *document.data_; }
};
} // namespace detail
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 4;
constexpr std::string_view scene_kind = "anima.scene", prefab_kind = "anima.prefab";
constexpr unsigned scene_set_version = 2;
constexpr std::string_view scene_set_kind = "anima.scene-set";
using detail::maximum_document_bytes, detail::maximum_document_objects, detail::maximum_object_components,
    detail::maximum_scene_set_members;
using detail::require;
constexpr detail::NumberFormat number_format{
    .scalar = "Scene scalar must be a number",
    .finite = "Scene scalar must be finite",
    .matrix = "Scene matrix requires 16 scalars",
};
constexpr detail::RendererFormat renderer_format{
    .empty_state = "Empty scene object has renderer state",
    .placements = "Scene placements must copy the object's mesh, which has no pose",
    .mesh_naming = "Scene serialization needs a mesh naming callback",
    .written_mesh_key = "Invalid scene mesh key",
    .shared_mesh_key = "Different meshes share a scene resource key",
    .read_mesh_key = "Scene loading needs a mesh resolver and key",
    .mesh_resolver = "Scene loading needs a mesh resolver and key",
    .unresolved_mesh = "Scene mesh key could not be resolved",
    .pose = "Scene pose does not match the mesh",
    .material_factors = "Invalid scene material factors",
    .material_factor = "Scene material factor requires RGB",
    .custom_materials = "Invalid scene custom materials",
    .custom_material_name = "Invalid scene custom material name",
    .unresolved_custom_material = "Scene custom material name could not be resolved",
    .renamed_custom_material = "Scene custom material resolved to a material of another name",
    .visibility_range = "Invalid scene visibility range",
    .numbers = number_format,
};
void validate_nodes(std::span<const Prefab::Node> nodes, bool single_root) {
    require(nodes.size() <= maximum_document_objects && (!single_root || !nodes.empty()), "Invalid scene object count");
    std::set<ObjectKey> keys;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto &node = nodes[i];
        require(node.key.value && keys.insert(node.key).second, "Null or duplicate document object key");
        require(node.components.size() <= maximum_object_components, "Invalid serialized component count");
        detail::require_distinct_component_types(node.components);
        require(!node.parent || *node.parent < i, "Scene parent must precede its child");
        require(!single_root || i == 0 || node.parent.has_value(), "Prefab must have exactly one root");
        detail::validate_renderer_state(node.renderer, renderer_format);
    }
}
std::vector<GameObject> instantiate_nodes(Scene &scene, std::span<const Prefab::Node> nodes, const GameObject *parent,
                                          const Mat4 &placement, const ComponentCodecs *codecs,
                                          bool preserve_keys = false) {
    std::vector<GameObject> roots;
    roots.reserve(nodes.size());
    const auto objects = detail::instantiate_prefab_nodes(scene, nodes, parent, placement, preserve_keys);
    try {
        if (codecs)
            detail::restore_prefab_components(objects, nodes, *codecs);
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (!nodes[i].parent)
                roots.push_back(objects[i]);
    } catch (...) {
        detail::destroy_prefab_objects(objects);
        throw;
    }
    return roots;
}
struct Captured {
    std::vector<GameObject> objects;
    std::vector<Prefab::Node> nodes;
};
Captured capture_native_nodes(std::span<const GameObject> roots) {
    require(roots.size() <= maximum_document_objects, "Scene exceeds the object limit");
    std::vector<GameObject> objects(roots.begin(), roots.end());
    std::vector<Prefab::Node> nodes(roots.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto node = detail::ScenePersistence::capture(objects[i]);
        node.parent = nodes[i].parent;
        nodes[i] = std::move(node);
        const auto children = objects[i].children();
        require(children.size() <= maximum_document_objects - objects.size(), "Scene exceeds the object limit");
        for (auto child : children) {
            objects.push_back(child);
            Prefab::Node next;
            next.parent = i;
            nodes.push_back(std::move(next));
        }
    }
    return {std::move(objects), std::move(nodes)};
}
void capture_components(Captured &captured, const ComponentCodecs &codecs, const ObjectReferences &references) {
    for (std::size_t i = 0; i < captured.objects.size(); ++i)
        captured.nodes[i].components = codecs.capture(captured.objects[i], references);
}
std::vector<Prefab::Node> capture_nodes(std::span<const GameObject> roots, const ComponentCodecs &codecs) {
    auto captured = capture_native_nodes(roots);
    // Build the complete graph before any encoder, including links across roots.
    const ObjectReferences references(captured.objects);
    capture_components(captured, codecs, references);
    return std::move(captured.nodes);
}
Json encode_nodes(std::span<const Prefab::Node> nodes, const MeshName &name, detail::ResourceNames &resources) {
    auto objects = Json::array();
    for (const auto &node : nodes) {
        Json parent = nullptr;
        if (node.parent)
            parent = *node.parent;
        auto components = Json::array();
        for (const auto &component : node.components)
            components.push_back(
                {{"type", component.type}, {"state", component.state}, {"enabled", component.enabled}});
        auto object = detail::encode_renderer_state(node.renderer, name, resources, renderer_format);
        object["key"] = node.key.string();
        object["name"] = node.name;
        object["parent"] = parent;
        object["local"] = node.local;
        object["active"] = node.active;
        object["components"] = std::move(components);
        objects.push_back(std::move(object));
    }
    return objects;
}
std::string encode(std::span<const Prefab::Node> nodes, std::string_view kind, const MeshName &name,
                   std::uint64_t next_key = 1) {
    detail::ResourceNames resources;
    Json value{{"version", document_version}, {"kind", kind}, {"objects", encode_nodes(nodes, name, resources)}};
    if (kind == scene_kind)
        value["next_key"] = ObjectKey{next_key}.string();
    auto document = detail::json_step([&] { return value.dump(2); });
    require(document.size() <= maximum_document_bytes, "Scene document exceeds the byte limit");
    return document;
}
using detail::Resources;
using detail::SceneSetStage;
using detail::SceneStage;
using detail::StagingSteps;
// Mesh keys and custom material names that a staging call has counted as steps.
struct CountedNames {
    std::set<std::string_view> meshes, materials;
};
// Parses @p document as the parse step of a staging call.
Json parse_step(std::string_view document, const StagingSteps &steps) {
    steps.add(1);
    steps.check();
    auto parsed = detail::parse_json(document, maximum_document_bytes);
    steps.complete();
    return parsed;
}
// Adds a step for each value of @p objects and for each mesh key and custom material name among them that
// @p counted lacks. The document is not validated yet; content that decoding rejects fails the call, whose total
// then no longer matters.
void add_object_steps(const Json &objects, CountedNames &counted, const StagingSteps &steps) {
    if (!steps.counting() || !objects.is_array())
        return;
    std::uint64_t added = objects.size();
    for (const auto &value : objects) {
        if (!value.is_object())
            continue;
        if (const auto mesh = value.find("mesh"); mesh != value.end() && mesh->is_string() &&
                                                  counted.meshes.insert(mesh->get_ref<const std::string &>()).second)
            ++added;
        if (const auto custom = value.find("custom_materials"); custom != value.end() && custom->is_array())
            for (const auto &entry : *custom)
                if (entry.is_string() && counted.materials.insert(entry.get_ref<const std::string &>()).second)
                    ++added;
    }
    steps.add(added);
}
// The bytes that StagedScene::retained_bytes documents for @p nodes.
std::size_t retained_bytes(std::span<const Prefab::Node> nodes) {
    constexpr std::size_t flags_per_byte = 8;
    std::size_t bytes = 0;
    for (const auto &node : nodes) {
        const auto &renderer = node.renderer;
        bytes += sizeof(Prefab::Node) + node.name.size() + renderer.material_factors.size() * sizeof(Vec3) +
                 renderer.custom_materials.size() * sizeof(std::shared_ptr<const CustomMaterial>) +
                 (renderer.primitive_visible.size() + flags_per_byte - 1) / flags_per_byte;
        if (renderer.pose)
            bytes += renderer.pose->world.size() * sizeof(Mat4);
        for (const auto &component : node.components)
            bytes += sizeof(ComponentData) + component.type.size() + component.state.size();
        if (const auto &placements = renderer.placements)
            bytes += sizeof(MeshPlacements) + placements->transforms().size_bytes() +
                     placements->clusters().size_bytes() + placements->primitive_bounds().size_bytes();
    }
    return bytes;
}
std::vector<Prefab::Node> decode_nodes(const Json &objects, bool single_root, const MeshResolver &resolve,
                                       const CustomMaterialResolver &materials, Resources &resources,
                                       const StagingSteps &steps) {
    require(objects.is_array() && objects.size() <= maximum_document_objects, "Invalid scene object count");
    std::vector<Prefab::Node> nodes;
    nodes.reserve(objects.size());
    // Resource resolution is explicit and cached once per key. No scene is mutated here.
    for (const auto &value : objects) {
        steps.check();
        anima::detail::json_fields(value, {"key", "name", "parent", "local", "mesh", "pose", "visible", "active",
                                           "material_factors", "custom_materials", "primitive_visible", "casts_shadows",
                                           "placements", "visibility_range", "components"});
        Prefab::Node node;
        node.key = ObjectKey::parse(value.at("key").get<std::string>());
        node.active = value.at("active").get<bool>();
        node.name = value.at("name").get<std::string>();
        const auto &parent = value.at("parent");
        if (!parent.is_null()) {
            require(parent.is_number_unsigned() || (parent.is_number_integer() && parent.get<std::int64_t>() >= 0),
                    "Invalid scene parent index");
            const auto index = parent.get<std::uint64_t>();
            require(index < nodes.size(), "Scene parent must precede its child");
            node.parent = static_cast<std::size_t>(index);
        }
        node.local = detail::document_matrix(value.at("local"), number_format);
        node.renderer = detail::decode_renderer_state(value, resolve, materials, resources, steps, renderer_format);
        const auto &components = value.at("components");
        require(components.is_array() && components.size() <= maximum_object_components,
                "Invalid serialized component count");
        for (const auto &component : components) {
            anima::detail::json_fields(component, {"type", "state", "enabled"});
            node.components.push_back({component.at("type").get<std::string>(),
                                       component.at("state").get<std::string>(), component.at("enabled").get<bool>()});
        }
        nodes.push_back(std::move(node));
        steps.complete();
    }
    validate_nodes(nodes, single_root);
    return nodes;
}
std::uint64_t decode_next_key(const Json &value, std::span<const Prefab::Node> nodes) {
    const auto next_key = ObjectKey::parse(value.get<std::string>()).value;
    for (const auto &node : nodes)
        require(!next_key || next_key > node.key.value, "Invalid scene next object key");
    return next_key;
}
SceneStage decode_document(const Json &parsed, std::string_view kind, const MeshResolver &resolve,
                           const CustomMaterialResolver &materials, const StagingSteps &steps = {}) {
    require(parsed.is_object() && parsed.contains("version"), "Invalid scene document");
    anima::detail::json_version(parsed, "version", document_version, "Unsupported scene document version");
    require(parsed.contains("kind") && parsed.at("kind").is_string() &&
                parsed.at("kind").get_ref<const std::string &>() == kind,
            "Invalid scene document kind");
    if (kind == scene_kind)
        anima::detail::json_fields(parsed, {"version", "kind", "objects", "next_key"});
    else
        anima::detail::json_fields(parsed, {"version", "kind", "objects"});
    CountedNames counted;
    add_object_steps(parsed.at("objects"), counted, steps);
    Resources resources;
    auto nodes = decode_nodes(parsed.at("objects"), kind == prefab_kind, resolve, materials, resources, steps);
    std::uint64_t next_key = 1;
    if (kind == scene_kind)
        next_key = decode_next_key(parsed.at("next_key"), nodes);
    return {std::move(nodes), next_key};
}
std::shared_ptr<Scene> instantiate_scene(const SceneStage &decoded, const ComponentCodecs &codecs) {
    for (const auto &node : decoded.nodes)
        codecs.validate(node.components);
    auto scene = std::make_shared<Scene>();
    detail::ScenePersistence::next_key(*scene, decoded.next_key);
    (void)instantiate_nodes(*scene, decoded.nodes, nullptr, identity(), &codecs, true);
    return scene;
}
SceneSetStage decode_scene_set(const Json &parsed, const MeshResolver &resolve, const CustomMaterialResolver &materials,
                               const StagingSteps &steps = {}) {
    detail::json_version(parsed, "version", scene_set_version, "Unsupported scene set document version");
    require(parsed.contains("kind") && parsed.at("kind").is_string() &&
                parsed.at("kind").get_ref<const std::string &>() == scene_set_kind,
            "Invalid scene set document kind");
    detail::json_fields(parsed, {"version", "kind", "active", "scenes", "references"});
    const auto &documents = parsed.at("scenes"), &table = parsed.at("references");
    require(documents.is_array() && documents.size() <= maximum_scene_set_members, "Invalid scene set scene count");
    require(table.is_array() && table.size() <= maximum_document_objects, "Invalid scene set reference count");
    CountedNames counted;
    for (const auto &value : documents)
        if (value.is_object())
            if (const auto objects = value.find("objects"); objects != value.end())
                add_object_steps(*objects, counted, steps);

    SceneSetStage result;
    result.scenes.reserve(documents.size());
    std::map<std::string, std::size_t, std::less<>> namespaces;
    std::vector<std::set<ObjectKey>> unmapped;
    unmapped.reserve(documents.size());
    Resources resources;
    std::size_t object_count = 0;
    for (const auto &value : documents) {
        detail::json_fields(value, {"key", "next_key", "objects"});
        auto key = value.at("key").get<std::string>();
        require(!key.empty() && key.size() <= SceneSet::max_namespace_bytes && key.find('\0') == std::string::npos,
                "Invalid scene namespace");
        require(namespaces.emplace(key, result.scenes.size()).second, "Duplicate scene namespace");
        const auto &objects = value.at("objects");
        require(objects.is_array() && objects.size() <= maximum_document_objects - object_count,
                "Scene set exceeds the object limit");
        auto nodes = decode_nodes(objects, false, resolve, materials, resources, steps);
        object_count += nodes.size();
        std::set<ObjectKey> object_keys;
        for (const auto &node : nodes)
            object_keys.insert(node.key);
        const auto next_key = decode_next_key(value.at("next_key"), nodes);
        unmapped.push_back(std::move(object_keys));
        result.scenes.push_back({std::move(key), {std::move(nodes), next_key}});
    }
    const auto &active = parsed.at("active");
    if (result.scenes.empty())
        require(active.is_null(), "Empty scene set has an active scene");
    else {
        require(active.is_string(), "Scene set requires an active namespace");
        const auto found = namespaces.find(active.get_ref<const std::string &>());
        require(found != namespaces.end(), "Active scene namespace is missing");
        result.active = found->second;
    }

    result.references.reserve(table.size());
    std::set<ObjectKey> reference_keys;
    require(table.size() == object_count, "Scene set reference table must cover every object");
    for (const auto &value : table) {
        detail::json_fields(value, {"key", "scene", "object"});
        const auto key = ObjectKey::parse(value.at("key").get<std::string>());
        const auto object = ObjectKey::parse(value.at("object").get<std::string>());
        require(key.value && reference_keys.insert(key).second, "Null or duplicate scene set reference key");
        const auto found = namespaces.find(value.at("scene").get<std::string>());
        require(found != namespaces.end(), "Scene set reference namespace is missing");
        require(unmapped[found->second].erase(object) == 1, "Missing or duplicate scene set reference target");
        result.references.push_back({key, object, found->second});
    }
    return result;
}
// Checks every component type of @p staged against @p codecs before anything is created.
void validate_components(const SceneSetStage &staged, const ComponentCodecs &codecs) {
    for (const auto &scene : staged.scenes)
        for (const auto &node : scene.content.nodes)
            codecs.validate(node.components);
}
} // namespace

namespace detail {
void destroy_prefab_objects(std::span<const GameObject> objects) {
    // Includes an object whose parenting/transform failed before it was linked.
    for (auto it = objects.rbegin(); it != objects.rend(); ++it)
        if (it->valid()) {
            auto object = *it;
            object.destroy();
        }
}
std::vector<GameObject> instantiate_prefab_nodes(Scene &scene, std::span<const Prefab::Node> nodes,
                                                 const GameObject *parent, const Mat4 &placement, bool preserve_keys) {
    std::vector<GameObject> objects;
    objects.reserve(nodes.size());
    try {
        for (const auto &node : nodes) {
            // Establish the actual hierarchy/world transform before validating
            // mesh bounds. An intermediate identity/local-only placement can
            // overflow even when the final parent-composed transform is valid.
            auto object =
                preserve_keys ? ScenePersistence::create(scene, node.key, node.name, {}) : scene.create(node.name);
            objects.push_back(object);
            object.set_active(node.active);
            if (node.parent)
                object.set_parent(objects.at(*node.parent), ReparentMode::keep_local);
            else if (parent)
                object.set_parent(*parent, ReparentMode::keep_local);
            object.set_local_matrix(node.parent ? node.local : placement * node.local);
            const auto &state = node.renderer;
            if (!state.mesh)
                continue;
            ScenePersistence::assign_mesh(scene, object.id(), state.mesh, state.pose);
            auto renderer = object.renderer();
            renderer.set_visible(state.visible);
            const auto &instance = scene.instance(object.id());
            require(state.material_factors.empty() || state.material_factors.size() == instance.factors.size(),
                    "Prefab material factors do not match the mesh");
            require(state.custom_materials.empty() || state.custom_materials.size() == instance.custom_materials.size(),
                    "Prefab custom materials do not match the mesh");
            require(state.primitive_visible.empty() ||
                        state.primitive_visible.size() == instance.primitive_visible.size(),
                    "Prefab primitive visibility does not match the mesh");
            for (std::size_t i = 0; i < state.material_factors.size(); ++i)
                renderer.set_material_factor(i, state.material_factors[i]);
            for (std::size_t i = 0; i < state.custom_materials.size(); ++i)
                renderer.set_custom_material(i, state.custom_materials[i]);
            for (std::size_t i = 0; i < state.primitive_visible.size(); ++i)
                renderer.set_primitive_visible(i, state.primitive_visible[i]);
            renderer.set_casts_shadows(state.casts_shadows);
            if (state.placements)
                renderer.set_placements(state.placements);
            renderer.set_visibility_range(state.visibility_range);
        }
    } catch (...) {
        destroy_prefab_objects(objects);
        throw;
    }
    return objects;
}
void restore_prefab_components(std::span<const GameObject> objects, std::span<const Prefab::Node> nodes,
                               const ComponentCodecs &codecs) {
    std::vector<ObjectReferences::Entry> entries;
    entries.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
        entries.push_back({nodes[i].key, objects[i]});
    const ObjectReferences references(entries);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        codecs.restore(objects[i], nodes[i].components, references);
}
} // namespace detail

Prefab::Prefab(std::vector<Node> nodes, ComponentCodecs codecs) : nodes_(std::move(nodes)), codecs_(std::move(codecs)) {
    require(nodes_.size() <= maximum_document_objects && !nodes_.empty(), "Invalid prefab object count");
    // Programmatic nodes may omit keys. Preserve explicit keys and assign unused
    // identities deterministically; document readers require all v3 keys explicitly.
    std::set<ObjectKey> used;
    for (const auto &node : nodes_)
        if (node.key.value)
            require(used.insert(node.key).second, "Duplicate prefab object key");
    std::uint64_t next = 1;
    for (auto &node : nodes_)
        if (!node.key.value) {
            while (used.contains(ObjectKey{next}))
                ++next;
            node.key = ObjectKey{next++};
            used.insert(node.key);
        }
    validate_nodes(nodes_, true);
    for (const auto &node : nodes_)
        codecs_.validate(node.components);
    Scene validation;
    (void)instantiate_nodes(validation, nodes_, nullptr, identity(), nullptr);
}
Prefab Prefab::capture(GameObject root, const ComponentCodecs &codecs) {
    const std::array roots{root};
    return Prefab(capture_nodes(roots, codecs), codecs);
}
GameObject Prefab::create(Scene &scene, const GameObject *parent, const Mat4 &placement,
                          const ComponentCodecs &codecs) const {
    for (const auto &node : nodes_)
        codecs.validate(node.components);
    return instantiate_nodes(scene, nodes_, parent, placement, &codecs).front();
}
GameObject Prefab::instantiate(Scene &scene, const Mat4 &placement) const {
    return create(scene, nullptr, placement, codecs_);
}
GameObject Prefab::instantiate(GameObject parent, const Mat4 &placement) const {
    return create(parent.scene(), &parent, placement, codecs_);
}
GameObject Prefab::instantiate(Scene &scene, const Mat4 &placement, const ComponentCodecs &codecs) const {
    return create(scene, nullptr, placement, codecs);
}
GameObject Prefab::instantiate(GameObject parent, const Mat4 &placement, const ComponentCodecs &codecs) const {
    return create(parent.scene(), &parent, placement, codecs);
}
std::string Prefab::serialize(const MeshName &name) const { return encode(nodes_, prefab_kind, name); }
Prefab Prefab::deserialize(std::string_view document, const MeshResolver &resolve, ComponentCodecs codecs,
                           const CustomMaterialResolver &materials) {
    return Prefab(detail::json_step([&] {
                      return decode_document(detail::parse_json(document, maximum_document_bytes), prefab_kind, resolve,
                                             materials);
                  }).nodes,
                  std::move(codecs));
}
std::string serialize_scene(Scene &scene, const MeshName &name, const ComponentCodecs &codecs) {
    const auto nodes = capture_nodes(scene.roots(), codecs);
    validate_nodes(nodes, false);
    return encode(nodes, scene_kind, name, detail::ScenePersistence::next_key(scene));
}
std::size_t StagedScene::retained_bytes() const noexcept { return anima::retained_bytes(data_->nodes); }
std::size_t StagedSceneSet::retained_bytes() const noexcept {
    // A reference row keeps its key, its object's key and its member's index.
    constexpr std::size_t reference_bytes = 2 * sizeof(ObjectKey) + sizeof(std::size_t);
    auto bytes = data_->references.size() * reference_bytes;
    for (const auto &member : data_->scenes)
        bytes += member.key.size() + anima::retained_bytes(member.content.nodes);
    return bytes;
}
StagedScene stage_scene(std::string_view document, const MeshResolver &resolve, const CustomMaterialResolver &materials,
                        const StagingOptions &options) {
    const StagingSteps steps(options);
    auto stage = detail::json_step(
        [&] { return decode_document(parse_step(document, steps), scene_kind, resolve, materials, steps); });
    steps.check();
    return detail::ScenePersistence::staged(std::move(stage));
}
StagedSceneSet stage_scene_set(std::string_view document, const MeshResolver &resolve,
                               const CustomMaterialResolver &materials, const StagingOptions &options) {
    const StagingSteps steps(options);
    auto stage =
        detail::json_step([&] { return decode_scene_set(parse_step(document, steps), resolve, materials, steps); });
    steps.check();
    return detail::ScenePersistence::staged(std::move(stage));
}
std::shared_ptr<Scene> load_scene(const StagedScene &staged, const ComponentCodecs &codecs) {
    return instantiate_scene(detail::ScenePersistence::stage(staged), codecs);
}
std::shared_ptr<Scene> load_scene(std::string_view document, const MeshResolver &resolve, const ComponentCodecs &codecs,
                                  const CustomMaterialResolver &materials) {
    return load_scene(stage_scene(document, resolve, materials), codecs);
}

namespace detail {
std::string serialize_scene_set(SceneSet &scenes, const MeshName &name, const ComponentCodecs &codecs) {
    const auto selected = scenes.scenes();
    require(selected.size() <= maximum_scene_set_members, "Scene set exceeds the scene limit");
    std::vector<Captured> captured;
    std::vector<std::uint64_t> next_keys;
    captured.reserve(selected.size());
    next_keys.reserve(selected.size());
    std::size_t object_count = 0;
    for (auto scene : selected) {
        require(scene->size() <= maximum_document_objects - object_count, "Scene set exceeds the object limit");
        auto nodes = capture_native_nodes(scene->roots());
        object_count += nodes.objects.size();
        captured.push_back(std::move(nodes));
        next_keys.push_back(ScenePersistence::next_key(scene.get()));
    }

    std::vector<ObjectReferences::Entry> entries;
    entries.reserve(object_count);
    auto table = Json::array();
    std::uint64_t next_reference = 1;
    for (std::size_t i = 0; i < selected.size(); ++i)
        for (auto object : captured[i].objects) {
            const ObjectKey key{next_reference++};
            entries.push_back({key, object});
            table.push_back({{"key", key.string()}, {"scene", selected[i].key()}, {"object", object.key().string()}});
        }
    const ObjectReferences references(entries);
    auto documents = Json::array();
    detail::ResourceNames resources;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        capture_components(captured[i], codecs, references);
        validate_nodes(captured[i].nodes, false);
        documents.push_back({{"key", selected[i].key()},
                             {"next_key", ObjectKey{next_keys[i]}.string()},
                             {"objects", encode_nodes(captured[i].nodes, name, resources)}});
    }
    Json active = nullptr;
    if (const auto current = scenes.active())
        active = current.key();
    const Json value{{"version", scene_set_version},
                     {"kind", scene_set_kind},
                     {"active", std::move(active)},
                     {"scenes", std::move(documents)},
                     {"references", std::move(table)}};
    auto document = detail::json_step([&] { return value.dump(2); });
    require(document.size() <= maximum_document_bytes, "Scene set document exceeds the byte limit");
    return document;
}

std::unique_ptr<SceneSet> load_scene_set(const StagedSceneSet &staged, const ComponentCodecs &codecs) {
    const auto &decoded = ScenePersistence::stage(staged);
    validate_components(decoded, codecs);
    // A complete owner provides cross-scene rollback: all staged identities die
    // before any decoded component is released, including cyclic object links.
    auto built = std::make_unique<SceneSet>();
    std::vector<SceneRef> selected;
    selected.reserve(decoded.scenes.size());
    for (const auto &scene : decoded.scenes) {
        auto created = built->create(scene.key);
        ScenePersistence::next_key(created.get(), scene.content.next_key);
        (void)instantiate_nodes(created.get(), scene.content.nodes, nullptr, identity(), nullptr, true);
        selected.push_back(created);
    }
    if (decoded.active)
        built->set_active(selected[*decoded.active]);
    std::vector<ObjectReferences::Entry> entries;
    entries.reserve(decoded.references.size());
    for (const auto &reference : decoded.references)
        entries.push_back({reference.key, selected[reference.scene]->find(reference.object)});
    const ObjectReferences references(entries);
    const SceneDriver::Scope scope(*built);
    for (std::size_t i = 0; i < decoded.scenes.size(); ++i)
        for (const auto &node : decoded.scenes[i].content.nodes)
            codecs.restore(selected[i]->find(node.key), node.components, references);
    return built;
}

namespace {
// Loads member @p key of @p decoded as the StagedSceneSet overload describes.
std::shared_ptr<Scene> load_scene_member(const SceneSetStage &decoded, const SceneSet &scenes, std::string_view key,
                                         const ComponentCodecs &codecs) {
    validate_components(decoded, codecs);
    const auto member = std::find_if(decoded.scenes.begin(), decoded.scenes.end(),
                                     [&](const SceneSetStage::Member &candidate) { return candidate.key == key; });
    require(member != decoded.scenes.end(), "Replaced scene namespace is missing");
    const auto index = static_cast<std::size_t>(member - decoded.scenes.begin());
    const auto &nodes = member->content.nodes;
    // On failure the scene is released, which invalidates every staged handle before any decoded
    // component is cleaned up.
    auto scene = std::make_shared<Scene>();
    ScenePersistence::next_key(*scene, member->content.next_key);
    const auto objects = instantiate_prefab_nodes(*scene, nodes, nullptr, identity(), true);
    // Rows in the replaced namespace name the new objects. Other rows resolve by address, as a
    // restore resolves them, but against the members that stay; an address that finds no object is
    // left unmapped, so only a link to it fails.
    std::vector<ObjectReferences::Entry> entries;
    entries.reserve(decoded.references.size());
    for (const auto &reference : decoded.references) {
        const auto object = reference.scene == index
                                ? scene->find(reference.object)
                                : scenes.find(SceneAddress{decoded.scenes[reference.scene].key, reference.object});
        if (object.valid())
            entries.push_back({reference.key, object});
    }
    const ObjectReferences references(entries);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        codecs.restore(objects[i], nodes[i].components, references);
    return scene;
}
} // namespace

std::shared_ptr<Scene> load_scene_member(std::string_view document, const SceneSet &scenes, std::string_view key,
                                         const MeshResolver &resolve, const CustomMaterialResolver &materials,
                                         const ComponentCodecs &codecs) {
    const auto parsed = json_step([&] { return parse_json(document, maximum_document_bytes); });
    const bool set_document = parsed.is_object() && parsed.contains("kind") && parsed.at("kind").is_string() &&
                              parsed.at("kind").get_ref<const std::string &>() == scene_set_kind;
    if (!set_document)
        return instantiate_scene(json_step([&] { return decode_document(parsed, scene_kind, resolve, materials); }),
                                 codecs);
    return load_scene_member(json_step([&] { return decode_scene_set(parsed, resolve, materials); }), scenes, key,
                             codecs);
}
std::shared_ptr<Scene> load_scene_member(const StagedSceneSet &staged, const SceneSet &scenes, std::string_view key,
                                         const ComponentCodecs &codecs) {
    return load_scene_member(ScenePersistence::stage(staged), scenes, key, codecs);
}
} // namespace detail
} // namespace anima
