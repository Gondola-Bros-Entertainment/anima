#pragma once
// The sky that anima::Atmosphere documents, evaluated in double precision for the --atmosphere check from the
// contract alone, sharing no code with the shaders: the sun's transmittance integrated directly at every step of the
// view ray, and the multiple scattering term tabulated over altitude and the sun's zenith cosine from 256 directions
// per entry, finer than the renderer's tables, within 1% of a converged table from noon to 9 degrees below the
// horizon. Agreement shows that the tables, their mappings, interpolation and
// half-precision storage reproduce the documented integral; it cannot show that Hillaire's approximation of multiple
// scattering matches light that truly scatters many times.
#include <algorithm>
#include <anima/environment.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace atmosphere_reference {
using Rgb = std::array<double, 3>;
// A unit direction, +Y up.
struct Direction {
    double x{}, y{}, z{};
};
// The direction @p elevation degrees above the horizontal, turned @p azimuth degrees from -Z toward +X.
inline Direction direction(double elevation, double azimuth) {
    const double e = elevation * std::numbers::pi / 180, a = azimuth * std::numbers::pi / 180;
    return {std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)};
}
inline double dot(const Direction &a, const Direction &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

class Sky {
  public:
    explicit Sky(const anima::Atmosphere &a)
        : ground_(a.planet_radius), thickness_(a.thickness), rayleigh_height_(a.rayleigh_scale_height),
          mie_height_(a.mie_scale_height), ozone_altitude_(a.ozone_altitude), ozone_half_width_(a.ozone_width / 2.0),
          g_(a.mie_anisotropy), rayleigh_{a.rayleigh_scattering.x, a.rayleigh_scattering.y, a.rayleigh_scattering.z},
          mie_{a.mie_scattering.x, a.mie_scattering.y, a.mie_scattering.z},
          mie_extinction_{double(a.mie_scattering.x) + a.mie_absorption.x,
                          double(a.mie_scattering.y) + a.mie_absorption.y,
                          double(a.mie_scattering.z) + a.mie_absorption.z},
          ozone_{a.ozone_absorption.x, a.ozone_absorption.y, a.ozone_absorption.z},
          albedo_{a.ground_albedo.x, a.ground_albedo.y, a.ground_albedo.z} {
        tabulate_transmittance();
        tabulate_multiple_scattering();
    }
    // The sky's radiance per unit of the sun's irradiance above the atmosphere, seen from @p altitude along the unit
    // direction @p view, with the sun toward the unit direction @p sun.
    [[nodiscard]] Rgb radiance(double altitude, const Direction &view, const Direction &sun) const {
        const double mu = view.y, along = dot(view, sun), r = ground_ + altitude;
        const bool ground = meets_ground(altitude, mu);
        const double span = ground ? distance_to_ground(altitude, mu) : distance_to_top(altitude, mu);
        const double rayleigh_phase = 3 / (16 * std::numbers::pi) * (1 + along * along);
        const double mie_phase = 3 / (8 * std::numbers::pi) * (1 - g_ * g_) * (1 + along * along) /
                                 ((2 + g_ * g_) * std::pow(1 + g_ * g_ - 2 * g_ * along, 1.5));
        constexpr int steps = 1000;
        Rgb depth{}, light{};
        for (int i = 0; i < steps; ++i) {
            // Midpoints of steps that grow with the square of their index, finest near the eye.
            const double near = span * square(double(i) / steps), far = span * square(double(i + 1) / steps);
            const double t = (near + far) / 2, dt = far - near;
            const double rt = radius(altitude, mu, t), height = rt - ground_;
            const double sun_mu = std::clamp((r * sun.y + t * along) / rt, -1.0, 1.0);
            Rgb extinction{}, rayleigh{}, mie{};
            media(height, extinction, rayleigh, mie);
            const auto sunlight = to_top(height, sun_mu, 200);
            const auto multiple = multiple_scattering(height, sun_mu);
            for (std::size_t c = 0; c < 3; ++c) {
                const double kept = std::exp(-(depth[c] + extinction[c] * dt / 2));
                light[c] += kept * dt *
                            ((rayleigh[c] * rayleigh_phase + mie[c] * mie_phase) * sunlight[c] +
                             (rayleigh[c] + mie[c]) * multiple[c]);
                depth[c] += extinction[c] * dt;
            }
        }
        if (ground) {
            const double sun_mu = std::clamp((r * sun.y + span * along) / ground_, -1.0, 1.0);
            const auto sunlight = to_top(0, sun_mu, 400);
            for (std::size_t c = 0; c < 3; ++c)
                light[c] += std::exp(-depth[c]) * albedo_[c] / std::numbers::pi * sunlight[c] * std::max(sun_mu, 0.0);
        }
        return light;
    }
    // The share of light that crosses the atmosphere from @p altitude to its top along zenith cosine @p mu, zero along
    // a path that meets the ground, integrated in @p steps.
    [[nodiscard]] Rgb to_top(double altitude, double mu, int steps) const {
        if (meets_ground(altitude, mu))
            return {0, 0, 0};
        const double span = distance_to_top(altitude, mu);
        Rgb depth{};
        for (int i = 0; i < steps; ++i) {
            // Midpoints in the square root of the distance, finest near the start.
            const double u = (i + .5) / steps, t = span * u * u, dt = 2 * span * u / steps;
            Rgb extinction{}, rayleigh{}, mie{};
            media(radius(altitude, mu, t) - ground_, extinction, rayleigh, mie);
            for (std::size_t c = 0; c < 3; ++c)
                depth[c] += extinction[c] * dt;
        }
        return {std::exp(-depth[0]), std::exp(-depth[1]), std::exp(-depth[2])};
    }

  private:
    static double square(double x) { return x * x; }
    // Distance from the planet's center a distance @p t along the path from @p altitude at zenith cosine @p mu.
    [[nodiscard]] double radius(double altitude, double mu, double t) const {
        const double r = ground_ + altitude;
        return std::sqrt(std::max(r * r + 2 * r * t * mu + t * t, 0.0));
    }
    [[nodiscard]] bool meets_ground(double altitude, double mu) const {
        const double r = ground_ + altitude;
        return mu < 0 && r * r * mu * mu >= altitude * (2 * ground_ + altitude);
    }
    [[nodiscard]] double distance_to_ground(double altitude, double mu) const {
        const double r = ground_ + altitude, lowered = altitude * (2 * ground_ + altitude);
        return lowered / (-r * mu + std::sqrt(std::max(r * r * mu * mu - lowered, 0.0)));
    }
    [[nodiscard]] double distance_to_top(double altitude, double mu) const {
        const double r = ground_ + altitude, inside = (thickness_ - altitude) * (r + ground_ + thickness_);
        const double root = std::sqrt(std::max(r * r * mu * mu + inside, 0.0));
        return mu > 0 ? inside / (r * mu + root) : root - r * mu;
    }
    // The zenith cosine of the horizon from @p altitude.
    [[nodiscard]] double horizon(double altitude) const {
        return -std::sqrt(altitude * (2 * ground_ + altitude)) / (ground_ + altitude);
    }
    void media(double altitude, Rgb &extinction, Rgb &rayleigh, Rgb &mie) const {
        const double r = std::exp(-altitude / rayleigh_height_), m = std::exp(-altitude / mie_height_);
        const double o = std::max(0.0, 1 - std::abs(altitude - ozone_altitude_) / ozone_half_width_);
        for (std::size_t c = 0; c < 3; ++c) {
            rayleigh[c] = rayleigh_[c] * r;
            mie[c] = mie_[c] * m;
            extinction[c] = rayleigh[c] + mie_extinction_[c] * m + ozone_[c] * o;
        }
    }

    // Transmittance to the top over rows of altitude, spaced by the square of their index from the ground, and
    // columns of zenith cosine above the horizon, spaced by the square of their center's distance from it, for the
    // multiple scattering table's many lookups.
    static constexpr std::size_t altitude_rows = 128, cosine_columns = 256;
    void tabulate_transmittance() {
        transmittance_.resize(altitude_rows * cosine_columns);
        for (std::size_t i = 0; i < altitude_rows; ++i) {
            const double altitude = thickness_ * square(double(i) / (altitude_rows - 1)), low = horizon(altitude);
            for (std::size_t j = 0; j < cosine_columns; ++j) {
                const double x = (j + .5) / cosine_columns;
                transmittance_[i * cosine_columns + j] = to_top(altitude, low + (1 - low) * x * x, 200);
            }
        }
    }
    [[nodiscard]] Rgb transmittance(double altitude, double mu) const {
        altitude = std::clamp(altitude, 0.0, thickness_);
        if (meets_ground(altitude, mu))
            return {0, 0, 0};
        const double low = horizon(altitude);
        const double x = std::sqrt(std::clamp((mu - low) / (1 - low), 0.0, 1.0));
        return bilinear(transmittance_, cosine_columns, std::sqrt(altitude / thickness_) * (altitude_rows - 1),
                        std::clamp(x * cosine_columns - .5, 0.0, double(cosine_columns - 1)), altitude_rows);
    }

    // Psi_ms = L2 / (1 - f_ms) over rows of altitude, spaced by the square of their index from the ground, and
    // columns of the sun's zenith cosine from -1 to 1. Each entry averages 256 directions: 32 bands of zenith cosine,
    // half above the point's horizon and half below it, crowded toward it by the square of their index, and 8
    // azimuths on the side of the sun's vertical plane, each standing for itself and its mirror image across that
    // plane, which sees the same light; within 1% of 512 bands from the ground to 30 km and from noon to 9 degrees
    // below the horizon. Lookups interpolate the logarithm, since across the terminator the term falls by orders of
    // magnitude over a degree or two of the sun; 128 columns keep the result within 1% of a table of 1024.
    static constexpr std::size_t scattering_rows = 32, scattering_columns = 128;
    void tabulate_multiple_scattering() {
        multiple_.resize(scattering_rows * scattering_columns);
        constexpr int bands = 32, azimuths = 8, steps = 32;
        for (std::size_t i = 0; i < scattering_rows; ++i) {
            const double altitude = thickness_ * square(double(i) / (scattering_rows - 1)), r = ground_ + altitude;
            for (std::size_t j = 0; j < scattering_columns; ++j) {
                const double sun_mu = -1 + 2 * double(j) / (scattering_columns - 1);
                const Direction sun{std::sqrt(std::max(1 - sun_mu * sun_mu, 0.0)), sun_mu, 0};
                Rgb second{}, transfer{};
                for (int band = 0; band < bands; ++band)
                    for (int azimuth = 0; azimuth < azimuths; ++azimuth) {
                        const int rank = band % (bands / 2);
                        const double low = double(rank) / (bands / 2), high = double(rank + 1) / (bands / 2),
                                     middle = (low + high) / 2, level = horizon(altitude);
                        const double extent = band < bands / 2 ? 1 - level : 1 + level;
                        const double mu = level + (band < bands / 2 ? 1 : -1) * extent * middle * middle;
                        const double share = extent * (high * high - low * low) / 2 / azimuths;
                        const double across = std::sqrt(std::max(1 - mu * mu, 0.0));
                        const double angle = (azimuth + .5) * std::numbers::pi / azimuths;
                        const Direction way{across * std::cos(angle), mu, across * std::sin(angle)};
                        const double along = dot(way, sun);
                        const bool ground = meets_ground(altitude, mu);
                        const double span = ground ? distance_to_ground(altitude, mu) : distance_to_top(altitude, mu);
                        Rgb depth{}, light{}, transferred{};
                        for (int k = 0; k < steps; ++k) {
                            const double near = span * square(double(k) / steps),
                                         far = span * square(double(k + 1) / steps);
                            const double t = (near + far) / 2, dt = far - near;
                            const double rt = radius(altitude, mu, t);
                            const double there = std::clamp((r * sun_mu + t * along) / rt, -1.0, 1.0);
                            Rgb extinction{}, rayleigh{}, mie{};
                            media(rt - ground_, extinction, rayleigh, mie);
                            const auto sunlight = transmittance(rt - ground_, there);
                            for (std::size_t c = 0; c < 3; ++c) {
                                const double kept = std::exp(-(depth[c] + extinction[c] * dt / 2));
                                const double scattering = rayleigh[c] + mie[c];
                                light[c] += kept * scattering * sunlight[c] / (4 * std::numbers::pi) * dt;
                                transferred[c] += kept * scattering * dt;
                                depth[c] += extinction[c] * dt;
                            }
                        }
                        if (ground) {
                            const double there = std::clamp((r * sun_mu + span * along) / ground_, -1.0, 1.0);
                            const auto sunlight = transmittance(0, there);
                            for (std::size_t c = 0; c < 3; ++c)
                                light[c] += std::exp(-depth[c]) * albedo_[c] / std::numbers::pi * sunlight[c] *
                                            std::max(there, 0.0);
                        }
                        for (std::size_t c = 0; c < 3; ++c) {
                            second[c] += share * light[c];
                            transfer[c] += share * transferred[c];
                        }
                    }
                auto &entry = multiple_[i * scattering_columns + j];
                for (std::size_t c = 0; c < 3; ++c)
                    entry[c] = std::log(std::max(second[c] / (1 - std::min(transfer[c], .99)), 1e-300));
            }
        }
    }
    [[nodiscard]] Rgb multiple_scattering(double altitude, double sun_mu) const {
        const auto logarithm =
            bilinear(multiple_, scattering_columns,
                     std::sqrt(std::clamp(altitude / thickness_, 0.0, 1.0)) * (scattering_rows - 1),
                     (std::clamp(sun_mu, -1.0, 1.0) + 1) / 2 * (scattering_columns - 1), scattering_rows);
        return {std::exp(logarithm[0]), std::exp(logarithm[1]), std::exp(logarithm[2])};
    }

    // The table of @p columns per row interpolated at fractional @p row and @p column.
    static Rgb bilinear(const std::vector<Rgb> &table, std::size_t columns, double row, double column,
                        std::size_t rows) {
        row = std::clamp(row, 0.0, double(rows - 1));
        const auto i = std::min(std::size_t(row), rows - 2), j = std::min(std::size_t(column), columns - 2);
        const double a = row - double(i), b = column - double(j);
        Rgb result{};
        for (std::size_t c = 0; c < 3; ++c)
            result[c] = (1 - a) * ((1 - b) * table[i * columns + j][c] + b * table[i * columns + j + 1][c]) +
                        a * ((1 - b) * table[(i + 1) * columns + j][c] + b * table[(i + 1) * columns + j + 1][c]);
        return result;
    }

    double ground_, thickness_, rayleigh_height_, mie_height_, ozone_altitude_, ozone_half_width_, g_;
    Rgb rayleigh_, mie_, mie_extinction_, ozone_, albedo_;
    std::vector<Rgb> transmittance_, multiple_;
};
} // namespace atmosphere_reference
