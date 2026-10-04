#pragma once
#include <algorithm>
#include <anima/core/math_error.hpp>
#include <anima/core/transform.hpp>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace anima::detail {
// A 3x3 matrix in double precision, indexed [row][column].
using Matrix3 = std::array<std::array<double, 3>, 3>;
// Shepperd's method divides by this pivot; smaller ones divide by rounding noise.
inline constexpr double minimum_rotation_pivot = 1e-12;
// A linear part whose determinant is within this of 0 has collapsed at least one axis, as a joint scaled to zero
// does, and has no rotation (MathErrorCode::collapsed_transform).
inline constexpr double collapse_determinant = 1e-12;

// The upper 3x3 of @p m, its linear part.
inline Matrix3 upper(const Mat4 &m) {
    Matrix3 result{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            result[r][c] = m[c * 4 + r];
    return result;
}
inline double determinant(const Matrix3 &m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
inline bool collapsed(const Matrix3 &m) { return std::abs(determinant(m)) <= collapse_determinant; }

// The unit quaternion of rotation matrix @p m by Shepperd's method, which divides by the largest of the trace
// and the diagonal terms for stability. Returns nothing when @p m is too far from a rotation for that pivot.
inline std::optional<Quat> rotation_quaternion(const Matrix3 &m) {
    Quat q{};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const double s = 2 * std::sqrt(trace + 1);
        q = {static_cast<float>((m[2][1] - m[1][2]) / s), static_cast<float>((m[0][2] - m[2][0]) / s),
             static_cast<float>((m[1][0] - m[0][1]) / s), static_cast<float>(s / 4)};
    } else {
        unsigned i = 0;
        if (m[1][1] > m[i][i])
            i = 1;
        if (m[2][2] > m[i][i])
            i = 2;
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        const double s = 2 * std::sqrt(std::max(0., 1 + m[i][i] - m[j][j] - m[k][k]));
        if (!(s > minimum_rotation_pivot))
            return std::nullopt;
        q[i] = static_cast<float>(s / 4);
        q[j] = static_cast<float>((m[j][i] + m[i][j]) / s);
        q[k] = static_cast<float>((m[k][i] + m[i][k]) / s);
        q[3] = static_cast<float>((m[k][j] - m[j][k]) / s);
    }
    return unit_quaternion(q);
}

// The inverse transpose of @p m. Throws std::invalid_argument when its determinant is not finite or is within 1e-20
// of 0.
inline Matrix3 inverse_transpose(const Matrix3 &m) {
    // The inverse divides by the determinant, so smaller magnitudes count as singular.
    constexpr double singular_determinant = 1e-20;
    const double d = determinant(m);
    if (!(std::isfinite(d) && std::abs(d) > singular_determinant))
        throw std::invalid_argument("Polar decomposition is singular");
    Matrix3 result{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            result[r][c] = (m[(r + 1) % 3][(c + 1) % 3] * m[(r + 2) % 3][(c + 2) % 3] -
                            m[(r + 1) % 3][(c + 2) % 3] * m[(r + 2) % 3][(c + 1) % 3]) /
                           d;
    return result;
}
// The polar decomposition `a = R * S` of a linear part into rotation R and symmetric stretch S, which holds its scale
// and shear.
struct Polar {
    Quat rotation;
    Matrix3 stretch;
};
// The polar decomposition of @p a, which must not reflect, by the scaled Newton iteration; R is the rotation nearest
// to @p a. Throws std::invalid_argument when a step is singular, the iteration does not converge or R has no
// quaternion. Callers reject collapsed matrices first.
inline Polar polar(const Matrix3 &a) {
    constexpr unsigned maximum_iterations = 64;
    // The iteration has converged once a step changes no element by this much.
    constexpr double convergence = 1e-12;
    auto r = a;
    bool converged = false;
    for (unsigned iteration = 0; iteration < maximum_iterations; ++iteration) {
        const auto it = inverse_transpose(r);
        double nr = 0, ni = 0, error = 0;
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                nr += r[i][j] * r[i][j];
                ni += it[i][j] * it[i][j];
            }
        const auto gamma = std::pow(ni / nr, .25);
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                const double next = .5 * (gamma * r[i][j] + it[i][j] / gamma);
                error = std::max(error, std::abs(next - r[i][j]));
                r[i][j] = next;
            }
        if (error < convergence) {
            converged = true;
            break;
        }
    }
    if (!converged)
        throw std::invalid_argument("Polar decomposition did not converge");
    const auto rotation = rotation_quaternion(r);
    if (!rotation)
        throw std::invalid_argument("Invalid polar rotation");
    Matrix3 s{};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned k = 0; k < 3; ++k)
                s[i][j] += r[k][i] * a[k][j];
    return {*rotation, s};
}

// The scale that T * R * S gives linear part @p m: the lengths of its columns, computed in double, with X negated when
// @p m reflects.
inline Vec3 axis_scale(const Matrix3 &m) {
    const auto column = [&](unsigned c) { return float(std::hypot(m[0][c], m[1][c], m[2][c])); };
    const float x = column(0);
    return {determinant(m) < 0 ? -x : x, column(1), column(2)};
}
// The rotation that T * R * S gives linear part @p m: the rotation of its polar decomposition after negating its X
// column when it reflects, which for a matrix without shear turns each axis onto its column's direction. Throws
// MathError with MathErrorCode::collapsed_transform when @p m is collapsed.
inline Quat orientation(Matrix3 m) {
    const double d = determinant(m);
    if (!(std::abs(d) > collapse_determinant))
        throw MathError(MathErrorCode::collapsed_transform);
    if (d < 0)
        for (auto &row : m)
            row[0] = -row[0];
    return polar(m).rotation;
}
} // namespace anima::detail
