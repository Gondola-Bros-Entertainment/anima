#include "../detail/json.hpp"
#include "../detail/prefab_instantiation.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_persistence.hpp"
#include <anima/prefab.hpp>
#include <map>
#include <set>

namespace anima {
namespace detail {
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
        node.mesh = source.value.asset;
        node.pose = source.pose;
        if (node.mesh) {
            node.visible = source.value.visible;
            node.material_factors = source.value.factors;
            node.primitive_visible = source.value.primitive_visible;
        }
        return node;
    }
};
} // namespace detail
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 3;
constexpr std::string_view scene_kind = "anima.scene", prefab_kind = "anima.prefab";
constexpr std::size_t maximum_objects = 65'536, maximum_document_bytes = 16 * 1024 * 1024;
constexpr std::size_t maximum_components = 1024, maximum_mesh_key_bytes = 4096;
constexpr unsigned scene_set_version = 1;
constexpr std::string_view scene_set_kind = "anima.scene-set";
constexpr std::size_t maximum_scenes = 1024, maximum_namespace_bytes = 4096;
void require(bool accepted, const char *reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}
float scalar(const Json &value) {
    require(value.is_number(), "Scene scalar must be a number");
    const auto result = value.get<float>();
    require(std::isfinite(result), "Scene scalar must be finite");
    return result;
}
Mat4 matrix_value(const Json &value) {
    require(value.is_array() && value.size() == 16, "Scene matrix requires 16 scalars");
    Mat4 result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = scalar(value[i]);
    return result;
}
void validate_nodes(std::span<const Prefab::Node> nodes, bool single_root) {
    require(nodes.size() <= maximum_objects && (!single_root || !nodes.empty()), "Invalid scene object count");
    std::set<ObjectKey> keys;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto &node = nodes[i];
        require(node.key.value && keys.insert(node.key).second, "Null or duplicate document object key");
        require(node.components.size() <= maximum_components, "Invalid serialized component count");
        require(!node.parent || *node.parent < i, "Scene parent must precede its child");
        require(!single_root || i == 0 || node.parent.has_value(), "Prefab must have exactly one root");
        require(node.mesh ||
                    (!node.pose && node.material_factors.empty() && node.primitive_visible.empty() && node.visible),
                "Empty scene object has renderer state");
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
    require(roots.size() <= maximum_objects, "Scene exceeds the object limit");
    std::vector<GameObject> objects(roots.begin(), roots.end());
    std::vector<Prefab::Node> nodes(roots.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto node = detail::ScenePersistence::capture(objects[i]);
        node.parent = nodes[i].parent;
        nodes[i] = std::move(node);
        const auto children = objects[i].children();
        require(children.size() <= maximum_objects - objects.size(), "Scene exceeds the object limit");
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
struct MeshNames {
    std::map<const Mesh *, std::string> names;
    std::map<std::string, const Mesh *, std::less<>> identities;
};
Json encode_nodes(std::span<const Prefab::Node> nodes, const MeshName &name, MeshNames &resources) {
    auto objects = Json::array();
    for (const auto &node : nodes) {
        Json mesh = nullptr, parent = nullptr, pose = nullptr;
        if (node.parent)
            parent = *node.parent;
        if (node.mesh) {
            if (!resources.names.contains(node.mesh.get())) {
                require(bool(name), "Scene serialization needs a mesh naming callback");
                auto key = name(node.mesh);
                require(!key.empty() && key.size() <= maximum_mesh_key_bytes, "Invalid scene mesh key");
                const auto [existing, inserted] = resources.identities.emplace(key, node.mesh.get());
                require(inserted || existing->second == node.mesh.get(), "Different meshes share a scene resource key");
                resources.names.emplace(node.mesh.get(), std::move(key));
            }
            mesh = resources.names.at(node.mesh.get());
        }
        if (node.pose)
            pose = node.pose->world;
        auto factors = Json::array();
        for (auto factor : node.material_factors)
            factors.push_back({factor.x, factor.y, factor.z});
        auto components = Json::array();
        for (const auto &component : node.components)
            components.push_back(
                {{"type", component.type}, {"state", component.state}, {"enabled", component.enabled}});
        objects.push_back({{"key", node.key.string()},
                           {"name", node.name},
                           {"parent", parent},
                           {"local", node.local},
                           {"mesh", mesh},
                           {"pose", pose},
                           {"visible", node.visible},
                           {"active", node.active},
                           {"material_factors", factors},
                           {"primitive_visible", node.primitive_visible},
                           {"components", components}});
    }
    return objects;
}
std::string encode(std::span<const Prefab::Node> nodes, std::string_view kind, const MeshName &name,
                   std::uint64_t next_key = 1) {
    MeshNames resources;
    Json value{{"version", document_version}, {"kind", kind}, {"objects", encode_nodes(nodes, name, resources)}};
    if (kind == scene_kind)
        value["next_key"] = ObjectKey{next_key}.string();
    auto document = value.dump(2);
    require(document.size() <= maximum_document_bytes, "Scene document exceeds the byte limit");
    return document;
}
struct Decoded {
    std::vector<Prefab::Node> nodes;
    std::uint64_t next_key = 1;
};
using MeshResources = std::map<std::string, std::shared_ptr<const Mesh>, std::less<>>;
std::vector<Prefab::Node> decode_nodes(const Json &objects, bool single_root, const MeshResolver &resolve,
                                       MeshResources &resources) {
    require(objects.is_array() && objects.size() <= maximum_objects, "Invalid scene object count");
    std::vector<Prefab::Node> nodes;
    nodes.reserve(objects.size());
    // Resource resolution is explicit and cached once per key. No scene is mutated here.
    for (const auto &value : objects) {
        anima::detail::json_fields(value, {"key", "name", "parent", "local", "mesh", "pose", "visible", "active",
                                           "material_factors", "primitive_visible", "components"});
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
        node.local = matrix_value(value.at("local"));
        const auto &mesh = value.at("mesh");
        if (!mesh.is_null()) {
            const auto key = mesh.get<std::string>();
            require(!key.empty() && key.size() <= maximum_mesh_key_bytes && bool(resolve),
                    "Scene loading needs a mesh resolver and key");
            auto found = resources.find(key);
            if (found == resources.end()) {
                auto resource = resolve(key);
                require(bool(resource), "Scene mesh key could not be resolved");
                found = resources.emplace(key, std::move(resource)).first;
            }
            node.mesh = found->second;
        }
        const auto &pose = value.at("pose");
        if (!pose.is_null()) {
            require(pose.is_array() && node.mesh && pose.size() == node.mesh->rest_pose().world.size(),
                    "Scene pose does not match the mesh");
            node.pose.emplace();
            for (const auto &world : pose)
                node.pose->world.push_back(matrix_value(world));
        }
        node.visible = value.at("visible").get<bool>();
        const auto &factors = value.at("material_factors");
        require(factors.is_array(), "Invalid scene material factors");
        for (const auto &factor : factors) {
            require(factor.is_array() && factor.size() == 3, "Scene material factor requires RGB");
            node.material_factors.push_back({scalar(factor[0]), scalar(factor[1]), scalar(factor[2])});
        }
        node.primitive_visible = value.at("primitive_visible").get<std::vector<bool>>();
        const auto &components = value.at("components");
        require(components.is_array() && components.size() <= maximum_components, "Invalid serialized component count");
        for (const auto &component : components) {
            anima::detail::json_fields(component, {"type", "state", "enabled"});
            node.components.push_back({component.at("type").get<std::string>(),
                                       component.at("state").get<std::string>(), component.at("enabled").get<bool>()});
        }
        nodes.push_back(std::move(node));
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
Decoded decode(std::string_view document, std::string_view kind, const MeshResolver &resolve) {
    const auto parsed = detail::parse_json(document, maximum_document_bytes);
    require(parsed.is_object() && parsed.contains("version"), "Invalid scene document");
    require(parsed.at("version").is_number_integer() && parsed.at("version") == document_version,
            "Unsupported scene document version");
    if (kind == scene_kind)
        anima::detail::json_fields(parsed, {"version", "kind", "objects", "next_key"});
    else
        anima::detail::json_fields(parsed, {"version", "kind", "objects"});
    require(parsed.at("kind").is_string() && parsed.at("kind").get_ref<const std::string &>() == kind,
            "Invalid scene document kind");
    MeshResources resources;
    auto nodes = decode_nodes(parsed.at("objects"), kind == prefab_kind, resolve, resources);
    std::uint64_t next_key = 1;
    if (kind == scene_kind)
        next_key = decode_next_key(parsed.at("next_key"), nodes);
    return {std::move(nodes), next_key};
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
            if (!node.mesh)
                continue;
            ScenePersistence::assign_mesh(scene, object.id(), node.mesh, node.pose);
            auto renderer = object.renderer();
            renderer.set_visible(node.visible);
            const auto &instance = scene.instance(object.id());
            require(node.material_factors.empty() || node.material_factors.size() == instance.factors.size(),
                    "Prefab material factors do not match the mesh");
            require(node.primitive_visible.empty() ||
                        node.primitive_visible.size() == instance.primitive_visible.size(),
                    "Prefab primitive visibility does not match the mesh");
            for (std::size_t i = 0; i < node.material_factors.size(); ++i)
                renderer.set_material_factor(i, node.material_factors[i]);
            for (std::size_t i = 0; i < node.primitive_visible.size(); ++i)
                renderer.set_primitive_visible(i, node.primitive_visible[i]);
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
    require(nodes_.size() <= maximum_objects && !nodes_.empty(), "Invalid prefab object count");
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
Prefab Prefab::deserialize(std::string_view document, const MeshResolver &resolve, ComponentCodecs codecs) {
    return Prefab(decode(document, prefab_kind, resolve).nodes, std::move(codecs));
}
std::string serialize_scene(Scene &scene, const MeshName &name, const ComponentCodecs &codecs) {
    const auto nodes = capture_nodes(scene.roots(), codecs);
    validate_nodes(nodes, false);
    return encode(nodes, scene_kind, name, detail::ScenePersistence::next_key(scene));
}
std::shared_ptr<Scene> load_scene(std::string_view document, const MeshResolver &resolve,
                                  const ComponentCodecs &codecs) {
    const auto decoded = decode(document, scene_kind, resolve);
    const auto &nodes = decoded.nodes;
    for (const auto &node : nodes)
        codecs.validate(node.components);
    auto scene = std::make_shared<Scene>();
    detail::ScenePersistence::next_key(*scene, decoded.next_key);
    (void)instantiate_nodes(*scene, nodes, nullptr, identity(), &codecs, true);
    return scene;
}

namespace detail {
std::string serialize_scene_set(SceneSet &scenes, const MeshName &name, const ComponentCodecs &codecs) {
    const auto selected = scenes.scenes();
    require(selected.size() <= maximum_scenes, "Scene set exceeds the scene limit");
    std::vector<Captured> captured;
    std::vector<std::uint64_t> next_keys;
    captured.reserve(selected.size());
    next_keys.reserve(selected.size());
    std::size_t object_count = 0;
    for (auto scene : selected) {
        require(scene->size() <= maximum_objects - object_count, "Scene set exceeds the object limit");
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
    MeshNames resources;
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
    auto document = value.dump(2);
    require(document.size() <= maximum_document_bytes, "Scene set document exceeds the byte limit");
    return document;
}

std::unique_ptr<SceneSet> load_scene_set(std::string_view document, const MeshResolver &resolve,
                                         const ComponentCodecs &codecs) {
    const auto parsed = parse_json(document, maximum_document_bytes);
    json_fields(parsed, {"version", "kind", "active", "scenes", "references"});
    require(parsed.at("version").is_number_integer() && parsed.at("version") == scene_set_version,
            "Unsupported scene set document version");
    require(parsed.at("kind").is_string() && parsed.at("kind").get_ref<const std::string &>() == scene_set_kind,
            "Invalid scene set document kind");
    const auto &documents = parsed.at("scenes"), &table = parsed.at("references");
    require(documents.is_array() && documents.size() <= maximum_scenes, "Invalid scene set scene count");
    require(table.is_array() && table.size() <= maximum_objects, "Invalid scene set reference count");

    struct DecodedScene {
        std::string key;
        Decoded content;
    };
    std::vector<DecodedScene> decoded;
    decoded.reserve(documents.size());
    std::map<std::string, std::size_t, std::less<>> namespaces;
    std::vector<std::set<ObjectKey>> unmapped;
    unmapped.reserve(documents.size());
    MeshResources resources;
    std::size_t object_count = 0;
    for (const auto &value : documents) {
        json_fields(value, {"key", "next_key", "objects"});
        auto key = value.at("key").get<std::string>();
        require(!key.empty() && key.size() <= maximum_namespace_bytes && key.find('\0') == std::string::npos,
                "Invalid scene namespace");
        require(namespaces.emplace(key, decoded.size()).second, "Duplicate scene namespace");
        const auto &objects = value.at("objects");
        require(objects.is_array() && objects.size() <= maximum_objects - object_count,
                "Scene set exceeds the object limit");
        auto nodes = decode_nodes(objects, false, resolve, resources);
        object_count += nodes.size();
        std::set<ObjectKey> keys;
        for (const auto &node : nodes) {
            codecs.validate(node.components);
            keys.insert(node.key);
        }
        const auto next_key = decode_next_key(value.at("next_key"), nodes);
        unmapped.push_back(std::move(keys));
        decoded.push_back({std::move(key), {std::move(nodes), next_key}});
    }
    const auto &active = parsed.at("active");
    std::optional<std::size_t> active_index;
    if (decoded.empty())
        require(active.is_null(), "Empty scene set has an active scene");
    else {
        require(active.is_string(), "Scene set requires an active namespace");
        const auto found = namespaces.find(active.get_ref<const std::string &>());
        require(found != namespaces.end(), "Active scene namespace is missing");
        active_index = found->second;
    }

    struct Reference {
        ObjectKey key, object;
        std::size_t scene;
    };
    std::vector<Reference> decoded_references;
    decoded_references.reserve(table.size());
    std::set<ObjectKey> reference_keys;
    require(table.size() == object_count, "Scene set reference table must cover every object");
    for (const auto &value : table) {
        json_fields(value, {"key", "scene", "object"});
        const auto key = ObjectKey::parse(value.at("key").get<std::string>());
        const auto object = ObjectKey::parse(value.at("object").get<std::string>());
        require(key.value && reference_keys.insert(key).second, "Null or duplicate scene set reference key");
        const auto found = namespaces.find(value.at("scene").get<std::string>());
        require(found != namespaces.end(), "Scene set reference namespace is missing");
        require(unmapped[found->second].erase(object) == 1, "Missing or duplicate scene set reference target");
        decoded_references.push_back({key, object, found->second});
    }

    // A complete owner provides cross-scene rollback: all staged identities die
    // before any decoded component is released, including cyclic object links.
    auto staged = std::make_unique<SceneSet>();
    std::vector<SceneRef> selected;
    selected.reserve(decoded.size());
    for (const auto &scene : decoded) {
        auto created = staged->create(scene.key);
        ScenePersistence::next_key(created.get(), scene.content.next_key);
        (void)instantiate_nodes(created.get(), scene.content.nodes, nullptr, identity(), nullptr, true);
        selected.push_back(created);
    }
    if (active_index)
        staged->set_active(selected[*active_index]);
    std::vector<ObjectReferences::Entry> entries;
    entries.reserve(decoded_references.size());
    for (const auto &reference : decoded_references)
        entries.push_back({reference.key, selected[reference.scene]->find(reference.object)});
    const ObjectReferences references(entries);
    const SceneDriver::Scope scope(*staged);
    for (std::size_t i = 0; i < decoded.size(); ++i)
        for (const auto &node : decoded[i].content.nodes)
            codecs.restore(selected[i]->find(node.key), node.components, references);
    return staged;
}
} // namespace detail
} // namespace anima
