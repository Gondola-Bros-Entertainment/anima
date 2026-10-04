#pragma once
// The curves that anima::ToneMapping documents, evaluated in double precision for the --tone-mapping and --fog checks
// from their published definitions alone, sharing no code with resolve.frag. Reinhard is `c / (1 + c)`. PBR Neutral
// follows the piecewise formula of the Khronos specification (https://github.com/KhronosGroup/ToneMapping,
// PBR_Neutral/README.md). AgX builds Blender's AgX formation for sRGB displays from its definitions: the Rec.709 and
// Rec.2020 matrices derived from their primaries and D65 white, the inset matrix of Blender's "AgX Log" color space
// (release/datafiles/colormanagement/config.ocio, Blender 4.0 through 5.2.2), the outset as the inverse of the matrix
// that Filament's ToneMapper.cpp lists as AgXOutsetMatrixInv, the sigmoid's scales solved here from its pivot, slope
// and powers, the blend that keeps 40% of the curve's change in HSV hue, and the clamp of each channel that
// anima::ToneMapping::agx states in place of Blender's. Agreement with the GPU therefore shows that the shader's
// precomputed matrices and scales, its branches and its float arithmetic reproduce these definitions; it cannot show
// that the definitions reproduce Blender's own table.
#include <algorithm>
#include <anima/environment.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace tone_mapping_reference {
using Rgb = std::array<double, 3>;
// Rows.
using Matrix = std::array<Rgb, 3>;

// Every curve, in enumerator order.
constexpr std::array mappings{anima::ToneMapping::none, anima::ToneMapping::reinhard, anima::ToneMapping::agx,
                              anima::ToneMapping::pbr_neutral};
// The name that the scene-environment payload stores @p mapping under.
constexpr std::string_view name(anima::ToneMapping mapping) {
    switch (mapping) {
    case anima::ToneMapping::none:
        return "none";
    case anima::ToneMapping::reinhard:
        return "reinhard";
    case anima::ToneMapping::agx:
        return "agx";
    case anima::ToneMapping::pbr_neutral:
        return "pbr_neutral";
    }
    return "unknown";
}

inline Rgb transformed(const Matrix &m, const Rgb &v) {
    Rgb result{};
    for (std::size_t row = 0; row < 3; ++row)
        result[row] = m[row][0] * v[0] + m[row][1] * v[1] + m[row][2] * v[2];
    return result;
}
inline Matrix product(const Matrix &a, const Matrix &b) {
    Matrix result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            for (std::size_t k = 0; k < 3; ++k)
                result[row][column] += a[row][k] * b[k][column];
    return result;
}
// The inverse by cofactors; the matrices here are far from singular.
inline Matrix inverse(const Matrix &m) {
    const auto cofactor = [&](std::size_t row, std::size_t column) {
        const auto r0 = (row + 1) % 3, r1 = (row + 2) % 3, c0 = (column + 1) % 3, c1 = (column + 2) % 3;
        return m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0];
    };
    const double determinant = m[0][0] * cofactor(0, 0) + m[0][1] * cofactor(0, 1) + m[0][2] * cofactor(0, 2);
    Matrix result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            result[row][column] = cofactor(column, row) / determinant;
    return result;
}

// A CIE xy chromaticity.
struct Chromaticity {
    double x, y;
};
// The matrix from linear RGB of the primaries @p red, @p green and @p blue to CIE XYZ, which takes RGB (1, 1, 1) to
// @p white at Y = 1.
inline Matrix to_xyz(Chromaticity red, Chromaticity green, Chromaticity blue, Chromaticity white) {
    const auto xyz = [](Chromaticity c) { return Rgb{c.x / c.y, 1, (1 - c.x - c.y) / c.y}; };
    const std::array primaries{xyz(red), xyz(green), xyz(blue)};
    Matrix m{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            m[row][column] = primaries[column][row];
    const auto scale = transformed(inverse(m), xyz(white));
    for (auto &row : m)
        for (std::size_t column = 0; column < 3; ++column)
            row[column] *= scale[column];
    return m;
}
constexpr Chromaticity d65{.3127, .3290};
// Linear Rec.709 to linear Rec.2020, both with D65 white.
inline Matrix rec709_to_rec2020() {
    const auto rec709 = to_xyz({.64, .33}, {.30, .60}, {.15, .06}, d65);
    const auto rec2020 = to_xyz({.708, .292}, {.170, .797}, {.131, .046}, d65);
    return product(inverse(rec2020), rec709);
}
// The rows of the MatrixTransform of Blender's "AgX Log" color space, which acts on linear Rec.2020.
constexpr Matrix blender_inset{{{0.856627153315983, 0.0951212405381588, 0.0482516061458583},
                                {0.137318972929847, 0.761241990602591, 0.101439036467562},
                                {0.11189821299995, 0.0767994186031903, 0.811302368396859}}};
// The rows of Filament's AgXOutsetMatrixInv, whose inverse is the outset; Filament lists its columns.
constexpr Matrix outset_inverse{{{0.899796955911611, 0.0871996192028351, 0.013003424885555},
                                 {0.11142098895748, 0.875575586156966, 0.0130034248855548},
                                 {0.11142098895748, 0.0871996192028349, 0.801379391839686}}};

// The sigmoid of AgX over the logarithm x, from 0 at 10 stops below 0.18 to 1 at 6.5 stops above it: through the pivot
// (10 / 16.5, 0.18^(1 / 2.4)) with slope 2.4, as `scale * e(slope * (x - pivot_x) / scale) + pivot_y` with
// `e(t) = t / (1 + t^1.5)^(1 / 1.5)`, each side's scale solved so that the curve passes through (0, 0) and (1, 1).
class Sigmoid {
  public:
    static constexpr double slope = 2.4, power = 1.5, pivot_x = 10 / 16.5;
    Sigmoid()
        : pivot_y_(std::pow(.18, 1 / 2.4)), toe_(-scale(slope * pivot_x, pivot_y_)),
          shoulder_(scale(slope * (1 - pivot_x), 1 - pivot_y_)) {}
    [[nodiscard]] double operator()(double x) const {
        const double s = x < pivot_x ? toe_ : shoulder_, t = slope * (x - pivot_x) / s;
        return s * t / std::pow(1 + std::pow(t, power), 1 / power) + pivot_y_;
    }

  private:
    // The scale s at which s * e(a / s) = b, for the run a and rise b from the pivot to the end of a side.
    static double scale(double a, double b) { return a / std::pow(std::pow(a / b, power) - 1, 1 / power); }
    double pivot_y_, toe_, shoulder_;
};

// The HSV hue, saturation and value of @p c.
struct Hsv {
    double hue, saturation, value;
};
inline Hsv hsv(const Rgb &c) {
    const double high = std::max({c[0], c[1], c[2]}), low = std::min({c[0], c[1], c[2]}), range = high - low;
    if (range <= 0)
        return {0, 0, high};
    double sixths = 0;
    if (c[0] == high)
        sixths = (c[1] - c[2]) / range;
    else if (c[1] == high)
        sixths = 2 + (c[2] - c[0]) / range;
    else
        sixths = 4 + (c[0] - c[1]) / range;
    const double hue = sixths / 6 - std::floor(sixths / 6);
    return {hue, range / high, high};
}
inline Rgb rgb(const Hsv &c) {
    const double v = c.value, s = c.saturation, sixths = c.hue * 6, sector = std::floor(sixths), f = sixths - sector;
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}

class Curves {
  public:
    Curves()
        : inset_(product(blender_inset, rec709_to_rec2020())),
          outset_(product(inverse(rec709_to_rec2020()), inverse(outset_inverse))) {}
    // The inset applied to linear Rec.709: Blender's inset after the conversion to Rec.2020.
    [[nodiscard]] const Matrix &inset() const noexcept { return inset_; }

    [[nodiscard]] static Rgb reinhard(const Rgb &c) {
        return {c[0] / (1 + c[0]), c[1] / (1 + c[1]), c[2] / (1 + c[2])};
    }

    // The specification's names: F90 = 0.04, Ks = 0.8 - F90 and Kd = 0.15.
    [[nodiscard]] static Rgb pbr_neutral(const Rgb &c) {
        constexpr double f90 = .04, ks = .8 - f90, kd = .15;
        const double x = std::min({c[0], c[1], c[2]});
        const double f = x <= 2 * f90 ? x - x * x / (4 * f90) : f90;
        const double p = std::max({c[0] - f, c[1] - f, c[2] - f});
        const double pn = 1 - (1 - ks) * (1 - ks) / (p + 1 - 2 * ks), g = 1 / (kd * (p - pn) + 1);
        Rgb result{};
        for (std::size_t i = 0; i < 3; ++i)
            result[i] = p <= ks ? c[i] - f : (c[i] - f) * pn / p * g + pn * (1 - g);
        return result;
    }

    [[nodiscard]] Rgb agx(const Rgb &c) const {
        const auto inset = transformed(inset_, c);
        Rgb curved{};
        for (std::size_t i = 0; i < 3; ++i) {
            // The logarithm, 0 at 10 stops below 0.18 and 1 at 6.5 stops above it. A channel at or below 0 lies below
            // every stop; any x below 0 takes the sigmoid below 0, which shows as 0.
            const double x = inset[i] > 0 ? (std::log2(inset[i] / .18) + 10) / 16.5 : -1;
            curved[i] = std::pow(std::max(sigmoid_(x), 0.), 2.4);
        }
        const auto before = hsv(inset).hue;
        auto shown = hsv(curved);
        double turn = shown.hue - before;
        if (turn > .5)
            turn -= 1;
        else if (turn < -.5)
            turn += 1;
        shown.hue = before + .4 * turn;
        shown.hue -= std::floor(shown.hue);
        auto result = transformed(outset_, rgb(shown));
        for (auto &channel : result)
            channel = std::clamp(channel, 0., 1.);
        return result;
    }

    // Linear @p c through @p mapping as resolve.frag applies it: times @p exposure, clamped to 0 to 65504, then the
    // curve. The display clamps the result to 0 to 1.
    [[nodiscard]] Rgb displayed(const Rgb &c, double exposure, anima::ToneMapping mapping) const {
        Rgb exposed{};
        for (std::size_t i = 0; i < 3; ++i)
            exposed[i] = std::clamp(c[i] * exposure, 0., 65504.);
        switch (mapping) {
        case anima::ToneMapping::reinhard:
            return reinhard(exposed);
        case anima::ToneMapping::agx:
            return agx(exposed);
        case anima::ToneMapping::pbr_neutral:
            return pbr_neutral(exposed);
        case anima::ToneMapping::none:
            break;
        }
        return exposed;
    }

  private:
    Matrix inset_, outset_;
    Sigmoid sigmoid_;
};

// @p value rounded to the nearest of a nonnegative float format with @p mantissa_bits explicit mantissa bits and a
// 5-bit exponent of bias 15, subnormals included, ties to even: 10 bits for half floats, and 6, 6 and 5 for the red,
// green and blue of B10G11R11_UFLOAT. Values stay far below the formats' largest.
inline double rounded(double value, int mantissa_bits) {
    if (value <= 0)
        return 0;
    int exponent = 0;
    (void)std::frexp(value, &exponent);
    // frexp's mantissa lies in [0.5, 1), so the leading bit is 2^(exponent - 1); subnormals keep the smallest normal
    // exponent, 2^-14.
    const int step = std::max(exponent - 1, -14) - mantissa_bits;
    return std::ldexp(std::nearbyint(std::ldexp(value, -step)), step);
}
} // namespace tone_mapping_reference
