#include "../detail/json.hpp"
#include "../detail/visibility_range_json.hpp"
#include <algorithm>
#include <anima/prefab_variant.hpp>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace anima {
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 1;
constexpr std::string_view document_kind = "anima.prefab-variant";
constexpr std::size_t maximum_overrides = 65'536, maximum_document_bytes = 16 * 1024 * 1024;
constexpr std::size_t maximum_components = 1024, maximum_key_bytes = 4096;

void require(bool accepted, const char *reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}
void validate_key(std::string_view key) {
    require(!key.empty() && key.size() <= maximum_key_bytes, "Invalid prefab variant resource or component key");
}
float scalar(const Json &value) {
    require(value.is_number(), "Prefab variant scalar must be a number");
    const auto result = detail::json_float(value);
    require(std::isfinite(result), "Prefab variant scalar must be finite");
    return result;
}
Mat4 matrix_value(const Json &value) {
    require(value.is_array() && value.size() == 16, "Prefab variant matrix requires 16 scalars");
    Mat4 result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = scalar(value[i]);
    return result;
}
void validate_native(Scene &validation, const PrefabVariant::Override &value) {
    const auto *renderer = value.renderer ? &*value.renderer : nullptr;
    require(!renderer || renderer->mesh ||
                (!renderer->pose && renderer->visible && renderer->material_factors.empty() &&
                 renderer->custom_materials.empty() && renderer->primitive_visible.empty() && renderer->casts_shadows &&
                 !renderer->placements && renderer->visibility_range == VisibilityRange{}),
            "Empty prefab variant renderer has state");
    require(!renderer || !renderer->placements || (renderer->placements->mesh() == renderer->mesh && !renderer->pose),
            "Prefab variant placements must copy the renderer's mesh, which has no pose");
    if (renderer)
        validate_visibility_range(renderer->visibility_range);
    auto object = validation.create();
    if (value.local)
        object.set_local_matrix(*value.local);
    if (!renderer || !renderer->mesh) {
        object.destroy();
        return;
    }
    // The base parent is not available yet. Validate authored matrices without
    // a renderer; resolving checks their actual world composition and bounds.
    if (renderer->pose) {
        require(renderer->pose->world.size() == renderer->mesh->rest_pose().world.size(),
                "Prefab variant pose does not match the mesh");
        for (const auto &world : renderer->pose->world)
            object.set_local_matrix(world);
    }
    require(renderer->material_factors.empty() ||
                renderer->material_factors.size() == renderer->mesh->description()->materials.size(),
            "Prefab variant material factors do not match the mesh");
    require(renderer->custom_materials.empty() ||
                renderer->custom_materials.size() == renderer->mesh->description()->materials.size(),
            "Prefab variant custom materials do not match the mesh");
    require(renderer->primitive_visible.empty() ||
                renderer->primitive_visible.size() == renderer->mesh->primitives().size(),
            "Prefab variant primitive visibility does not match the mesh");
    for (auto factor : renderer->material_factors)
        for (auto channel : {factor.x, factor.y, factor.z})
            require(std::isfinite(channel) && channel >= 0 && channel <= 1, "Invalid prefab variant material factor");
    object.destroy();
}
// Names written into one document.
struct ResourceNames {
    std::map<const Mesh *, std::string> names;
    std::map<std::string, const Mesh *, std::less<>> identities;
    std::map<std::string, const CustomMaterial *, std::less<>> materials;
};
Json encode_renderer(const PrefabVariant::Renderer &renderer, const MeshName &name, ResourceNames &resources) {
    Json mesh = nullptr, pose = nullptr, placements = nullptr;
    if (renderer.placements)
        placements = renderer.placements->transforms();
    if (renderer.mesh) {
        if (!resources.names.contains(renderer.mesh.get())) {
            require(bool(name), "Prefab variant serialization needs a mesh naming callback");
            auto key = name(renderer.mesh);
            validate_key(key);
            const auto [existing, inserted] = resources.identities.emplace(key, renderer.mesh.get());
            require(inserted || existing->second == renderer.mesh.get(),
                    "Different meshes share a prefab variant resource key");
            resources.names.emplace(renderer.mesh.get(), std::move(key));
        }
        mesh = resources.names.at(renderer.mesh.get());
    }
    if (renderer.pose)
        pose = renderer.pose->world;
    auto factors = Json::array();
    for (auto factor : renderer.material_factors)
        factors.push_back({factor.x, factor.y, factor.z});
    auto custom = Json::array();
    for (const auto &material : renderer.custom_materials) {
        if (!material) {
            custom.push_back(nullptr);
            continue;
        }
        const auto [existing, inserted] = resources.materials.emplace(material->name(), material.get());
        require(inserted || existing->second == material.get(), "Different custom materials share a document name");
        custom.push_back(material->name());
    }
    return {{"mesh", mesh},
            {"pose", pose},
            {"visible", renderer.visible},
            {"material_factors", factors},
            {"custom_materials", custom},
            {"primitive_visible", renderer.primitive_visible},
            {"casts_shadows", renderer.casts_shadows},
            {"placements", placements},
            {"visibility_range", detail::encode_visibility_range(renderer.visibility_range)}};
}
// Resources resolved while reading one document, each once per key or name.
struct Resources {
    std::map<std::string, std::shared_ptr<const Mesh>, std::less<>> meshes;
    std::map<std::string, std::shared_ptr<const CustomMaterial>, std::less<>> materials;
};
PrefabVariant::Renderer decode_renderer(const Json &value, const MeshResolver &resolve,
                                        const CustomMaterialResolver &materials, Resources &resources) {
    // An omitted setting takes the default that PrefabVariant::Renderer declares.
    detail::json_fields(value, {"mesh"},
                        {"pose", "visible", "material_factors", "custom_materials", "primitive_visible",
                         "casts_shadows", "placements", "visibility_range"});
    PrefabVariant::Renderer renderer;
    const auto &mesh = value.at("mesh");
    if (!mesh.is_null()) {
        const auto key = mesh.get<std::string>();
        validate_key(key);
        require(bool(resolve), "Prefab variant loading needs a mesh resolver");
        auto found = resources.meshes.find(key);
        if (found == resources.meshes.end()) {
            auto resource = resolve(key);
            require(bool(resource), "Prefab variant mesh key could not be resolved");
            found = resources.meshes.emplace(key, std::move(resource)).first;
        }
        renderer.mesh = found->second;
    }
    static const Json omitted;
    const auto &pose = value.contains("pose") ? value.at("pose") : omitted;
    if (!pose.is_null()) {
        require(pose.is_array() && renderer.mesh && pose.size() == renderer.mesh->rest_pose().world.size(),
                "Prefab variant pose does not match the mesh");
        renderer.pose.emplace();
        for (const auto &world : pose)
            renderer.pose->world.push_back(matrix_value(world));
    }
    renderer.visible = value.value("visible", renderer.visible);
    if (value.contains("material_factors")) {
        const auto &factors = value.at("material_factors");
        require(factors.is_array(), "Invalid prefab variant material factors");
        for (const auto &factor : factors) {
            require(factor.is_array() && factor.size() == 3, "Prefab variant material factor requires RGB");
            renderer.material_factors.push_back({scalar(factor[0]), scalar(factor[1]), scalar(factor[2])});
        }
    }
    if (value.contains("custom_materials")) {
        const auto &custom = value.at("custom_materials");
        require(custom.is_array(), "Invalid prefab variant custom materials");
        for (const auto &entry : custom) {
            if (entry.is_null()) {
                renderer.custom_materials.emplace_back();
                continue;
            }
            const auto name = entry.get<std::string>();
            require(!name.empty() && name.size() <= CustomMaterial::max_name_bytes,
                    "Invalid prefab variant custom material name");
            auto found = resources.materials.find(name);
            if (found == resources.materials.end()) {
                auto material = materials ? materials(name) : nullptr;
                require(bool(material), "Prefab variant custom material name could not be resolved");
                require(material->name() == name,
                        "Prefab variant custom material resolved to a material of another name");
                found = resources.materials.emplace(name, std::move(material)).first;
            }
            renderer.custom_materials.push_back(found->second);
        }
    }
    if (value.contains("primitive_visible"))
        renderer.primitive_visible = value.at("primitive_visible").get<std::vector<bool>>();
    renderer.casts_shadows = value.value("casts_shadows", renderer.casts_shadows);
    const auto &placements = value.contains("placements") ? value.at("placements") : omitted;
    if (!placements.is_null()) {
        require(placements.is_array() && renderer.mesh && !renderer.pose,
                "Prefab variant placements must copy the renderer's mesh, which has no pose");
        std::vector<Mat4> transforms;
        transforms.reserve(placements.size());
        for (const auto &transform : placements)
            transforms.push_back(matrix_value(transform));
        renderer.placements = MeshPlacements::create(renderer.mesh, transforms);
    }
    renderer.visibility_range =
        detail::decode_visibility_range(value.contains("visibility_range") ? value.at("visibility_range") : omitted,
                                        "Invalid prefab variant visibility range");
    return renderer;
}
} // namespace

PrefabVariant::PrefabVariant(std::string base_key, std::vector<Override> overrides)
    : base_key_(std::move(base_key)), overrides_(std::move(overrides)) {
    validate_key(base_key_);
    require(overrides_.size() <= maximum_overrides, "Invalid prefab variant override count");
    std::set<ObjectKey> keys;
    Scene validation;
    for (const auto &value : overrides_) {
        require(value.key.value && keys.insert(value.key).second, "Null or duplicate prefab variant object key");
        require(value.name || value.local || value.active || value.renderer || !value.set_components.empty() ||
                    !value.remove_components.empty(),
                "Empty prefab variant override");
        require(value.set_components.size() <= maximum_components &&
                    value.remove_components.size() <= maximum_components,
                "Invalid prefab variant component count");
        std::set<std::string_view> types;
        for (const auto &component : value.set_components) {
            validate_key(component.type);
            require(types.insert(component.type).second, "Duplicate prefab variant component override");
        }
        for (const auto &type : value.remove_components) {
            validate_key(type);
            require(types.insert(type).second, "Duplicate or conflicting prefab variant component removal");
        }
        validate_native(validation, value);
    }
}

Prefab PrefabVariant::resolve(const PrefabResolver &resolver, ComponentCodecs codecs) const {
    require(bool(resolver), "Prefab variant resolution needs a base resolver");
    const auto base = resolver(base_key_);
    require(bool(base), "Prefab variant base key could not be resolved");
    std::vector<Prefab::Node> nodes(base->nodes().begin(), base->nodes().end());
    std::map<ObjectKey, std::size_t> indices;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        indices.emplace(nodes[i].key, i);
    for (const auto &value : overrides_) {
        const auto found = indices.find(value.key);
        require(found != indices.end(), "Unknown prefab variant object key");
        auto &node = nodes[found->second];
        if (value.name)
            node.name = *value.name;
        if (value.local)
            node.local = *value.local;
        if (value.active)
            node.active = *value.active;
        if (value.renderer) {
            const auto &renderer = *value.renderer;
            node.mesh = renderer.mesh;
            node.pose = renderer.pose;
            node.visible = renderer.visible;
            node.material_factors = renderer.material_factors;
            node.custom_materials = renderer.custom_materials;
            node.primitive_visible = renderer.primitive_visible;
            node.casts_shadows = renderer.casts_shadows;
            node.placements = renderer.placements;
            node.visibility_range = renderer.visibility_range;
        }
        for (const auto &type : value.remove_components) {
            const auto component = std::find_if(node.components.begin(), node.components.end(),
                                                [&](const auto &data) { return data.type == type; });
            require(component != node.components.end(), "Unknown prefab variant component removal");
            node.components.erase(component);
        }
        for (const auto &data : value.set_components) {
            const auto component = std::find_if(node.components.begin(), node.components.end(),
                                                [&](const auto &existing) { return existing.type == data.type; });
            if (component == node.components.end())
                node.components.push_back(data);
            else
                *component = data;
        }
    }
    return Prefab(std::move(nodes), std::move(codecs));
}

std::string PrefabVariant::serialize(const MeshName &name) const {
    ResourceNames resources;
    auto overrides = Json::array();
    for (const auto &value : overrides_) {
        Json node_name = nullptr, local = nullptr, active = nullptr, renderer = nullptr;
        if (value.name)
            node_name = *value.name;
        if (value.local)
            local = *value.local;
        if (value.active)
            active = *value.active;
        if (value.renderer)
            renderer = encode_renderer(*value.renderer, name, resources);
        auto components = Json::array();
        for (const auto &component : value.set_components)
            components.push_back(
                {{"type", component.type}, {"state", component.state}, {"enabled", component.enabled}});
        overrides.push_back({{"key", value.key.string()},
                             {"name", node_name},
                             {"local", local},
                             {"active", active},
                             {"renderer", renderer},
                             {"set_components", components},
                             {"remove_components", value.remove_components}});
    }
    const Json value{
        {"version", document_version}, {"kind", document_kind}, {"base", base_key_}, {"overrides", overrides}};
    auto document = detail::json_step([&] { return value.dump(2); });
    require(document.size() <= maximum_document_bytes, "Prefab variant document exceeds the byte limit");
    return document;
}

namespace {
using Override = PrefabVariant::Override;
PrefabVariant decode_variant(std::string_view document, const MeshResolver &resolve,
                             const CustomMaterialResolver &materials) {
    const auto parsed = detail::parse_json(document, maximum_document_bytes);
    detail::json_version(parsed, "version", document_version, "Unsupported prefab variant document version");
    require(parsed.contains("kind") && parsed.at("kind").is_string() &&
                parsed.at("kind").get_ref<const std::string &>() == document_kind,
            "Invalid prefab variant document kind");
    detail::json_fields(parsed, {"version", "kind", "base", "overrides"});
    auto base = parsed.at("base").get<std::string>();
    validate_key(base);
    const auto &values = parsed.at("overrides");
    require(values.is_array() && values.size() <= maximum_overrides, "Invalid prefab variant override count");
    std::vector<Override> overrides;
    overrides.reserve(values.size());
    Resources resources;
    for (const auto &value : values) {
        // An omitted change inherits from the base, as null does.
        detail::json_fields(value, {"key"},
                            {"name", "local", "active", "renderer", "set_components", "remove_components"});
        Override result;
        result.key = ObjectKey::parse(value.at("key").get<std::string>());
        const auto given = [&](const char *name) { return value.contains(name) && !value.at(name).is_null(); };
        if (given("name"))
            result.name = value.at("name").get<std::string>();
        if (given("local"))
            result.local = matrix_value(value.at("local"));
        if (given("active"))
            result.active = value.at("active").get<bool>();
        if (given("renderer"))
            result.renderer = decode_renderer(value.at("renderer"), resolve, materials, resources);
        if (value.contains("set_components")) {
            const auto &components = value.at("set_components");
            require(components.is_array() && components.size() <= maximum_components,
                    "Invalid prefab variant component count");
            for (const auto &component : components) {
                detail::json_fields(component, {"type", "state", "enabled"});
                result.set_components.push_back({component.at("type").get<std::string>(),
                                                 component.at("state").get<std::string>(),
                                                 component.at("enabled").get<bool>()});
            }
        }
        if (value.contains("remove_components")) {
            const auto &removed = value.at("remove_components");
            require(removed.is_array() && removed.size() <= maximum_components,
                    "Invalid prefab variant component removal count");
            result.remove_components = removed.get<std::vector<std::string>>();
        }
        overrides.push_back(std::move(result));
    }
    return PrefabVariant(std::move(base), std::move(overrides));
}
} // namespace
PrefabVariant PrefabVariant::deserialize(std::string_view document, const MeshResolver &resolve,
                                         const CustomMaterialResolver &materials) {
    return detail::json_step([&] { return decode_variant(document, resolve, materials); });
}
} // namespace anima
