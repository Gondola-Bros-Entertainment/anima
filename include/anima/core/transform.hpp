#pragma once
#include <anima/core/math.hpp>

#include <cstddef>

namespace anima {
/// Rotation quaternion in XYZW order, the glTF convention: #x, #y and #z are the vector part and #w the scalar part.
///
/// It is a type of its own, so a `std::array<float, 4>`, such as a tangent(), does not convert to it. `Quat{}` is all
/// zeros, which is not a rotation and which unit_quaternion() rejects; identity_rotation is the rotation that changes
/// nothing.
struct Quat {
    float x{}, y{}, z{}, w{};
    /// Component @p i in XYZW order: 0 is #x, 1 #y, 2 #z and 3 #w; an index above 3 also gives #w.
    constexpr float &operator[](std::size_t i) noexcept {
        switch (i) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        default:
            return w;
        }
    }
    /// Component @p i in XYZW order: 0 is #x, 1 #y, 2 #z and 3 #w; an index above 3 also gives #w.
    constexpr const float &operator[](std::size_t i) const noexcept {
        switch (i) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        default:
            return w;
        }
    }
    /// Compares the components with `float` equality, so a NaN component makes quaternions unequal, and `q` and `-q`,
    /// which are the same rotation, are unequal.
    bool operator==(const Quat &) const = default;
};
/// The rotation that changes nothing, `{0, 0, 0, 1}`.
inline constexpr Quat identity_rotation{0, 0, 0, 1};
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
    Quat rotation = identity_rotation;
    Vec3 scale{1, 1, 1};
    /// Compares the components with `float` `==`, so `-0` equals `0` and a NaN component equals nothing; a
    /// quaternion and its negation, the same rotation, differ.
    bool operator==(const Transform &) const = default;
};
/// Column-major matrix that scales, then rotates, then translates (T * R * S), after normalizing
/// the rotation. Throws MathError when the rotation cannot be normalized.
[[nodiscard]] Mat4 matrix(const Transform &transform);
} // namespace anima
