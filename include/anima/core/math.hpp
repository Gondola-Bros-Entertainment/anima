#pragma once
#include <anima/core/math_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace anima {
struct Vec3 {
    float x{}, y{}, z{};
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
// Engine basis: right-handed with +Y up. Cameras and audio listeners face local -Z.
inline constexpr Vec3 world_up{0, 1, 0};
inline constexpr Vec3 view_forward{0, 0, -1};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalized(Vec3 v) {
    const auto n = length(v);
    return n > 1e-12F ? v * (1 / n) : Vec3{0, 1, 0};
}
using Mat4 = std::array<float, 16>; // Column-major, column vectors, glTF convention.
inline Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
// Columns 0 to 2 hold the transformed (scaled) X, Y and Z axes; column 3 holds the translation.
inline Vec3 axis_x(const Mat4 &m) { return {m[0], m[1], m[2]}; }
inline Vec3 axis_y(const Mat4 &m) { return {m[4], m[5], m[6]}; }
inline Vec3 axis_z(const Mat4 &m) { return {m[8], m[9], m[10]}; }
inline Vec3 translation_of(const Mat4 &m) { return {m[12], m[13], m[14]}; }
inline Mat4 inverse(const Mat4 &matrix) {
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
    Mat4 result{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            result[c * 4 + r] = static_cast<float>(rows[r][c + 4]);
            if (!std::isfinite(result[c * 4 + r]))
                throw MathError(MathErrorCode::inverse_overflow);
        }
    return result;
}
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 result{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned k = 0; k < 4; ++k)
                result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return result;
}
inline Vec3 point(const Mat4 &m, Vec3 v) {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
}
inline Vec3 normal(const Mat4 &m, Vec3 n) {
    const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
    const auto determinant = dot(a, cross(b, c));
    if (std::abs(determinant) < 1e-12F)
        throw std::runtime_error("Singular mesh/skin transform");
    return normalized((cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z) * (1 / determinant));
}
inline std::array<float, 4> tangent(const Mat4 &m, const std::array<float, 4> &t) {
    if (t[3] == 0)
        return {};
    const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
    const auto v = a * t[0] + b * t[1] + c * t[2];
    return {v.x, v.y, v.z, t[3] * (dot(a, cross(b, c)) < 0 ? -1.F : 1.F)};
}
inline Mat4 look_at(Vec3 eye, Vec3 target) {
    const auto forward = normalized(target - eye);
    const auto right = normalized(cross(forward, {0, 1, 0}));
    const auto up = cross(right, forward);
    return {right.x, up.x, -forward.x, 0, right.y,          up.y,          -forward.y,        0,
            right.z, up.z, -forward.z, 0, -dot(right, eye), -dot(up, eye), dot(forward, eye), 1};
}
inline Mat4 perspective(float aspect, float near_plane, float far_plane) {
    if (!(aspect > 0 && near_plane > 0 && far_plane > near_plane))
        throw MathError(MathErrorCode::invalid_frustum);
    constexpr float f = 2.41421356237F; // 45 degrees vertical field of view.
    // Vulkan zero-to-one depth, right-handed view, framebuffer Y points down.
    return {f / aspect,
            0,
            0,
            0,
            0,
            -f,
            0,
            0,
            0,
            0,
            far_plane / (near_plane - far_plane),
            -1,
            0,
            0,
            (near_plane * far_plane) / (near_plane - far_plane),
            0};
}
// Surface-to-view origin: perspective position (w=1), or the constant direction
// toward an orthographic camera (w=0). Solve VP*x=(0,0,1,0), the projection center
// at infinity in clip space. Callers need not supply a duplicate camera position.
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
    const double divisor = perspective_view ? rows[3][4] : -std::hypot(rows[0][4], rows[1][4], rows[2][4]);
    std::array<float, 4> result{};
    for (unsigned r = 0; r < 3; ++r) {
        result[r] = static_cast<float>(rows[r][4] / divisor);
        if (!std::isfinite(result[r]))
            throw std::invalid_argument("View origin exceeds finite range");
    }
    result[3] = perspective_view ? 1.F : 0.F;
    return result;
}
struct OrbitCamera {
    Vec3 target{};
    float radius = 1, yaw = 0.3F, pitch = 0.12F, distance = 3;
    void frame(Vec3 minimum, Vec3 maximum) {
        target = (minimum + maximum) * 0.5F;
        radius = std::max(length(maximum - minimum) * 0.5F, 0.01F);
        yaw = 0.3F;
        pitch = 0.12F;
        distance = radius * 3;
    }
    void orbit(float horizontal, float vertical) {
        yaw = std::remainder(yaw + horizontal, 6.28318530718F);
        pitch = std::clamp(pitch + vertical, -1.4F, 1.4F);
    }
    void zoom(float amount) { distance = std::clamp(distance * std::exp(-amount * 0.12F), radius * 1.2F, radius * 15); }
    [[nodiscard]] Vec3 position() const {
        return target +
               Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
    }
    // Ground-plane directions are independent of pitch and boom collision.
    // Applications choose how these directions map to their movement controls.
    [[nodiscard]] Vec3 horizontal_forward() const { return {-std::sin(yaw), 0, -std::cos(yaw)}; }
    [[nodiscard]] Vec3 horizontal_right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
    [[nodiscard]] Mat4 matrix(float aspect) const {
        return perspective(aspect, radius * 0.01F, radius * 50) * look_at(position(), target);
    }
};
} // namespace anima
