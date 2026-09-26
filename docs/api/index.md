# Anima C++ reference

Anima is a modular C++20 game engine. Each public declaration documents its
contract: ownership and lifetime, threading, units and ranges, failures and their
exception types, and persistence formats. Private implementation headers and
third-party libraries are excluded.

Entry points:

- anima::Scene, anima::GameObject and anima::SceneSet own objects, components and
  additive scenes; anima::Prefab builds object assemblies, and
  anima::ComponentCodecs persists components.
- anima::FixedStepClock drives fixed-step simulation.
- anima::physics::World and anima::physics2d::World simulate rigid bodies.
- anima::VulkanRenderer draws the selected scenes on the desktop.
- anima::Camera and anima::CameraView select views, and anima::view_matrix resolves
  them for the renderer.

Build instructions, module targets, architecture and conventions are in the
repository's [README](https://github.com/Gondola-Bros-Entertainment/anima#readme).
