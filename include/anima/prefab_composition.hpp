#pragma once
#include <anima/prefab.hpp>

namespace anima {
// A bounded tree of concrete prefab instances. Each part keeps its own authored
// object-reference scope; opaque component payloads are never renumbered.
class PrefabComposition {
  public:
    struct Mount {
        std::string part;
        ObjectKey object;
    };
    struct Part {
        std::string key;
        std::string prefab;
        std::optional<Mount> parent; // First part has none; later parts name an earlier part.
        Mat4 placement = identity(); // Multiplied by this prefab's authored root local matrix.
    };

    explicit PrefabComposition(std::vector<Part> parts);
    [[nodiscard]] std::span<const Part> parts() const { return parts_; }
    // Resolves each resource key once per call; all native objects and mounts
    // exist before component decoding. Destination codecs are borrowed only for
    // this call. Failure removes every staged object and its owned resources.
    // The call placement applies to the first part; mounted parts inherit it.
    // Resolvers must be read-only; decoders may attach components only to their
    // supplied object. Arbitrary application callback side effects are not rolled back.
    [[nodiscard]] GameObject instantiate(Scene &scene, const PrefabResolver &resolve, const ComponentCodecs &codecs,
                                         const Mat4 &placement = identity()) const;
    [[nodiscard]] GameObject instantiate(GameObject parent, const PrefabResolver &resolve,
                                         const ComponentCodecs &codecs, const Mat4 &placement = identity()) const;
    [[nodiscard]] std::string serialize() const;
    static PrefabComposition deserialize(std::string_view document);

  private:
    std::vector<Part> parts_;
    GameObject create(Scene &scene, const GameObject *parent, const PrefabResolver &resolve,
                      const ComponentCodecs &codecs, const Mat4 &placement) const;
};
} // namespace anima
