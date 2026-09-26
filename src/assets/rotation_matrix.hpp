#pragma once
#include <algorithm>
#include <anima/core/transform.hpp>
#include <array>
#include <cmath>
#include <optional>

namespace anima::detail {
// A 3x3 matrix in double precision, indexed [row][column].
using Matrix3 = std::array<std::array<double, 3>, 3>;
// Shepperd's method divides by this pivot; smaller ones divide by rounding noise.
inline constexpr double minimum_rotation_pivot = 1e-12;

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
} // namespace anima::detail
