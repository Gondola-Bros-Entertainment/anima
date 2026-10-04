#pragma once
#include <cstdint>

namespace anima::detail {
// Input limits that the 3D and 2D physics modules share. They bound validation, not precision.
inline constexpr float maximum_vector_component = 1'000'000.F;
inline constexpr double minimum_step_seconds = .000001;
inline constexpr double maximum_step_seconds = .1;
// A body's mass, in caller-consistent units such as kilograms.
inline constexpr float minimum_body_mass = .001F;
inline constexpr float maximum_body_mass = 1'000'000.F;
// Velocity damping per second. Jolt scales a dynamic body's velocities by max(0, 1 - c dt) in each collision step of
// dt seconds, at most 1/60: at a 1/60 s step every coefficient from 60 up stops the body in one step, so a larger one
// would damp no more. Box2D's 1 / (1 + c h) has no such limit, but one bound keeps settings valid in both modules.
inline constexpr float maximum_damping = 60;
// Ray and sweep displacements whose squared length is at or below this are rejected as zero.
inline constexpr float minimum_squared_displacement = 1e-12F;
// Bodies take a layer in [0, collision_layer_count), and each layer's collision mask holds one bit per layer.
inline constexpr unsigned collision_layer_count = 16;
inline constexpr auto all_layers = static_cast<std::uint16_t>((1U << collision_layer_count) - 1);
} // namespace anima::detail
