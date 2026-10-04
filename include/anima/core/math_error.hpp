#pragma once
#include <stdexcept>
#include <string_view>

/// @file
/// Error codes for math operations without a finite result. Part of the `anima::core` target.

namespace anima {
/// Reason a math operation failed.
enum class MathErrorCode {
    nonfinite_matrix,     ///< A matrix to invert has a nonfinite element.
    singular_matrix,      ///< Inverting a matrix met a zero pivot.
    inverse_overflow,     ///< An inverse has an element that is not a finite `float`.
    invalid_frustum,      ///< Perspective arguments are not finite or break `0 < vertical_fov_radians < pi`,
                          ///< `aspect > 0` and `0 < near_plane < far_plane`.
    nonfinite_projection, ///< A view-projection matrix has a nonfinite element.
    singular_projection,  ///< Solving a view-projection matrix met a zero pivot.
    nonfinite_quaternion, ///< A quaternion to normalize has a nonfinite component.
    zero_quaternion,      ///< A quaternion to normalize has a squared length below `1e-20`.
    invalid_orthographic, ///< Orthographic arguments are not finite or break `aspect > 0`, `height > 0` and
                          ///< `near_plane < far_plane`.
    invalid_axis_angle,   ///< A rotation angle is not finite, or its axis is not finite or no longer than `1e-12`.
    invalid_look,         ///< A look direction or up direction is not finite or no longer than `1e-12`, or the
                          ///< two are parallel: the sine of the angle between them is at most `1e-6`.
    collapsed_transform   ///< An affine transform's upper 3x3 has a determinant within `1e-12` of 0, so it has
                          ///< collapsed an axis and has no rotation.
};
/// Fixed English description of @p code, or `"Invalid math operation"` for a value outside the
/// enumeration.
constexpr const char *math_error_message(MathErrorCode code) noexcept {
    switch (code) {
    case MathErrorCode::nonfinite_matrix:
        return "Cannot invert nonfinite matrix";
    case MathErrorCode::singular_matrix:
        return "Cannot invert singular matrix";
    case MathErrorCode::inverse_overflow:
        return "Inverse exceeds finite range";
    case MathErrorCode::invalid_frustum:
        return "Invalid perspective frustum";
    case MathErrorCode::nonfinite_projection:
        return "View projection must be finite";
    case MathErrorCode::singular_projection:
        return "View projection must be invertible";
    case MathErrorCode::nonfinite_quaternion:
        return "Cannot normalize nonfinite quaternion";
    case MathErrorCode::zero_quaternion:
        return "Cannot normalize zero quaternion";
    case MathErrorCode::invalid_orthographic:
        return "Invalid orthographic volume";
    case MathErrorCode::invalid_axis_angle:
        return "Invalid axis-angle rotation";
    case MathErrorCode::invalid_look:
        return "Invalid look direction";
    case MathErrorCode::collapsed_transform:
        return "Affine transform is collapsed";
    }
    return "Invalid math operation";
}
/// Enumerator name of @p code, or `"unknown_math_error"` for a value outside the enumeration.
constexpr std::string_view math_error_name(MathErrorCode code) noexcept {
    switch (code) {
    case MathErrorCode::nonfinite_matrix:
        return "nonfinite_matrix";
    case MathErrorCode::singular_matrix:
        return "singular_matrix";
    case MathErrorCode::inverse_overflow:
        return "inverse_overflow";
    case MathErrorCode::invalid_frustum:
        return "invalid_frustum";
    case MathErrorCode::nonfinite_projection:
        return "nonfinite_projection";
    case MathErrorCode::singular_projection:
        return "singular_projection";
    case MathErrorCode::nonfinite_quaternion:
        return "nonfinite_quaternion";
    case MathErrorCode::zero_quaternion:
        return "zero_quaternion";
    case MathErrorCode::invalid_orthographic:
        return "invalid_orthographic";
    case MathErrorCode::invalid_axis_angle:
        return "invalid_axis_angle";
    case MathErrorCode::invalid_look:
        return "invalid_look";
    case MathErrorCode::collapsed_transform:
        return "collapsed_transform";
    }
    return "unknown_math_error";
}
/// `std::invalid_argument` that carries a MathErrorCode; `what()` returns math_error_message().
class MathError : public std::invalid_argument {
  public:
    explicit MathError(MathErrorCode code) : std::invalid_argument(math_error_message(code)), code_(code) {}
    /// Reason for the failure.
    MathErrorCode code() const noexcept { return code_; }

  private:
    MathErrorCode code_;
};
} // namespace anima
