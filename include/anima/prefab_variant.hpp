#pragma once
#include <anima/prefab.hpp>

namespace anima {
// Resolves one application-owned asset key to an immutable authored snapshot.
using PrefabResolver = std::function<std::shared_ptr<const Prefab>(std::string_view)>;

class PrefabVariant {
  public:
    // A complete renderer replacement. A null mesh removes the renderer and
    // requires the remaining defaults. A mesh with no pose uses its rest pose;
    // empty override arrays use the mesh defaults.
    struct Renderer {
        std::shared_ptr<const Mesh> mesh;
        std::optional<Pose> pose;
        bool visible = true;
        std::vector<Vec3> material_factors;
        std::vector<bool> primitive_visible;
    };
    struct Override {
        ObjectKey key; // Authored base identity, never an instantiated object's key.
        std::optional<std::string> name;
        std::optional<Mat4> local;
        std::optional<bool> active;
        std::optional<Renderer> renderer;          // Absent inherits the entire base renderer.
        std::vector<ComponentData> set_components; // Upsert complete state by stable codec type.
        std::vector<std::string> remove_components;
    };

    // Owns only authored deltas and immutable mesh resources, never service bindings.
    // Empty override lists inherit the whole base; individual empty overrides reject.
    PrefabVariant(std::string base_key, std::vector<Override> overrides);
    [[nodiscard]] std::string_view base_key() const { return base_key_; }
    [[nodiscard]] std::span<const Override> overrides() const { return overrides_; }
    // Calls the resolver once. The returned Prefab owns a copy of these bindings;
    // the base registry is not inherited. No component codec callbacks execute.
    // Hierarchy and authored keys remain unchanged. Unknown targets/removals reject.
    // Component replacement preserves base order; newly added types append in
    // override order. Resolution changes neither the base nor earlier results.
    [[nodiscard]] Prefab resolve(const PrefabResolver &resolver, ComponentCodecs codecs) const;
    [[nodiscard]] std::string serialize(const MeshName &name) const;
    static PrefabVariant deserialize(std::string_view document, const MeshResolver &resolve);

  private:
    std::string base_key_;
    std::vector<Override> overrides_;
};
} // namespace anima
