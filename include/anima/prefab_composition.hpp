#pragma once
#include <anima/prefab.hpp>

/// @file
/// Prefab compositions: trees of prefab instances mounted on each other's objects. Part of the
/// `anima::assets` target; documents follow the rules in prefab.hpp.

namespace anima {
/// Tree of concrete prefab instances, called parts, each mounted on an object of an earlier part.
///
/// A composition stores part keys, prefab resource keys, mounts and placements, but no scenes,
/// prefabs or codecs. Each part keeps its own object-reference scope: payloads are never
/// rewritten, links cannot cross parts, and two parts of one prefab each link only within
/// themselves. Instantiation may run from component hooks.
class PrefabComposition {
  public:
    /// Mount point of a part.
    struct Mount {
        /// Key of an earlier part.
        std::string part;
        /// Nonzero authored key of the object in that part's prefab.
        ObjectKey object;
    };
    /// One prefab instance of the composition.
    struct Part {
        /// Unique part name, 1 to 4,096 bytes.
        std::string key;
        /// Prefab resource key passed to the PrefabResolver, 1 to 4,096 bytes; parts may share one.
        std::string prefab;
        /// Empty for the first part and only for it; later parts mount on an earlier part.
        std::optional<Mount> parent;
        /// Multiplied by the prefab's authored root local matrix; must be affine.
        Mat4 placement = identity();
    };

    /// Validates @p parts: 1 to 1,024 parts with valid, unique keys, a mount on every part but the
    /// first naming an earlier part and a nonzero object, and affine placements. Throws
    /// `std::invalid_argument` otherwise.
    explicit PrefabComposition(std::vector<Part> parts);
    [[nodiscard]] std::span<const Part> parts() const { return parts_; }
    /// Instantiates every part into @p scene and returns the first part's root, a new scene root
    /// whose local matrix is @p placement times that part's placement and prefab root matrix. Later
    /// parts inherit the result through their mounts.
    ///
    /// Each distinct prefab key is resolved once per call. Mount objects, component types (from
    /// @p codecs, borrowed for this call) and the limit of 65,536 objects in total are checked before
    /// any object is created, and every part's objects exist before any decoder runs. Instances get
    /// fresh keys as with Prefab::instantiate. On failure every object created is destroyed; resolvers
    /// must only read, and other side effects of application callbacks are not undone.
    [[nodiscard]] GameObject instantiate(Scene &scene, const PrefabResolver &resolve, const ComponentCodecs &codecs,
                                         const Mat4 &placement = identity()) const;
    /// Instantiates every part under @p parent, as the Scene overload does.
    [[nodiscard]] GameObject instantiate(GameObject parent, const PrefabResolver &resolve,
                                         const ComponentCodecs &codecs, const Mat4 &placement = identity()) const;
    /// Writes an `anima.prefab-composition` version 1 document: exactly `version`, `kind` and
    /// `parts`, each part with exactly `key`, `prefab`, `parent` (null, or exactly `part` and
    /// `object`) and `placement` (16 numbers).
    [[nodiscard]] std::string serialize() const;
    /// Reads an `anima.prefab-composition` version 1 document and validates it as the constructor
    /// does.
    static PrefabComposition deserialize(std::string_view document);

  private:
    std::vector<Part> parts_;
    GameObject create(Scene &scene, const GameObject *parent, const PrefabResolver &resolve,
                      const ComponentCodecs &codecs, const Mat4 &placement) const;
};
} // namespace anima
