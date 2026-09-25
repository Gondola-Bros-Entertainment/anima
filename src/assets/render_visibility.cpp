#include <anima/assets/render_visibility.hpp>
#include <limits>

namespace anima {
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
} // namespace anima
