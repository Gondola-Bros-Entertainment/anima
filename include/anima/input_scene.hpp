#pragma once
#include <anima/input.hpp>
#include <anima/scene.hpp>
#include <utility>
namespace anima::input {
// Generic action state component; contains no player identity or game commands.
class ActionInput {
  public:
    explicit ActionInput(Map map = {}) : context_(std::move(map)) {}
    Context &context() { return context_; }
    const Context &context() const { return context_; }

  private:
    Context context_;
};
// Configuration only: device bindings are application-selected; physical state,
// focus, edge latches and device ownership are never persisted.
std::string serialize_map(const Map &map);
Map deserialize_map(std::string_view data);
void add_component_codec(ComponentCodecs &codecs);
void begin_frame(Scene &scene);
// Stages all contexts so rejection never partially publishes a scene input event.
// Component enablement gates delivery; the application chooses target scenes.
void dispatch(Scene &scene, const Event &event);
} // namespace anima::input
