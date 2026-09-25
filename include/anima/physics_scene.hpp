#pragma once
#include <anima/physics.hpp>
#include <anima/scene.hpp>

namespace anima::physics {
// Owns one body's lifetime. The GameObject's world transform must be rigid
// (unit scale, no shear). Dynamic bodies must remain scene roots.
class RigidBody {
  public:
    RigidBody(GameObject object, World &world, BodySettings settings = {});
    ~RigidBody();
    RigidBody(const RigidBody &) = delete;
    RigidBody &operator=(const RigidBody &) = delete;
    [[nodiscard]] Body body() const { return body_; }
    [[nodiscard]] const BodySettings &settings() const { return settings_; }

  private:
    BodySettings settings_;
    Body body_;
};
// Validate/synchronize the complete selection, step the shared world once, then
// publish dynamic poses. Does not invoke component hooks: call fixed_update on
// the Scene/SceneSet once before stepping either physics backend. Every binding
// of this backend must belong to this world (standalone bodies may share it).
void step(Scene &scene, World &world, double seconds);
void step(SceneSet &scenes, World &world, double seconds);
// Explicit codec registration, with a checked non-owning world binding.
void add_component_codec(ComponentCodecs &codecs, World &world);
} // namespace anima::physics
