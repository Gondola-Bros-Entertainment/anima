#include <anima/core/transform.hpp>

#include <anima/core/math_error.hpp>

namespace anima {
namespace {
// Squared norms below this are treated as zero: normalizing them would amplify rounding noise.
constexpr double minimum_squared_norm = 1e-20;
// Above this cosine the arc is too short for sin(angle) to divide by, so slerp blends linearly.
constexpr float linear_blend_cosine = 0.9995F;
} // namespace
Quat unit_quaternion(Quat q) {
    double sum = 0;
    for (const auto v : q) {
        if (!std::isfinite(v))
            throw MathError(MathErrorCode::nonfinite_quaternion);
        sum += double(v) * v;
    }
    if (sum < minimum_squared_norm)
        throw MathError(MathErrorCode::zero_quaternion);
    const auto scale = static_cast<float>(1 / std::sqrt(sum));
    for (auto &v : q)
        v *= scale;
    return q;
}
Quat slerp(Quat a, Quat b, float t) {
    a = unit_quaternion(a);
    b = unit_quaternion(b);
    float cosine = 0;
    for (unsigned i = 0; i < 4; ++i)
        cosine += a[i] * b[i];
    if (cosine < 0) {
        for (auto &v : b)
            v = -v;
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
    const float x = q[0], y = q[1], z = q[2], w = q[3];
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
} // namespace anima
