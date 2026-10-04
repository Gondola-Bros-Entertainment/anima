#pragma once
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
// Bytes of a mesh key in any document.
inline constexpr std::size_t maximum_mesh_key_bytes = 4096;
static_assert(CustomMaterial::max_name_bytes == 4096, "prefab.hpp documents the custom material name limit");

// One document kind's messages for renderer rejections, and its number readers, which throw with its messages.
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
    float (*scalar)(const nlohmann::json &value);
    Mat4 (*matrix)(const nlohmann::json &value);
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

inline void require_renderer(bool accepted, const char *reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}

// Checks the rules that need no mesh data: a renderer without a mesh keeps every default, placements copy the mesh of
// a renderer without a pose, and the visibility range is valid. Throws `std::invalid_argument` with @p format's
// messages, and as validate_visibility_range() does.
inline void validate_renderer_state(const RendererState &state, const RendererFormat &format) {
    require_renderer(state.mesh || state == RendererState{}, format.empty_state);
    require_renderer(!state.placements || (state.placements->mesh() == state.mesh && !state.pose), format.placements);
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
            require_renderer(bool(name), format.mesh_naming);
            auto key = name(state.mesh);
            require_renderer(!key.empty() && key.size() <= maximum_mesh_key_bytes, format.written_mesh_key);
            const auto [existing, inserted] = resources.identities.emplace(key, state.mesh.get());
            require_renderer(inserted || existing->second == state.mesh.get(), format.shared_mesh_key);
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
        require_renderer(inserted || existing->second == material.get(),
                         "Different custom materials share a document name");
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

// Reads the renderer fields of the object @p value, whose field set the caller has checked; an omitted field other than
// `mesh` takes its RendererState default. @p resolve and @p materials run once per distinct mesh key and custom
// material name in @p resources, each as a step of @p steps, which is checked before it. Throws
// `std::invalid_argument` with @p format's messages, and as validate_visibility_range() and MeshPlacements::create do.
inline RendererState decode_renderer_state(const nlohmann::json &value, const MeshResolver &resolve,
                                           const CustomMaterialResolver &materials, Resources &resources,
                                           const StagingSteps &steps, const RendererFormat &format) {
    RendererState state;
    const auto &mesh = value.at("mesh");
    if (!mesh.is_null()) {
        const auto key = mesh.get<std::string>();
        require_renderer(!key.empty() && key.size() <= maximum_mesh_key_bytes, format.read_mesh_key);
        require_renderer(bool(resolve), format.mesh_resolver);
        auto found = resources.meshes.find(key);
        if (found == resources.meshes.end()) {
            steps.check();
            auto resource = resolve(key);
            require_renderer(bool(resource), format.unresolved_mesh);
            found = resources.meshes.emplace(key, std::move(resource)).first;
            steps.complete();
        }
        state.mesh = found->second;
    }
    static const nlohmann::json omitted;
    const auto &pose = value.contains("pose") ? value.at("pose") : omitted;
    if (!pose.is_null()) {
        require_renderer(pose.is_array() && state.mesh && pose.size() == state.mesh->rest_pose().world.size(),
                         format.pose);
        state.pose.emplace();
        for (const auto &world : pose)
            state.pose->world.push_back(format.matrix(world));
    }
    state.visible = value.value("visible", state.visible);
    if (value.contains("material_factors")) {
        const auto &factors = value.at("material_factors");
        require_renderer(factors.is_array(), format.material_factors);
        for (const auto &factor : factors) {
            require_renderer(factor.is_array() && factor.size() == 3, format.material_factor);
            state.material_factors.push_back(
                {format.scalar(factor[0]), format.scalar(factor[1]), format.scalar(factor[2])});
        }
    }
    if (value.contains("custom_materials")) {
        const auto &custom = value.at("custom_materials");
        require_renderer(custom.is_array(), format.custom_materials);
        for (const auto &entry : custom) {
            if (entry.is_null()) {
                state.custom_materials.emplace_back();
                continue;
            }
            const auto name = entry.get<std::string>();
            require_renderer(!name.empty() && name.size() <= CustomMaterial::max_name_bytes,
                             format.custom_material_name);
            auto found = resources.materials.find(name);
            if (found == resources.materials.end()) {
                steps.check();
                auto material = materials ? materials(name) : nullptr;
                require_renderer(bool(material), format.unresolved_custom_material);
                require_renderer(material->name() == name, format.renamed_custom_material);
                found = resources.materials.emplace(name, std::move(material)).first;
                steps.complete();
            }
            state.custom_materials.push_back(found->second);
        }
    }
    if (value.contains("primitive_visible"))
        state.primitive_visible = value.at("primitive_visible").get<std::vector<bool>>();
    state.casts_shadows = value.value("casts_shadows", state.casts_shadows);
    const auto &placements = value.contains("placements") ? value.at("placements") : omitted;
    if (!placements.is_null()) {
        require_renderer(placements.is_array() && state.mesh && !state.pose, format.placements);
        std::vector<Mat4> transforms;
        transforms.reserve(placements.size());
        for (const auto &transform : placements)
            transforms.push_back(format.matrix(transform));
        state.placements = MeshPlacements::create(state.mesh, transforms);
    }
    state.visibility_range = decode_visibility_range(
        value.contains("visibility_range") ? value.at("visibility_range") : omitted, format.visibility_range);
    return state;
}
} // namespace anima::detail
