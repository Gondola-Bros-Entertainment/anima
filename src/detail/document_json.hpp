#pragma once
#include "json.hpp"
#include <anima/core/math.hpp>
#include <cmath>
#include <stdexcept>

// Number readers of the scene, prefab, prefab variant and prefab composition documents, which word their rejections
// in a NumberFormat of their own.
namespace anima::detail {
// Throws `std::invalid_argument` with @p reason unless @p accepted.
inline void require(bool accepted, const char *reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}

// One document kind's messages for a value that is not a number, a number that is not finite as a float, and a
// matrix that is not an array of 16.
struct NumberFormat {
    const char *scalar, *finite, *matrix;
};

// Reads the float @p value. Throws `std::invalid_argument` with @p format's messages for a value that is not a number
// or not finite, and as json_float() does for a number outside the float range.
inline float document_scalar(const nlohmann::json &value, const NumberFormat &format) {
    require(value.is_number(), format.scalar);
    const auto result = json_float(value);
    require(std::isfinite(result), format.finite);
    return result;
}

// Reads the column-major matrix @p value, an array of 16 numbers, each as document_scalar() reads it. Throws
// `std::invalid_argument` with @p format's messages, and as document_scalar() does.
inline Mat4 document_matrix(const nlohmann::json &value, const NumberFormat &format) {
    require(value.is_array() && value.size() == 16, format.matrix);
    Mat4 result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = document_scalar(value[i], format);
    return result;
}
} // namespace anima::detail
