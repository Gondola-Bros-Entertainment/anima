#pragma once
#include <stdexcept>
#include <string_view>

namespace anima {
enum class MathErrorCode {
    nonfinite_matrix,
    singular_matrix,
    inverse_overflow,
    invalid_frustum,
    nonfinite_projection,
    singular_projection,
    nonfinite_quaternion,
    zero_quaternion
};
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
    }
    return "Invalid math operation";
}
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
    }
    return "unknown_math_error";
}
class MathError : public std::invalid_argument {
  public:
    explicit MathError(MathErrorCode code) : std::invalid_argument(math_error_message(code)), code_(code) {}
    MathErrorCode code() const noexcept { return code_; }

  private:
    MathErrorCode code_;
};
} // namespace anima
