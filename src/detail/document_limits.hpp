#pragma once
#include <anima/components.hpp>
#include <cstddef>

// Limits of the scene, prefab, scene set, prefab variant and prefab composition documents, which their readers and
// writers share so that a document one writes the others read. include/anima/prefab.hpp, prefab_variant.hpp,
// prefab_composition.hpp and scene_set.hpp state them.
namespace anima::detail {
// Bytes of a whole document.
inline constexpr std::size_t maximum_document_bytes = 16 * 1024 * 1024;
// Objects of a document, of all members of a scene set together, of all parts of a composition together, and
// overrides of a prefab variant.
inline constexpr std::size_t maximum_document_objects = 65'536;
// Components of one object, and entries in either component list of a variant override.
inline constexpr std::size_t maximum_object_components = 1024;
// Bytes of a mesh key, a prefab resource key, a composition part key or a component type key.
inline constexpr std::size_t maximum_key_bytes = 4096;
static_assert(ComponentCodecs::max_key_bytes == maximum_key_bytes, "A document's component type key is a codec key");
// Members of a scene set document.
inline constexpr std::size_t maximum_scene_set_members = 1024;
// Parts of a prefab composition.
inline constexpr std::size_t maximum_composition_parts = 1024;
} // namespace anima::detail
