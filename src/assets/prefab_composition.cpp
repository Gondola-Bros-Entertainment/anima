#include "../detail/json.hpp"
#include "../detail/prefab_instantiation.hpp"
#include <anima/prefab_composition.hpp>
#include <cmath>
#include <map>
#include <utility>

namespace anima {
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 1;
constexpr std::string_view document_kind = "anima.prefab-composition";
constexpr std::size_t maximum_parts = 1024, maximum_objects = 65'536;
constexpr std::size_t maximum_key_bytes = 4096, maximum_document_bytes = 16 * 1024 * 1024;

void require(bool accepted, const char *reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}
void validate_key(std::string_view key) {
    require(!key.empty() && key.size() <= maximum_key_bytes, "Invalid prefab composition part or resource key");
}
Mat4 matrix_value(const Json &value) {
    require(value.is_array() && value.size() == 16, "Prefab composition matrix requires 16 scalars");
    Mat4 result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(value[i].is_number(), "Prefab composition scalar must be a number");
        result[i] = value[i].get<float>();
        require(std::isfinite(result[i]), "Prefab composition scalar must be finite");
    }
    return result;
}
struct Resource {
    std::shared_ptr<const Prefab> prefab;
    std::map<ObjectKey, std::size_t> nodes;
};
struct ResolvedPart {
    const Resource *resource;
    std::optional<std::pair<std::size_t, std::size_t>> mount; // Part and authored node indices.
};
} // namespace

PrefabComposition::PrefabComposition(std::vector<Part> parts) : parts_(std::move(parts)) {
    require(!parts_.empty() && parts_.size() <= maximum_parts, "Invalid prefab composition part count");
    std::map<std::string_view, std::size_t> indices;
    Scene validation;
    auto object = validation.create();
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        const auto &part = parts_[i];
        validate_key(part.key);
        validate_key(part.prefab);
        require(indices.emplace(part.key, i).second, "Duplicate prefab composition part key");
        require(i == 0 ? !part.parent : bool(part.parent), "Prefab composition requires one first root part");
        if (part.parent) {
            validate_key(part.parent->part);
            const auto found = indices.find(part.parent->part);
            require(found != indices.end() && found->second < i, "Prefab composition mount must name an earlier part");
            require(part.parent->object.value != 0, "Null prefab composition mount object key");
        }
        // A part's parent world is unknown until resolution. Validate only its
        // authored affine matrix here, without inventing renderer bounds.
        object.set_local_matrix(part.placement);
    }
}

GameObject PrefabComposition::create(Scene &scene, const GameObject *parent, const PrefabResolver &resolve,
                                     const ComponentCodecs &codecs, const Mat4 &placement) const {
    require(bool(resolve), "Prefab composition instantiation needs a resource resolver");
    {
        Scene validation;
        validation.create().set_local_matrix(placement);
    }
    std::map<std::string, Resource, std::less<>> resources;
    std::map<std::string_view, std::size_t> indices;
    std::vector<ResolvedPart> resolved;
    resolved.reserve(parts_.size());
    std::size_t count = 0;
    for (const auto &part : parts_) {
        auto found = resources.find(part.prefab);
        if (found == resources.end()) {
            Resource resource{resolve(part.prefab), {}};
            require(bool(resource.prefab), "Prefab composition resource key could not be resolved");
            const auto nodes = resource.prefab->nodes();
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                codecs.validate(nodes[i].components);
                resource.nodes.emplace(nodes[i].key, i);
            }
            found = resources.emplace(part.prefab, std::move(resource)).first;
        }
        const auto &resource = found->second;
        require(resource.prefab->nodes().size() <= maximum_objects - count,
                "Prefab composition exceeds the object limit");
        count += resource.prefab->nodes().size();
        ResolvedPart result{&resource, {}};
        if (part.parent) {
            const auto part_index = indices.at(part.parent->part);
            const auto &mounted = *resolved[part_index].resource;
            const auto node = mounted.nodes.find(part.parent->object);
            require(node != mounted.nodes.end(), "Prefab composition mount object is missing");
            result.mount = std::pair{part_index, node->second};
        }
        indices.emplace(part.key, resolved.size());
        resolved.push_back(result);
    }

    // Resource, mount and codec selection is complete before any scene mutation.
    // Each native batch owns its own failure cleanup until moved into this array.
    std::vector<std::vector<GameObject>> objects(parts_.size());
    try {
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            const auto &part = resolved[i];
            const GameObject *mount = parent;
            if (part.mount)
                mount = &objects[part.mount->first][part.mount->second];
            const auto local = i == 0 ? placement * parts_[i].placement : parts_[i].placement;
            objects[i] = detail::instantiate_prefab_nodes(scene, part.resource->prefab->nodes(), mount, local);
        }
        // Every part is mounted before any decoder. Repeated resource instances
        // retain separate authored reference scopes, including identical keys.
        for (std::size_t i = 0; i < parts_.size(); ++i)
            detail::restore_prefab_components(objects[i], resolved[i].resource->prefab->nodes(), codecs);
    } catch (...) {
        // Retire the complete connected hierarchy before component destructors
        // run. The fallback also covers objects removed by application callbacks.
        if (!objects.front().empty() && objects.front().front().valid())
            objects.front().front().destroy();
        for (const auto &part : objects)
            detail::destroy_prefab_objects(part);
        throw;
    }
    return objects.front().front();
}

GameObject PrefabComposition::instantiate(Scene &scene, const PrefabResolver &resolve, const ComponentCodecs &codecs,
                                          const Mat4 &placement) const {
    return create(scene, nullptr, resolve, codecs, placement);
}
GameObject PrefabComposition::instantiate(GameObject parent, const PrefabResolver &resolve,
                                          const ComponentCodecs &codecs, const Mat4 &placement) const {
    return create(parent.scene(), &parent, resolve, codecs, placement);
}

std::string PrefabComposition::serialize() const {
    auto parts = Json::array();
    for (const auto &part : parts_) {
        Json parent = nullptr;
        if (part.parent)
            parent = {{"part", part.parent->part}, {"object", part.parent->object.string()}};
        parts.push_back(
            {{"key", part.key}, {"prefab", part.prefab}, {"parent", parent}, {"placement", part.placement}});
    }
    const Json value{{"version", document_version}, {"kind", document_kind}, {"parts", parts}};
    auto document = value.dump(2);
    require(document.size() <= maximum_document_bytes, "Prefab composition document exceeds the byte limit");
    return document;
}

PrefabComposition PrefabComposition::deserialize(std::string_view document) {
    const auto parsed = detail::parse_json(document, maximum_document_bytes);
    detail::json_fields(parsed, {"version", "kind", "parts"});
    require(parsed.at("version").is_number_integer() && parsed.at("version") == document_version,
            "Unsupported prefab composition document version");
    require(parsed.at("kind").is_string() && parsed.at("kind").get_ref<const std::string &>() == document_kind,
            "Invalid prefab composition document kind");
    const auto &values = parsed.at("parts");
    require(values.is_array() && !values.empty() && values.size() <= maximum_parts,
            "Invalid prefab composition part count");
    std::vector<Part> parts;
    parts.reserve(values.size());
    for (const auto &value : values) {
        detail::json_fields(value, {"key", "prefab", "parent", "placement"});
        Part part;
        part.key = value.at("key").get<std::string>();
        part.prefab = value.at("prefab").get<std::string>();
        const auto &parent = value.at("parent");
        if (!parent.is_null()) {
            detail::json_fields(parent, {"part", "object"});
            part.parent =
                Mount{parent.at("part").get<std::string>(), ObjectKey::parse(parent.at("object").get<std::string>())};
        }
        part.placement = matrix_value(value.at("placement"));
        parts.push_back(std::move(part));
    }
    return PrefabComposition(std::move(parts));
}
} // namespace anima
