#pragma once
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

/// @file
/// Collision layer constants shared by the 3D and 2D physics modules.
///
/// Header-only, with no backend and no link dependency: anima/physics.hpp and anima/physics2d.hpp
/// both include it. A body takes one collision layer, and a collision mask of type std::uint16_t
/// holds bit `1 << n` for layer `n`.

namespace anima {
/// Number of collision layers. A body's layer is in [0, collision_layer_count), that is [0, 15].
inline constexpr unsigned collision_layer_count = 16;
/// Collision mask with the bit of every layer set, 0xffff.
inline constexpr auto all_collision_layers = static_cast<std::uint16_t>((1U << collision_layer_count) - 1);
/// Returns the collision mask with the bit of each layer in @p layers set; an empty list gives 0,
/// and a repeated layer sets its bit once. Throws `std::invalid_argument` ("Collision layer must be
/// in [0,15]") for a layer of collision_layer_count or more, which in a constant expression is a
/// compile error.
[[nodiscard]] constexpr std::uint16_t collision_layer_mask(std::initializer_list<std::uint8_t> layers) {
    unsigned mask = 0;
    for (const auto layer : layers) {
        if (layer >= collision_layer_count)
            throw std::invalid_argument("Collision layer must be in [0,15]");
        mask |= 1U << layer;
    }
    return static_cast<std::uint16_t>(mask);
}
} // namespace anima
