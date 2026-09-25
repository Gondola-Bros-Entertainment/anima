#pragma once
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/interaction_bindings.hpp>
namespace anima {
struct InteractionActor {
    std::shared_ptr<const anima::Asset> asset;
    std::shared_ptr<const MotionRuntime> motion;
    std::map<std::string, anima::InteractionSocket, std::less<>> sockets;
};
struct InteractionSample {
    anima::ActionTime clock;
    std::vector<anima::InteractionFrame> frames;
    struct Contact {
        std::string role, chain;
        float weight{}, error{};
        bool reachable{};
    };
    std::vector<Contact> contacts;
    std::vector<float> attachment_weights;
};

class InteractionRuntime {
  public:
    using Actors = std::map<std::string, InteractionActor, std::less<>>;
    InteractionRuntime(Actors actors, std::string_view document);
    const ActionTimeline &timeline() const;
    const InteractionBindings &bindings() const;
    const std::string &id() const;
    InteractionSample sample(double elapsed, std::optional<double> released,
                             const std::map<std::string, Mat4, std::less<>> &free_worlds) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
