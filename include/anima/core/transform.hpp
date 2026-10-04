#pragma once
#include <anima/core/math.hpp>

#include <cstddef>
#include <optional>

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
/// Hamilton product: the rotation that applies @p b, then @p a, as `matrix(a) * matrix(b)` does for unit quaternions.
/// It does not normalize, so the product of unit quaternions has unit length only up to rounding.
[[nodiscard]] constexpr Quat operator*(const Quat &a, const Quat &b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
/// The conjugate of @p q, `{-x, -y, -z, w}`, which for a unit quaternion is the inverse rotation.
[[nodiscard]] constexpr Quat conjugate(const Quat &q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
/// Rotates @p v by @p q, normalized as unit_quaternion() does, as the matrix() of a Transform with rotation @p q and
/// unit scale does up to rounding. Throws MathError when @p q cannot be normalized.
[[nodiscard]] Vec3 rotate(const Quat &q, Vec3 v);
/// The rotation by @p radians about @p axis, counterclockwise when seen from the tip of @p axis toward the origin (the
/// right-hand rule), as a unit quaternion. @p axis need not have unit length. The result is computed in double and
/// rounded once to `float`. Throws MathError with MathErrorCode::invalid_axis_angle unless @p radians is finite and
/// @p axis is finite and longer than `1e-12`.
[[nodiscard]] Quat axis_angle(Vec3 axis, float radians);
/// The rotation by Euler angles in radians, in Z-X-Y order:
/// `axis_angle(world_up, yaw) * axis_angle({1, 0, 0}, pitch) * axis_angle({0, 0, 1}, roll)`, which rolls about Z, then
/// pitches about X, then yaws about Y.
///
/// Each angle turns counterclockwise seen from the positive end of its axis, so a positive @p pitch raises
/// view_forward and a positive @p yaw turns it toward -X. Unity applies the same Z-X-Y order, but in its left-handed
/// coordinates with +Z forward a positive pitch lowers the view and a positive yaw turns it right. Throws MathError
/// with MathErrorCode::invalid_axis_angle unless every angle is finite.
[[nodiscard]] Quat euler_radians(float pitch, float yaw, float roll);
/// The rotation that turns view_forward (-Z), the direction cameras, audio listeners and GameObject::forward() face,
/// onto @p forward, and +Y onto @p up made perpendicular to @p forward, as a unit quaternion. Neither direction need
/// have unit length. Unity's `Quaternion.LookRotation` turns +Z onto its forward direction instead.
///
/// look_at() builds the inverse of this rotation from the same axes. Throws MathError with
/// MathErrorCode::invalid_look when either direction is not finite or is no longer than `1e-12`, or when the two are
/// parallel: the sine of the angle between them is at most `1e-6`, as for a direction straight up or down with the
/// default world_up, which another @p up resolves.
[[nodiscard]] Quat look_rotation(Vec3 forward, Vec3 up = world_up);
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
/// The translation, rotation and scale whose matrix() is @p affine, or nothing when no T * R * S product gives it.
///
/// The scale is the lengths of the first three columns, with X negated when the upper 3x3 has a negative determinant,
/// so a reflection becomes a negative X scale. The rotation turns each axis onto its column's direction: it is the
/// rotation of the polar decomposition of the upper 3x3 after negating X for a reflection, the rotation nearest to it.
/// Returns nothing when an element is not finite, when the bottom row's summed deviation from (0, 0, 0, 1) is `1e-5`
/// or more, when the upper 3x3's determinant is within `1e-12` of 0, so that an axis has collapsed and the rotation is
/// lost, or when it has shear: the cosine of the angle between two of its columns exceeds `1e-5` in magnitude.
[[nodiscard]] std::optional<Transform> decompose(const Mat4 &affine);
/// Rotation of the polar decomposition of @p transform's upper 3x3, the rotation nearest to it, as a unit quaternion.
/// It keeps no scale or shear, so the rigid frame of a stretched joint is `matrix(Transform{point(transform, {}),
/// affine_rotation(transform), {1, 1, 1}})`.
///
/// Throws `std::invalid_argument` when an element is not finite ("Nonfinite affine transform"), when the bottom row's
/// summed deviation from (0, 0, 0, 1) is `1e-5` or more ("Affine rotation needs an affine transform"), or when the
/// upper 3x3's determinant is below `-1e-12`, a reflection ("Affine rotation needs a transform without reflection"),
/// and MathError with MathErrorCode::collapsed_transform when that determinant is within `1e-12` of 0, so that an axis
/// has collapsed and the transform has no rotation.
[[nodiscard]] Quat affine_rotation(const Mat4 &transform);
} // namespace anima
