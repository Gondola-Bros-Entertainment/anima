#pragma once
#include <anima/core/math.hpp>

namespace anima {
using Quat = std::array<float, 4>; // xyzw, glTF convention
[[nodiscard]] Quat unit_quaternion(Quat q);
[[nodiscard]] Quat slerp(Quat a, Quat b, float t);
struct Transform {
    Vec3 translation{};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};
[[nodiscard]] Mat4 matrix(const Transform &transform);
} // namespace anima
