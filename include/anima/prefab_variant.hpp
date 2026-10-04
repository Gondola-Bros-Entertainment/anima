#pragma once
#include <anima/prefab.hpp>

/// @file
/// Prefab variants: typed overrides of a base prefab that is resolved by key. Part of the
/// `anima::assets` target; documents follow the rules in prefab.hpp.

namespace anima {
/// Authored overrides of an existing prefab's objects, resolved into an ordinary Prefab.
///
/// A variant owns its overrides and shared meshes but no scenes, components or service bindings. It
/// can rename objects, replace their local matrix, activation or whole renderer, and set or remove
/// component payloads; it never adds, deletes, reparents or rekeys objects.
class PrefabVariant {
  public:
    /// Changes to one base object; absent fields inherit from the base.
    struct Override {
        /// Nonzero authored key of the base object, never the key of an instantiated object.
        ObjectKey key;
        std::optional<std::string> name;
        std::optional<Mat4> local;
        std::optional<bool> active;
        /// Absent inherits the whole base renderer, including later changes to the base; present
        /// replaces every renderer field together, and a null RendererState::mesh removes the renderer.
        std::optional<RendererState> renderer;
        /// Complete payloads by codec type key: each replaces the base component of its type in
        /// place, or is appended in this order when the base has none.
        std::vector<ComponentData> set_components;
        /// Type keys of base components to remove; each must exist in the base.
        std::vector<std::string> remove_components;
    };

    /// Validates @p overrides of the prefab with resource key @p base_key.
    ///
    /// Throws `std::invalid_argument` when @p base_key or a component type key is empty or over 4,096
    /// bytes, or for more than 65,536 overrides, a null or repeated object key, an override that
    /// changes nothing, more than 1,024 entries in either component list, a type key used twice in
    /// one override, or invalid matrices or renderer data. An empty list inherits the whole base.
    PrefabVariant(std::string base_key, std::vector<Override> overrides);
    /// Resource key of the base prefab.
    [[nodiscard]] std::string_view base_key() const { return base_key_; }
    [[nodiscard]] std::span<const Override> overrides() const { return overrides_; }
    /// Resolves the base once through @p resolver, applies the overrides to a copy of its nodes and
    /// returns a Prefab that keeps @p codecs; the base's own codecs are not inherited.
    ///
    /// Within each override, removals apply before sets. Hierarchy, node order and authored keys are
    /// unchanged, so linked payloads keep the base's keys and remap per instance as usual. No codec
    /// callback runs, so a malformed payload fails only when instantiated. Throws
    /// `std::invalid_argument` when the base cannot be resolved, an override targets a missing
    /// object or removes a missing component, or the result is not a valid Prefab. The base and
    /// earlier results are unchanged.
    [[nodiscard]] Prefab resolve(const PrefabResolver &resolver, ComponentCodecs codecs) const;
    /// Writes an `anima.prefab-variant` version 2 document.
    ///
    /// It has exactly `version` (`2`), `kind`, `base` and `overrides`. Each override has exactly
    /// `key`, `name`, `local` (16 numbers), `active` and `renderer`, each null to inherit, and
    /// `set_components` (objects with exactly `type`, `state` and `enabled`) and
    /// `remove_components` (type keys), each possibly empty. A renderer has exactly `mesh`, `pose`,
    /// `visible`, `material_factors`, `custom_materials`, `primitive_visible`, `casts_shadows`,
    /// `placements` and `visibility_range`, with the rules of scene objects (serialize_scene).
    [[nodiscard]] std::string serialize(const MeshName &name) const;
    /// Reads an `anima.prefab-variant` version 2 document, resolving custom material names through
    /// @p materials, and validates it as the constructor does.
    static PrefabVariant deserialize(std::string_view document, const MeshResolver &resolve,
                                     const CustomMaterialResolver &materials = {});

  private:
    std::string base_key_;
    std::vector<Override> overrides_;
};
} // namespace anima
