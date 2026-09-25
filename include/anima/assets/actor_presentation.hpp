#pragma once
#include <anima/assets/interaction_runtime.hpp>
#include <anima/scene.hpp>
namespace anima {
struct ActorPresentation {
    std::string id;
    anima::Manifest manifest;
    InteractionActor actor;
    std::shared_ptr<const anima::Mesh> render;
    std::set<std::string, std::less<>> capabilities;

    explicit ActorPresentation(const std::filesystem::path &profile,
                               std::optional<std::filesystem::path> manifest_override = {});
};
class ActionSetCatalog {
  public:
    ActionSetCatalog(std::string_view document, const ActionRuntime &actions);
    const std::string &resolve(std::string_view set, std::string_view slot, const Capabilities &capabilities) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
