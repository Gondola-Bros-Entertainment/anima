#include "../detail/affine.hpp"
#include "../detail/document_json.hpp"
#include "../detail/document_limits.hpp"
#include "../detail/json.hpp"
#include "../detail/renderer_state.hpp"
#include <algorithm>
#include <anima/prefab_variant.hpp>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace anima {
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 2;
constexpr std::string_view document_kind = "anima.prefab-variant";
using detail::maximum_document_bytes, detail::maximum_document_objects, detail::maximum_object_components;
using detail::require;
constexpr detail::NumberFormat number_format{
    .scalar = "Prefab variant scalar must be a number",
    .finite = "Prefab variant scalar must be finite",
    .matrix = "Prefab variant matrix requires 16 scalars",
};

void validate_key(std::string_view key) {
    require(!key.empty() && key.size() <= detail::maximum_key_bytes,
            "Invalid prefab variant resource or component key");
}
constexpr detail::RendererFormat renderer_format{
    .empty_state = "Empty prefab variant renderer has state",
    .placements = "Prefab variant placements must copy the renderer's mesh, which has no pose",
    .mesh_naming = "Prefab variant serialization needs a mesh naming callback",
    .written_mesh_key = "Invalid prefab variant resource or component key",
    .shared_mesh_key = "Different meshes share a prefab variant resource key",
    .read_mesh_key = "Invalid prefab variant resource or component key",
    .mesh_resolver = "Prefab variant loading needs a mesh resolver",
    .unresolved_mesh = "Prefab variant mesh key could not be resolved",
    .pose = "Prefab variant pose does not match the mesh",
    .material_factors = "Invalid prefab variant material factors",
    .material_factor = "Prefab variant material factor requires RGB",
    .custom_materials = "Invalid prefab variant custom materials",
    .custom_material_name = "Invalid prefab variant custom material name",
    .unresolved_custom_material = "Prefab variant custom material name could not be resolved",
    .renamed_custom_material = "Prefab variant custom material resolved to a material of another name",
    .visibility_range = "Invalid prefab variant visibility range",
    .numbers = number_format,
};
void validate_native(const PrefabVariant::Override &value) {
    const auto *renderer = value.renderer ? &*value.renderer : nullptr;
    if (renderer)
        detail::validate_renderer_state(*renderer, renderer_format);
    if (value.local)
        detail::require_affine(*value.local);
    if (!renderer || !renderer->mesh)
        return;
    // The base parent is not available yet. Validate authored matrices without
    // a renderer; resolving checks their actual world composition and bounds.
    if (renderer->pose) {
        require(renderer->pose->world.size() == renderer->mesh->rest_pose().world.size(),
                "Prefab variant pose does not match the mesh");
        for (const auto &world : renderer->pose->world)
            detail::require_affine(world);
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
}
} // namespace

PrefabVariant::PrefabVariant(std::string base_key, std::vector<Override> overrides)
    : base_key_(std::move(base_key)), overrides_(std::move(overrides)) {
    validate_key(base_key_);
    require(overrides_.size() <= maximum_document_objects, "Invalid prefab variant override count");
    std::set<ObjectKey> keys;
    for (const auto &value : overrides_) {
        require(value.key.value && keys.insert(value.key).second, "Null or duplicate prefab variant object key");
        require(value.name || value.local || value.active || value.renderer || !value.set_components.empty() ||
                    !value.remove_components.empty(),
                "Empty prefab variant override");
        require(value.set_components.size() <= maximum_object_components &&
                    value.remove_components.size() <= maximum_object_components,
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
        validate_native(value);
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
        if (value.renderer)
            node.renderer = *value.renderer;
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
    detail::ResourceNames resources;
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
            renderer = detail::encode_renderer_state(*value.renderer, name, resources, renderer_format);
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
    require(values.is_array() && values.size() <= maximum_document_objects, "Invalid prefab variant override count");
    std::vector<Override> overrides;
    overrides.reserve(values.size());
    detail::Resources resources;
    for (const auto &value : values) {
        detail::json_fields(value,
                            {"key", "name", "local", "active", "renderer", "set_components", "remove_components"});
        Override result;
        result.key = ObjectKey::parse(value.at("key").get<std::string>());
        // Null inherits from the base.
        const auto given = [&](const char *name) { return !value.at(name).is_null(); };
        if (given("name"))
            result.name = value.at("name").get<std::string>();
        if (given("local"))
            result.local = detail::document_matrix(value.at("local"), number_format);
        if (given("active"))
            result.active = value.at("active").get<bool>();
        if (given("renderer")) {
            const auto &renderer = value.at("renderer");
            detail::json_fields(renderer, {"mesh", "pose", "visible", "material_factors", "custom_materials",
                                           "primitive_visible", "casts_shadows", "placements", "visibility_range"});
            result.renderer =
                detail::decode_renderer_state(renderer, resolve, materials, resources, {}, renderer_format);
        }
        const auto &components = value.at("set_components");
        require(components.is_array() && components.size() <= maximum_object_components,
                "Invalid prefab variant component count");
        for (const auto &component : components) {
            detail::json_fields(component, {"type", "state", "enabled"});
            result.set_components.push_back({component.at("type").get<std::string>(),
                                             component.at("state").get<std::string>(),
                                             component.at("enabled").get<bool>()});
        }
        const auto &removed = value.at("remove_components");
        require(removed.is_array() && removed.size() <= maximum_object_components,
                "Invalid prefab variant component removal count");
        result.remove_components = removed.get<std::vector<std::string>>();
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
