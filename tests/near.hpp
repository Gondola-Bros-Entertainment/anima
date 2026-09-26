#pragma once
#include <cmath>
#include <ostream>

// An expected value with an absolute tolerance, for CHECK(actual == Near{expected, tolerance}). doctest::Approx
// widens its margin with the magnitudes compared, so it cannot express a fixed tolerance.
struct Near {
    double value{};
    double tolerance{};
};
inline bool operator==(double actual, Near expected) { return std::abs(actual - expected.value) < expected.tolerance; }
// doctest prints the expected side of a failed comparison with this.
inline std::ostream &operator<<(std::ostream &stream, Near expected) {
    return stream << expected.value << " +/- " << expected.tolerance;
}
