#pragma once
#include <anima/scene_set.hpp>

namespace anima::detail {
// The caller guards published membership. Loading owns an unpublished complete
// set so failure invalidates every staged identity before component cleanup.
std::string serialize_scene_set(SceneSet &scenes, const MeshName &name, const ComponentCodecs &codecs);
std::unique_ptr<SceneSet> load_scene_set(std::string_view document, const MeshResolver &resolve,
                                         const ComponentCodecs &codecs);
} // namespace anima::detail
