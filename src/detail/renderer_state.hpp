#pragma once
#include "document_json.hpp"
#include "document_limits.hpp"
#include "json.hpp"
#include "staging.hpp"
#include "visibility_range_json.hpp"
#include <anima/prefab.hpp>
#include <map>
#include <stdexcept>
#include <string>

// The RendererState of scene and prefab objects and of prefab variant renderers, and its document fields: `mesh`,
// `pose`, `visible`, `material_factors`, `custom_materials`, `primitive_visible`, `casts_shadows`, `placements` and
// `visibility_range`. Each document kind words the rejections in its own RendererFormat.
namespace anima::detail {
static_assert(CustomMaterial::max_name_bytes == 4096, "prefab.hpp documents the custom material name limit");

// One document kind's messages for renderer rejections.
struct RendererFormat {
    // A renderer without a mesh that has other state.
    const char *empty_state;
    // Placements of another mesh, or placements together with a pose.
    const char *placements;
    // Writing: no mesh naming callback; a mesh key that is empty or too long; two meshes that share a key.
    const char *mesh_naming, *written_mesh_key, *shared_mesh_key;
    // Reading: a mesh key that is empty or too long; no mesh resolver; a key that the resolver maps to null.
    const char *read_mesh_key, *mesh_resolver, *unresolved_mesh;
    // Reading: a pose without a mesh or with another count than the mesh's nodes.
    const char *pose;
    // Reading: material factors that are not an array; a factor that is not an array of three.
    const char *material_factors, *material_factor;
    // Reading: custom materials that are not an array; a name that is empty or too long; a name that the resolver
    // maps to null, or to a material of another name.
    const char *custom_materials, *custom_material_name, *unresolved_custom_material, *renamed_custom_material;
    // Reading: a visibility range that is not null or an object of four numbers.
    const char *visibility_range;
    // Reading: material factors and matrices.
    NumberFormat numbers;
};

// Names that writing one document gave its meshes and custom materials.
struct ResourceNames {
    std::map<const Mesh *, std::string> names;
    std::map<std::string, const Mesh *, std::less<>> identities;
    std::map<std::string, const CustomMaterial *, std::less<>> materials;
};
// Resources resolved while reading one document, each once per key or name.
struct Resources {
    std::map<std::string, std::shared_ptr<const Mesh>, std::less<>> meshes;
    std::map<std::string, std::shared_ptr<const CustomMaterial>, std::less<>> materials;
};

// Checks the rules that need no mesh data: a renderer without a mesh keeps every default, placements copy the mesh of
// a renderer without a pose, and the visibility range is valid. Throws `std::invalid_argument` with @p format's
// messages, and as validate_visibility_range() does.
inline void validate_renderer_state(const RendererState &state, const RendererFormat &format) {
    require(state.mesh || state == RendererState{}, format.empty_state);
    require(!state.placements || (state.placements->mesh() == state.mesh && !state.pose), format.placements);
    validate_visibility_range(state.visibility_range);
}

// Returns the renderer fields of @p state as a JSON object. @p name names each distinct mesh once per document, and
// two meshes or two custom materials cannot share a name in it.
inline nlohmann::json encode_renderer_state(const RendererState &state, const MeshName &name, ResourceNames &resources,
                                            const RendererFormat &format) {
    nlohmann::json mesh = nullptr, pose = nullptr, placements = nullptr;
    if (state.placements)
        placements = state.placements->transforms();
    if (state.mesh) {
        if (!resources.names.contains(state.mesh.get())) {
            require(bool(name), format.mesh_naming);
            auto key = name(state.mesh);
            require(!key.empty() && key.size() <= maximum_key_bytes, format.written_mesh_key);
            const auto [existing, inserted] = resources.identities.emplace(key, state.mesh.get());
            require(inserted || existing->second == state.mesh.get(), format.shared_mesh_key);
            resources.names.emplace(state.mesh.get(), std::move(key));
        }
        mesh = resources.names.at(state.mesh.get());
    }
    if (state.pose)
        pose = state.pose->world;
    auto factors = nlohmann::json::array();
    for (auto factor : state.material_factors)
        factors.push_back({factor.x, factor.y, factor.z});
    auto custom = nlohmann::json::array();
    for (const auto &material : state.custom_materials) {
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
            {"visible", state.visible},
            {"material_factors", factors},
            {"custom_materials", custom},
            {"primitive_visible", state.primitive_visible},
            {"casts_shadows", state.casts_shadows},
            {"placements", placements},
            {"visibility_range", encode_visibility_range(state.visibility_range)}};
}

// Reads the renderer fields of the object @p value, which the caller has checked with json_fields() to have all of
// them. @p resolve and @p materials run once per distinct mesh key and custom material name in @p resources, each as
// a step of @p steps, which is checked before it. Throws `std::invalid_argument` with @p format's messages, and as
// decode_visibility_range() and MeshPlacements::create do.
inline RendererState decode_renderer_state(const nlohmann::json &value, const MeshResolver &resolve,
                                           const CustomMaterialResolver &materials, Resources &resources,
                                           const StagingSteps &steps, const RendererFormat &format) {
    RendererState state;
    const auto &mesh = value.at("mesh");
    if (!mesh.is_null()) {
        const auto key = mesh.get<std::string>();
        require(!key.empty() && key.size() <= maximum_key_bytes, format.read_mesh_key);
        require(bool(resolve), format.mesh_resolver);
        auto found = resources.meshes.find(key);
        if (found == resources.meshes.end()) {
            steps.check();
            auto resource = resolve(key);
            require(bool(resource), format.unresolved_mesh);
            found = resources.meshes.emplace(key, std::move(resource)).first;
            steps.complete();
        }
        state.mesh = found->second;
    }
    const auto &pose = value.at("pose");
    if (!pose.is_null()) {
        require(pose.is_array() && state.mesh && pose.size() == state.mesh->rest_pose().world.size(), format.pose);
        state.pose.emplace();
        for (const auto &world : pose)
            state.pose->world.push_back(document_matrix(world, format.numbers));
    }
    state.visible = value.at("visible").get<bool>();
    const auto &factors = value.at("material_factors");
    require(factors.is_array(), format.material_factors);
    for (const auto &factor : factors) {
        require(factor.is_array() && factor.size() == 3, format.material_factor);
        state.material_factors.push_back({document_scalar(factor[0], format.numbers),
                                          document_scalar(factor[1], format.numbers),
                                          document_scalar(factor[2], format.numbers)});
    }
    const auto &custom = value.at("custom_materials");
    require(custom.is_array(), format.custom_materials);
    for (const auto &entry : custom) {
        if (entry.is_null()) {
            state.custom_materials.emplace_back();
            continue;
        }
        const auto name = entry.get<std::string>();
        require(!name.empty() && name.size() <= CustomMaterial::max_name_bytes, format.custom_material_name);
        auto found = resources.materials.find(name);
        if (found == resources.materials.end()) {
            steps.check();
            auto material = materials ? materials(name) : nullptr;
            require(bool(material), format.unresolved_custom_material);
            require(material->name() == name, format.renamed_custom_material);
            found = resources.materials.emplace(name, std::move(material)).first;
            steps.complete();
        }
        state.custom_materials.push_back(found->second);
    }
    state.primitive_visible = value.at("primitive_visible").get<std::vector<bool>>();
    state.casts_shadows = value.at("casts_shadows").get<bool>();
    const auto &placements = value.at("placements");
    if (!placements.is_null()) {
        require(placements.is_array() && state.mesh && !state.pose, format.placements);
        std::vector<Mat4> transforms;
        transforms.reserve(placements.size());
        for (const auto &transform : placements)
            transforms.push_back(document_matrix(transform, format.numbers));
        state.placements = MeshPlacements::create(state.mesh, transforms);
    }
    state.visibility_range = decode_visibility_range(value.at("visibility_range"), format.visibility_range);
    return state;
}
} // namespace anima::detail
