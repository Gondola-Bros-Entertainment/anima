#include <anima/core/transform.hpp>

#include "../detail/rotation_matrix.hpp"

#include <anima/core/math_error.hpp>

namespace anima {
namespace {
// Squared norms below this are treated as zero: normalizing them would amplify rounding noise.
constexpr double minimum_squared_norm = 1e-20;
// Above this cosine the arc is too short for sin(angle) to divide by, so slerp blends linearly.
constexpr float linear_blend_cosine = 0.9995F;
bool finite(const Mat4 &m) {
    return std::ranges::all_of(m, [](float x) { return std::isfinite(x); });
}
// Whether the bottom row of @p m deviates from (0, 0, 0, 1) by less than 1e-5 in sum.
bool affine_bottom_row(const Mat4 &m) {
    constexpr float affine_tolerance = 1e-5F;
    return std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1) < affine_tolerance;
}
} // namespace
Quat unit_quaternion(Quat q) {
    double sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(q[i]))
            throw MathError(MathErrorCode::nonfinite_quaternion);
        sum += double(q[i]) * q[i];
    }
    if (sum < minimum_squared_norm)
        throw MathError(MathErrorCode::zero_quaternion);
    const auto scale = static_cast<float>(1 / std::sqrt(sum));
    return {q.x * scale, q.y * scale, q.z * scale, q.w * scale};
}
Quat slerp(Quat a, Quat b, float t) {
    a = unit_quaternion(a);
    b = unit_quaternion(b);
    float cosine = 0;
    for (unsigned i = 0; i < 4; ++i)
        cosine += a[i] * b[i];
    if (cosine < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, 0.F, 1.F);
    float wa = 1 - t, wb = t;
    if (cosine < linear_blend_cosine) {
        const auto angle = std::acos(cosine), denominator = std::sin(angle);
        wa = std::sin((1 - t) * angle) / denominator;
        wb = std::sin(t * angle) / denominator;
    }
    Quat result{};
    for (unsigned i = 0; i < 4; ++i)
        result[i] = wa * a[i] + wb * b[i];
    return unit_quaternion(result);
}
Mat4 matrix(const Transform &t) {
    const auto q = unit_quaternion(t.rotation);
    const auto [x, y, z, w] = q;
    return {(1 - 2 * (y * y + z * z)) * t.scale.x,
            2 * (x * y + z * w) * t.scale.x,
            2 * (x * z - y * w) * t.scale.x,
            0,
            2 * (x * y - z * w) * t.scale.y,
            (1 - 2 * (x * x + z * z)) * t.scale.y,
            2 * (y * z + x * w) * t.scale.y,
            0,
            2 * (x * z + y * w) * t.scale.z,
            2 * (y * z - x * w) * t.scale.z,
            (1 - 2 * (x * x + y * y)) * t.scale.z,
            0,
            t.translation.x,
            t.translation.y,
            t.translation.z,
            1};
}
Vec3 rotate(const Quat &q, Vec3 v) {
    const auto [x, y, z, w] = unit_quaternion(q);
    // v + 2w (u x v) + 2 u x (u x v) for the vector part u.
    const Vec3 u{x, y, z};
    const auto t = cross(u, v) * 2;
    return v + t * w + cross(u, t);
}
Quat axis_angle(Vec3 axis, float radians) {
    const double length = detail::length_in_double(axis);
    if (!(length > detail::minimum_direction_length && std::isfinite(length) && std::isfinite(radians)))
        throw MathError(MathErrorCode::invalid_axis_angle);
    const double half = double(radians) / 2, scale = std::sin(half) / length;
    return {float(axis.x * scale), float(axis.y * scale), float(axis.z * scale), float(std::cos(half))};
}
Quat euler_radians(float pitch, float yaw, float roll) {
    return axis_angle(world_up, yaw) * axis_angle({1, 0, 0}, pitch) * axis_angle({0, 0, 1}, roll);
}
Quat look_rotation(Vec3 forward, Vec3 up) {
    const auto [right, view_up, ahead] = detail::look_axes(forward, up);
    // The columns are where +X, +Y and +Z go; +Z points away from the view.
    const detail::Matrix3 rotation{
        {{right.x, view_up.x, -ahead.x}, {right.y, view_up.y, -ahead.y}, {right.z, view_up.z, -ahead.z}}};
    // Orthonormal columns keep Shepperd's pivot at about 2 or more, so a quaternion always exists.
    return detail::rotation_quaternion(rotation).value();
}
std::optional<Transform> decompose(const Mat4 &affine) {
    // Columns whose angle has a cosine above this in magnitude are sheared.
    constexpr double maximum_shear_cosine = 1e-5;
    if (!finite(affine) || !affine_bottom_row(affine))
        return std::nullopt;
    const auto linear = detail::upper(affine);
    if (detail::collapsed(linear))
        return std::nullopt;
    detail::Matrix3 columns{};
    for (unsigned c = 0; c < 3; ++c) {
        const double size = std::hypot(linear[0][c], linear[1][c], linear[2][c]);
        for (unsigned r = 0; r < 3; ++r)
            columns[c][r] = linear[r][c] / size;
    }
    for (unsigned a = 0; a < 3; ++a) {
        const auto b = (a + 1) % 3;
        const double cosine =
            columns[a][0] * columns[b][0] + columns[a][1] * columns[b][1] + columns[a][2] * columns[b][2];
        if (std::abs(cosine) > maximum_shear_cosine)
            return std::nullopt;
    }
    return Transform{translation_of(affine), detail::orientation(linear), detail::axis_scale(linear)};
}
Quat affine_rotation(const Mat4 &transform) {
    if (!finite(transform))
        throw std::invalid_argument("Nonfinite affine transform");
    if (!affine_bottom_row(transform))
        throw std::invalid_argument("Affine rotation needs an affine transform");
    const auto linear = detail::upper(transform);
    // Below the negative of the collapse threshold, the transform reflects.
    if (detail::determinant(linear) < -detail::collapse_determinant)
        throw std::invalid_argument("Affine rotation needs a transform without reflection");
    if (detail::collapsed(linear))
        throw MathError(MathErrorCode::collapsed_transform);
    return detail::polar(linear).rotation;
}
} // namespace anima
