#pragma once
#include <anima/core/math.hpp>
#include <string>
#include <string_view>

namespace anima::detail {
// Camera and directional-light orientation share this contract: finite scene
// axes may carry positive scale, but must remain orthogonal and right-handed.
// The axis floor rejects nearly singular poses; the tolerance permits float
// roundoff from composed transforms without accepting authored shear.
inline std::array<Vec3, 3> scene_orientation(const Mat4 &world, std::string_view component) {
    constexpr float minimum_axis_length = 1e-4F, axis_tolerance = 1e-4F;
    std::array<Vec3, 3> axes;
    for (unsigned i = 0; i < axes.size(); ++i) {
        const double size = std::hypot(double(world[i * 4]), double(world[i * 4 + 1]), double(world[i * 4 + 2]));
        if (size < minimum_axis_length)
            throw std::invalid_argument(std::string(component) + " world axes must be nonzero");
        axes[i] = {float(world[i * 4] / size), float(world[i * 4 + 1] / size), float(world[i * 4 + 2] / size)};
    }
    if (std::abs(dot(axes[0], axes[1])) > axis_tolerance || std::abs(dot(axes[0], axes[2])) > axis_tolerance ||
        std::abs(dot(axes[1], axes[2])) > axis_tolerance || dot(cross(axes[0], axes[1]), axes[2]) < 1 - axis_tolerance)
        throw std::invalid_argument(std::string(component) + " world axes must be orthogonal and right-handed");
    return axes;
}
} // namespace anima::detail
