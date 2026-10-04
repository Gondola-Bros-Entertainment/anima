#pragma once
#include <algorithm>
#include <anima/core/math.hpp>
#include <cmath>
#include <stdexcept>

namespace anima::detail {
// Each element of an affine matrix's bottom row is within this of (0, 0, 0, 1), as Scene documents.
inline constexpr float affine_tolerance = 1e-5F;

// Whether every element of @p matrix is finite and its bottom row is (0, 0, 0, 1) within affine_tolerance.
inline bool is_affine(const Mat4 &matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); }) &&
           std::abs(matrix[3]) < affine_tolerance && std::abs(matrix[7]) < affine_tolerance &&
           std::abs(matrix[11]) < affine_tolerance && std::abs(matrix[15] - 1) < affine_tolerance;
}
// Accepts what is_affine() does, as Scene validates object, node and palette matrices. Throws
// std::invalid_argument("Non-finite instance transform") when an element is not finite, else
// std::invalid_argument("Instance transform must be affine") when the bottom row is not.
inline void require_affine(const Mat4 &matrix) {
    if (!std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); }))
        throw std::invalid_argument("Non-finite instance transform");
    if (!is_affine(matrix))
        throw std::invalid_argument("Instance transform must be affine");
}
} // namespace anima::detail
