#pragma once
#include <anima/prefab.hpp>

namespace anima::detail {
// Creates native data only, returning objects in authored node order. On failure
// it removes its own objects; after success the caller owns staging rollback.
std::vector<GameObject> instantiate_prefab_nodes(Scene &scene, std::span<const Prefab::Node> nodes,
                                                 const GameObject *parent, const Mat4 &placement,
                                                 bool preserve_keys = false);
// Restores one authored reference scope after all required native objects exist.
// The caller handles rollback if a decoder fails.
void restore_prefab_components(std::span<const GameObject> objects, std::span<const Prefab::Node> nodes,
                               const ComponentCodecs &codecs);
void destroy_prefab_objects(std::span<const GameObject> objects);
} // namespace anima::detail
