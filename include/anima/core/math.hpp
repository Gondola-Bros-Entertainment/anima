#pragma once
#include <anima/core/math_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <type_traits>

/// @file
/// Vector and matrix math in the engine's conventions. Part of the `anima::core` target.
///
/// World space is right-handed with +Y up, and cameras and audio listeners face local -Z. Matrices
/// are column-major and act on column vectors, the glTF convention. Projections use Vulkan clip
/// space with reversed depth: framebuffer Y points down and depth runs from 1 at the near plane to 0
/// at the far plane, so that a floating-point depth buffer keeps its precision at a distance.

namespace anima {
/// Three-component float vector for positions, directions and scales.
struct Vec3 {
    float x{}, y{}, z{};
    /// Compares the components with `float` `==`, so `-0` equals `0` and a NaN component equals nothing.
    bool operator==(const Vec3 &) const = default;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
/// World up direction, +Y.
inline constexpr Vec3 world_up{0, 1, 0};
/// Local forward direction of cameras and audio listeners, -Z.
inline constexpr Vec3 view_forward{0, 0, -1};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
namespace detail {
/// A vector no longer than this has no direction: normalized() falls back to world_up for it.
inline constexpr double minimum_direction_length = 1e-12;
/// The length of @p v, squared and summed in double, where no finite `float` component overflows or underflows.
inline double length_in_double(Vec3 v) { return std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z); }
} // namespace detail
/// Returns the length of @p v, computed in double and rounded once to `float`, so a large component does not overflow
/// when squared. A length beyond the `float` range, or an infinite component, gives infinity, and a NaN component
/// gives NaN.
inline float length(Vec3 v) { return float(detail::length_in_double(v)); }
/// Returns @p v scaled to unit length, or world_up when its length is NaN, infinite or at most `1e-12`. The length
/// and the division are computed in double, so any vector of finite `float` components longer than `1e-12`
/// normalizes.
inline Vec3 normalized(Vec3 v) {
    const double n = detail::length_in_double(v);
    if (!(n > detail::minimum_direction_length) || !std::isfinite(n))
        return world_up;
    return {float(v.x / n), float(v.y / n), float(v.z / n)};
}
/// 4x4 matrix stored column-major for column vectors: row `r` of column `c` is element `c * 4 + r`.
///
/// It is a type of its own in namespace anima, so argument-dependent lookup finds its operators from any namespace.
/// Brace elision fills #elements from a flat list, as in `Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}`, and
/// `Mat4{}` is all zeros. It holds its 16 `float`s contiguously with nothing else, and is standard-layout and trivially
/// copyable, so its bytes are the column-major elements a GPU buffer expects.
struct Mat4 {
    /// The elements, column-major.
    std::array<float, 16> elements{};
    /// Element @p i, which must be less than 16; as `std::array`, it does not check the index.
    constexpr float &operator[](std::size_t i) noexcept { return elements[i]; }
    /// Element @p i, which must be less than 16; as `std::array`, it does not check the index.
    constexpr const float &operator[](std::size_t i) const noexcept { return elements[i]; }
    /// The first of the 16 contiguous elements.
    constexpr float *data() noexcept { return elements.data(); }
    /// The first of the 16 contiguous elements.
    constexpr const float *data() const noexcept { return elements.data(); }
    /// The element count, 16.
    static constexpr std::size_t size() noexcept { return 16; }
    /// The first element, for iteration in column-major order.
    constexpr float *begin() noexcept { return elements.data(); }
    /// The first element, for iteration in column-major order.
    constexpr const float *begin() const noexcept { return elements.data(); }
    /// One past the last element.
    constexpr float *end() noexcept { return elements.data() + size(); }
    /// One past the last element.
    constexpr const float *end() const noexcept { return elements.data() + size(); }
    /// Compares the elements in order with `float` equality, so a NaN element makes matrices unequal and 0 equals -0.
    bool operator==(const Mat4 &) const = default;
};
static_assert(sizeof(Mat4) == Mat4::size() * sizeof(float) && std::is_standard_layout_v<Mat4> &&
                  std::is_trivially_copyable_v<Mat4>,
              "Mat4 must be 16 contiguous floats for GPU copies");
inline Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
/// Column 0: the X axis of @p m, including its scale.
inline Vec3 axis_x(const Mat4 &m) { return {m[0], m[1], m[2]}; }
/// Column 1: the Y axis of @p m, including its scale.
inline Vec3 axis_y(const Mat4 &m) { return {m[4], m[5], m[6]}; }
/// Column 2: the Z axis of @p m, including its scale.
inline Vec3 axis_z(const Mat4 &m) { return {m[8], m[9], m[10]}; }
/// Column 3: the translation of @p m.
inline Vec3 translation_of(const Mat4 &m) { return {m[12], m[13], m[14]}; }
/// Sets the translation column of @p m to @p t, leaving its linear part unchanged.
inline void set_translation(Mat4 &m, Vec3 t) {
    m[12] = t.x;
    m[13] = t.y;
    m[14] = t.z;
}
namespace detail {
/// The inverse of @p matrix in double precision, column-major; inverse() states its failures, except the overflow of a
/// `float` element, which it alone checks.
inline std::array<double, 16> inverse_in_double(const Mat4 &matrix) {
    double rows[4][8]{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            if (!std::isfinite(matrix[c * 4 + r]))
                throw MathError(MathErrorCode::nonfinite_matrix);
            rows[r][c] = matrix[c * 4 + r];
            rows[r][c + 4] = r == c ? 1 : 0;
        }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; ++r)
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c]))
                pivot = r;
        if (rows[pivot][c] == 0)
            throw MathError(MathErrorCode::singular_matrix);
        for (unsigned k = 0; k < 8; ++k)
            std::swap(rows[pivot][k], rows[c][k]);
        const double scale = rows[c][c];
        for (unsigned k = 0; k < 8; ++k)
            rows[c][k] /= scale;
        for (unsigned r = 0; r < 4; ++r)
            if (r != c) {
                const double f = rows[r][c];
                for (unsigned k = 0; k < 8; ++k)
                    rows[r][k] -= f * rows[c][k];
            }
    }
    std::array<double, 16> result{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            result[c * 4 + r] = rows[r][c + 4];
    return result;
}
} // namespace detail
/// Returns the inverse of @p matrix, computed in double precision. Throws MathError with
/// MathErrorCode::nonfinite_matrix for a nonfinite element, MathErrorCode::singular_matrix when
/// elimination meets a zero pivot, or MathErrorCode::inverse_overflow when a result element is not
/// a finite `float`.
inline Mat4 inverse(const Mat4 &matrix) {
    const auto exact = detail::inverse_in_double(matrix);
    Mat4 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<float>(exact[i]);
        if (!std::isfinite(result[i]))
            throw MathError(MathErrorCode::inverse_overflow);
    }
    return result;
}
/// Matrix product; applied to a column vector, `a * b` applies @p b first.
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 result{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned k = 0; k < 4; ++k)
                result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return result;
}
/// Transforms point @p v by @p m as an affine matrix, without a perspective divide.
inline Vec3 point(const Mat4 &m, Vec3 v) {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
}
/// Transforms normal @p n by the inverse transpose of the upper 3x3 of @p m, then normalizes it
/// (see normalized()).
///
/// It uses the cofactor matrix, the inverse transpose scaled by the determinant, with the
/// determinant's sign, which keeps the direction and stays defined for a singular 3x3: a
/// transform that collapses one axis, such as a joint scaled to zero along it, gives the normal
/// of the flattened surface, and one that collapses every axis gives normalized()'s fallback.
inline Vec3 normal(const Mat4 &m, Vec3 n) {
    const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
    const auto cofactor = cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z;
    return normalized(dot(a, cross(b, c)) < 0 ? -cofactor : cofactor);
}
/// Transforms tangent @p t, a direction in XYZ with a handedness sign in W, by the upper 3x3 of
/// @p m without normalizing it, negating W when that 3x3 has a negative determinant. A W of 0
/// marks a missing tangent and returns all zeros.
inline std::array<float, 4> tangent(const Mat4 &m, const std::array<float, 4> &t) {
    if (t[3] == 0)
        return {};
    const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
    const auto v = a * t[0] + b * t[1] + c * t[2];
    return {v.x, v.y, v.z, t[3] * (dot(a, cross(b, c)) < 0 ? -1.F : 1.F)};
}
namespace detail {
/// The unit right, up and forward axes, in that order, of a view along @p forward with @p up as its up reference:
/// forward is @p forward normalized, right is `forward x up` normalized, and up is `right x forward`. Throws as
/// look_at() states.
inline std::array<Vec3, 3> look_axes(Vec3 forward, Vec3 up) {
    // The sine of the angle between the directions must exceed this, so that their cross product has a direction.
    constexpr float minimum_sine = 1e-6F;
    const auto usable = [](Vec3 v) {
        const double n = length_in_double(v);
        return n > minimum_direction_length && std::isfinite(n);
    };
    if (!usable(forward) || !usable(up))
        throw MathError(MathErrorCode::invalid_look);
    const auto ahead = normalized(forward), side = cross(ahead, normalized(up));
    if (!(length(side) > minimum_sine))
        throw MathError(MathErrorCode::invalid_look);
    const auto right = normalized(side);
    return {right, cross(right, ahead), ahead};
}
} // namespace detail
/// View matrix for an eye at @p eye looking at @p target, with @p up as the up reference. View space has +X right, +Y
/// up and the view direction along -Z; its +Y is @p up made perpendicular to the view direction, so @p up need not be
/// a unit vector. Its rotation is the inverse of the one that look_rotation() gives for `target - eye` and @p up.
///
/// Throws MathError with MathErrorCode::invalid_look when `target - eye` or @p up is not finite or is no longer than
/// `1e-12`, or when the two are parallel: the sine of the angle between them is at most `1e-6`, as for a view straight
/// up or down with the default world_up, which another @p up resolves.
[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = world_up) {
    const auto [right, view_up, forward] = detail::look_axes(target - eye, up);
    return {right.x, view_up.x, -forward.x, 0, right.y,          view_up.y,          -forward.y,        0,
            right.z, view_up.z, -forward.z, 0, -dot(right, eye), -dot(view_up, eye), dot(forward, eye), 1};
}
/// Perspective projection with a full vertical field of view of @p vertical_fov_radians and a width of @p aspect
/// times its height, for view space looking down -Z, in Vulkan clip space with reversed depth: 1 at @p near_plane and 0
/// at @p far_plane. Camera::projection() projects a perspective Camera with it, passing
/// CameraSettings::vertical_fov_degrees converted to radians and rounded to `float`. Each element is computed in double
/// and rounded once to `float`, so one too large for `float`, as from a tiny @p aspect or field of view, is infinite.
/// Throws MathError with MathErrorCode::invalid_frustum unless every argument is finite,
/// `0 < vertical_fov_radians < pi`, `aspect > 0` and `0 < near_plane < far_plane`; `std::numbers::pi_v<float>` exceeds
/// pi.
[[nodiscard]] inline Mat4 perspective(float vertical_fov_radians, float aspect, float near_plane, float far_plane) {
    if (!(std::isfinite(vertical_fov_radians) && std::isfinite(aspect) && std::isfinite(near_plane) &&
          std::isfinite(far_plane) && vertical_fov_radians > 0 && vertical_fov_radians < std::numbers::pi &&
          aspect > 0 && near_plane > 0 && near_plane < far_plane))
        throw MathError(MathErrorCode::invalid_frustum);
    const double f = 1 / std::tan(double(vertical_fov_radians) / 2), depth = double(far_plane) - near_plane;
    Mat4 result{};
    result[0] = float(f / aspect);
    result[5] = float(-f); // Framebuffer Y points down.
    result[10] = float(near_plane / depth);
    result[11] = -1;
    result[14] = float(double(near_plane) * far_plane / depth);
    return result;
}
/// Orthographic projection of a box @p height units high and `aspect * height` wide, centered on the view axis, for
/// view space looking down -Z, in Vulkan clip space with reversed depth: 1 at @p near_plane and 0 at @p far_plane,
/// linear in between. It is the orthographic counterpart of perspective(), as `glm::ortho` is of `glm::perspective`,
/// and the projection of an orthographic Camera; a matrix built by hand for forward depth makes a reversed-depth
/// renderer draw farther surfaces over nearer ones. Each element is computed in double and rounded once to `float`, so
/// one too large for `float`, as from a tiny @p aspect, is infinite. Throws MathError with
/// MathErrorCode::invalid_orthographic unless every argument is finite, `aspect > 0`, `height > 0` and
/// `near_plane < far_plane`; either plane may lie at or behind the eye.
inline Mat4 orthographic(float aspect, float height, float near_plane, float far_plane) {
    if (!(std::isfinite(aspect) && std::isfinite(height) && std::isfinite(near_plane) && std::isfinite(far_plane) &&
          aspect > 0 && height > 0 && near_plane < far_plane))
        throw MathError(MathErrorCode::invalid_orthographic);
    const double depth = double(far_plane) - near_plane;
    Mat4 result{};
    result[0] = float(2 / (double(height) * aspect));
    result[5] = float(-2 / double(height)); // Framebuffer Y points down.
    result[10] = float(1 / depth);
    result[14] = float(far_plane / depth);
    result[15] = 1;
    return result;
}
/// Recovers the viewer from view-projection matrix @p vp, which uses reversed depth: the eye position
/// with W = 1 for a perspective projection, or the unit direction toward an orthographic camera with
/// W = 0.
///
/// It solves `vp * x = (0, 0, 1, 0)`, so callers need not supply the camera position separately. An
/// orthographic matrix with forward depth gives the direction away from its camera.
/// Throws MathError with MathErrorCode::nonfinite_projection for a nonfinite element or
/// MathErrorCode::singular_projection when elimination meets a zero pivot, and
/// `std::invalid_argument` when the result is not a finite `float`.
inline std::array<float, 4> view_origin(const Mat4 &vp) {
    double rows[4][5]{};
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            if (!std::isfinite(vp[c * 4 + r]))
                throw MathError(MathErrorCode::nonfinite_projection);
            rows[r][c] = vp[c * 4 + r];
        }
        rows[r][4] = r == 2 ? 1 : 0;
    }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; ++r)
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c]))
                pivot = r;
        if (rows[pivot][c] == 0)
            throw MathError(MathErrorCode::singular_projection);
        for (unsigned k = 0; k < 5; ++k)
            std::swap(rows[c][k], rows[pivot][k]);
        const double divisor = rows[c][c];
        for (unsigned k = c; k < 5; ++k)
            rows[c][k] /= divisor;
        for (unsigned r = 0; r < 4; ++r)
            if (r != c) {
                const double factor = rows[r][c];
                for (unsigned k = c; k < 5; ++k)
                    rows[r][k] -= factor * rows[c][k];
            }
    }
    const bool perspective_view = rows[3][4] != 0;
    // Reversed depth grows toward the camera, so an orthographic solution already points at it.
    const double divisor = perspective_view ? rows[3][4] : std::hypot(rows[0][4], rows[1][4], rows[2][4]);
    std::array<float, 4> result{};
    for (unsigned r = 0; r < 3; ++r) {
        result[r] = static_cast<float>(rows[r][4] / divisor);
        if (!std::isfinite(result[r]))
            throw std::invalid_argument("View origin exceeds finite range");
    }
    result[3] = perspective_view ? 1.F : 0.F;
    return result;
}
/// Camera orbiting #target at #distance; angles are in radians.
struct OrbitCamera {
    /// Point the camera orbits and looks at.
    Vec3 target{};
    /// Radius of the framed bounds; the zoom limits and clip planes scale with it.
    float radius = 1;
    /// Horizontal angle around world_up; 0 places the camera on the +Z side of #target.
    float yaw = 0.3F;
    /// Elevation angle; positive values raise the camera.
    float pitch = 0.12F;
    /// Distance from #target.
    float distance = 3;
    /// Frames the box from @p minimum to @p maximum: targets its center, sets #radius to half its
    /// diagonal (at least 0.01), restores the default #yaw and #pitch and sets #distance to three
    /// radii.
    void frame(Vec3 minimum, Vec3 maximum) {
        target = (minimum + maximum) * 0.5F;
        radius = std::max(length(maximum - minimum) * 0.5F, 0.01F);
        yaw = 0.3F;
        pitch = 0.12F;
        distance = radius * 3;
    }
    /// Adds @p horizontal to #yaw, wrapped to [-pi, pi], and @p vertical to #pitch, clamped to
    /// [-1.4, 1.4].
    void orbit(float horizontal, float vertical) {
        yaw = std::remainder(yaw + horizontal, 6.28318530718F);
        pitch = std::clamp(pitch + vertical, -1.4F, 1.4F);
    }
    /// Scales #distance by `exp(-0.12 * amount)`, so a positive @p amount moves closer, keeping it
    /// between 1.2 and 15 times #radius.
    void zoom(float amount) { distance = std::clamp(distance * std::exp(-amount * 0.12F), radius * 1.2F, radius * 15); }
    /// World position of the camera.
    [[nodiscard]] Vec3 position() const {
        return target +
               Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
    }
    /// Horizontal unit direction from the camera toward #target; it depends only on #yaw.
    [[nodiscard]] Vec3 horizontal_forward() const { return {-std::sin(yaw), 0, -std::cos(yaw)}; }
    /// Horizontal unit direction to the camera's right; it depends only on #yaw.
    [[nodiscard]] Vec3 horizontal_right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
    /// View-projection matrix: perspective() with a vertical field of view of `std::numbers::pi_v<float> / 4` radians
    /// (45 degrees) and near and far planes at 0.01 and 50 times #radius, applied to look_at() from position() toward
    /// #target. Throws MathError with MathErrorCode::invalid_frustum when perspective() rejects those arguments, as for
    /// an @p aspect or #radius that is not finite and positive, or with MathErrorCode::invalid_look when look_at()
    /// rejects position() and #target, as for a #distance of 0 or a #pitch of
    /// `std::numbers::pi_v<float> / 2`.
    [[nodiscard]] Mat4 matrix(float aspect) const {
        return perspective(std::numbers::pi_v<float> / 4, aspect, radius * 0.01F, radius * 50) *
               look_at(position(), target);
    }
};
} // namespace anima
