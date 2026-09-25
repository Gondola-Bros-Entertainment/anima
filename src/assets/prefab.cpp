#include "../detail/json.hpp"
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
    std::vector<GameObject> objects, roots;
    objects.reserve(nodes.size());
    roots.reserve(nodes.size());
    try {
        for (const auto &node : nodes) {
            auto object = preserve_keys ? detail::ScenePersistence::create(scene, node.key, node.name, node.mesh)
                                        : scene.create(node.name, node.mesh);
            objects.push_back(object);
            object.set_active(node.active);
            if (node.parent)
                object.set_parent(objects.at(*node.parent), ReparentMode::keep_local);
            else {
                roots.push_back(object);
                if (parent)
                    object.set_parent(*parent, ReparentMode::keep_local);
            }
            object.set_local_matrix(node.parent ? node.local : placement * node.local);
            if (!node.mesh)
                continue;
            auto renderer = object.renderer();
            if (node.pose)
                renderer.set_pose(*node.pose);
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
        // All transforms, renderers and parent links exist before user components.
        if (codecs) {
            std::vector<ObjectReferences::Entry> entries;
            entries.reserve(nodes.size());
            for (std::size_t i = 0; i < nodes.size(); ++i)
                entries.push_back({nodes[i].key, objects[i]});
            const ObjectReferences references(entries);
            for (std::size_t i = 0; i < nodes.size(); ++i)
                codecs->restore(objects[i], nodes[i].components, references);
        }
    } catch (...) {
        // Includes an object whose parenting/transform failed before it was linked.
        for (auto it = objects.rbegin(); it != objects.rend(); ++it)
            if (it->valid())
                it->destroy();
        throw;
    }
    return roots;
}
std::vector<Prefab::Node> capture_nodes(std::span<const GameObject> roots, const ComponentCodecs &codecs) {
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
    // Build the complete graph before any encoder, including links across roots.
    const ObjectReferences references(objects);
    for (std::size_t i = 0; i < objects.size(); ++i)
        nodes[i].components = codecs.capture(objects[i], references);
    return nodes;
}
std::string encode(std::span<const Prefab::Node> nodes, std::string_view kind, const MeshName &name,
                   std::uint64_t next_key = 1) {
    std::map<const Mesh *, std::string> names;
    std::map<std::string, const Mesh *, std::less<>> identities;
    auto objects = Json::array();
    for (const auto &node : nodes) {
        Json mesh = nullptr, parent = nullptr, pose = nullptr;
        if (node.parent)
            parent = *node.parent;
        if (node.mesh) {
            if (!names.contains(node.mesh.get())) {
                require(bool(name), "Scene serialization needs a mesh naming callback");
                auto key = name(node.mesh);
                require(!key.empty() && key.size() <= maximum_mesh_key_bytes, "Invalid scene mesh key");
                const auto [existing, inserted] = identities.emplace(key, node.mesh.get());
                require(inserted || existing->second == node.mesh.get(), "Different meshes share a scene resource key");
                names.emplace(node.mesh.get(), std::move(key));
            }
            mesh = names.at(node.mesh.get());
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
    Json value{{"version", document_version}, {"kind", kind}, {"objects", std::move(objects)}};
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
    const auto &objects = parsed.at("objects");
    require(objects.is_array() && objects.size() <= maximum_objects, "Invalid scene object count");
    std::vector<Prefab::Node> nodes;
    nodes.reserve(objects.size());
    // Resource resolution is explicit and cached once per key. No scene is mutated here.
    std::map<std::string, std::shared_ptr<const Mesh>, std::less<>> resources;
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
    validate_nodes(nodes, kind == prefab_kind);
    std::uint64_t next_key = 1;
    if (kind == scene_kind) {
        next_key = ObjectKey::parse(parsed.at("next_key").get<std::string>()).value;
        for (const auto &node : nodes)
            require(!next_key || next_key > node.key.value, "Invalid scene next object key");
    }
    return {std::move(nodes), next_key};
}
} // namespace

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
GameObject Prefab::create(Scene &scene, const GameObject *parent, const Mat4 &placement) const {
    return instantiate_nodes(scene, nodes_, parent, placement, &codecs_).front();
}
GameObject Prefab::instantiate(Scene &scene, const Mat4 &placement) const { return create(scene, nullptr, placement); }
GameObject Prefab::instantiate(GameObject parent, const Mat4 &placement) const {
    return create(parent.scene(), &parent, placement);
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
} // namespace anima
