#pragma once
#include <anima/core/math.hpp>

namespace anima::detail {
// Whether @p posing reverses the winding of the triangles it places: its upper 3x3 has a negative determinant, as
// glTF specifies for mirrored nodes and as the renderer's vertex shader tests it. A zero determinant keeps it.
inline bool reverses_winding(const Mat4 &posing) {
    const Vec3 a{posing[0], posing[1], posing[2]}, b{posing[4], posing[5], posing[6]},
        c{posing[8], posing[9], posing[10]};
    return dot(a, cross(b, c)) < 0;
}
} // namespace anima::detail
