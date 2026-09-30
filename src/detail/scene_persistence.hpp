#pragma once
#include <anima/scene_set.hpp>

namespace anima::detail {
// The caller guards published membership. Loading owns an unpublished complete
// set so failure invalidates every staged identity before component cleanup.
std::string serialize_scene_set(SceneSet &scenes, const MeshName &name, const ComponentCodecs &codecs);
std::unique_ptr<SceneSet> load_scene_set(const StagedSceneSet &staged, const ComponentCodecs &codecs);
// Loads the replacement for member @p key of @p scenes from a scene document, or from the member
// with that namespace in a scene set document, staging the document on the calling thread first.
std::shared_ptr<Scene> load_scene_member(std::string_view document, const SceneSet &scenes, std::string_view key,
                                         const MeshResolver &resolve, const CustomMaterialResolver &materials,
                                         const ComponentCodecs &codecs);
// Loads the replacement for member @p key of @p scenes from the member with that namespace in @p staged, after
// checking every member's component types. Rows in other namespaces resolve by address against @p scenes. The
// unpublished scene is released on failure.
std::shared_ptr<Scene> load_scene_member(const StagedSceneSet &staged, const SceneSet &scenes, std::string_view key,
                                         const ComponentCodecs &codecs);
} // namespace anima::detail
