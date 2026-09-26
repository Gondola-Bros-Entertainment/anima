#pragma once
#include <anima/core/math.hpp>

namespace anima {
/// Rotation quaternion in XYZW order, the glTF convention.
using Quat = std::array<float, 4>;
/// Returns @p q scaled to unit length. Throws MathError with MathErrorCode::nonfinite_quaternion
/// or MathErrorCode::zero_quaternion when @p q cannot be normalized.
[[nodiscard]] Quat unit_quaternion(Quat q);
/// Spherical interpolation along the shorter arc from @p a (at @p t = 0) to @p b (at @p t = 1),
/// normalizing both inputs and the result. Throws MathError when an input cannot be normalized or
/// @p t is not finite.
[[nodiscard]] Quat slerp(Quat a, Quat b, float t);
/// Translation, rotation and scale of an object relative to its parent.
struct Transform {
    Vec3 translation{};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};
/// Column-major matrix that scales, then rotates, then translates (T * R * S), after normalizing
/// the rotation. Throws MathError when the rotation cannot be normalized.
[[nodiscard]] Mat4 matrix(const Transform &transform);
} // namespace anima
