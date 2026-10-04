#include <algorithm>
#include <anima/assets/render_visibility.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace anima {
namespace {
// The length of the three axes' measures of @p bounds from @p eye, each @p measure of the amount by which the eye lies
// below the minimum and that by which it lies above the maximum; NaN when either amount is.
template <class Measure> double axis_length(Vec3 eye, const RenderBounds &bounds, Measure measure) noexcept {
    double squared = 0;
    for (const auto [lo, hi, at] :
         {std::array{bounds.minimum.x, bounds.maximum.x, eye.x}, std::array{bounds.minimum.y, bounds.maximum.y, eye.y},
          std::array{bounds.minimum.z, bounds.maximum.z, eye.z}}) {
        const double below = double(lo) - at, above = double(at) - hi;
        if (std::isnan(below) || std::isnan(above))
            return std::numeric_limits<double>::quiet_NaN();
        const double length = measure(below, above);
        squared += length * length;
    }
    return std::sqrt(squared);
}
} // namespace
RenderFrustum::RenderFrustum(const Mat4 &view) {
    for (const auto value : view)
        if (!std::isfinite(value))
            throw std::invalid_argument("Culling view projection must be finite");
    for (unsigned column = 0; column < 4; ++column) {
        const double x = view[4 * column], y = view[4 * column + 1];
        const double z = view[4 * column + 2], w = view[4 * column + 3];
        planes_[0][column] = w + x;
        planes_[1][column] = w - x;
        planes_[2][column] = w + y;
        planes_[3][column] = w - y;
        planes_[4][column] = z;
        planes_[5][column] = w - z;
        error_magnitudes_[0][column] = error_magnitudes_[1][column] = std::abs(w) + std::abs(x);
        error_magnitudes_[2][column] = error_magnitudes_[3][column] = std::abs(w) + std::abs(y);
        error_magnitudes_[4][column] = std::abs(z);
        error_magnitudes_[5][column] = std::abs(w) + std::abs(z);
    }
}
bool RenderFrustum::intersects(const RenderBounds &bounds) const noexcept {
    if (!bounds.valid)
        return true;
    const double low[]{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
    const double high[]{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(low[axis]) || !std::isfinite(high[axis]) || low[axis] > high[axis])
            return true;
    for (std::size_t i = 0; i < planes_.size(); ++i) {
        const auto &plane = planes_[i], &error = error_magnitudes_[i];
        double maximum = plane[3], magnitude = error[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            maximum += plane[axis] * (plane[axis] >= 0 ? high[axis] : low[axis]);
            magnitude += error[axis] * std::max(std::abs(low[axis]), std::abs(high[axis]));
        }
        // Bounds and vertex shaders use floats. Retain a scale-relative margin
        // for rounding of the original camera rows, including cancellation when
        // clip coordinates are evaluated separately and then compared by the GPU.
        const double tolerance = 16 * std::numeric_limits<float>::epsilon() * magnitude;
        if (maximum < -tolerance)
            return false;
    }
    return true;
}
double nearest_distance(Vec3 eye, const RenderBounds &bounds) noexcept {
    if (!bounds.valid)
        return 0;
    return axis_length(eye, bounds, [](double below, double above) { return std::max({below, above, 0.0}); });
}
double farthest_distance(Vec3 eye, const RenderBounds &bounds) noexcept {
    if (!bounds.valid)
        return std::numeric_limits<double>::infinity();
    return axis_length(eye, bounds,
                       [](double below, double above) { return std::max(std::abs(below), std::abs(above)); });
}
bool within_visibility_range(const VisibilityRange &range, Vec3 eye, const RenderBounds &bounds) noexcept {
    // Negated comparisons keep bounds and ranges whose distances are NaN.
    return !bounds.valid ||
           (!(nearest_distance(eye, bounds) >= range.end) && !(farthest_distance(eye, bounds) < range.begin));
}
std::optional<std::size_t> lod_level(std::span<const DrawLevel> levels, double distance, double scale,
                                     double pixels_per_unit_error, bool perspective, float threshold_pixels) noexcept {
    if (levels.empty() || !(threshold_pixels > 0) || (perspective && !(distance > 0)))
        return std::nullopt;
    const double pixels = pixels_per_unit_error * scale / (perspective ? distance : 1);
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        // Negated, so that a NaN product does not fit.
        if (!(double(levels[i].error) * pixels <= threshold_pixels))
            break;
        chosen = i;
    }
    return chosen;
}
} // namespace anima
