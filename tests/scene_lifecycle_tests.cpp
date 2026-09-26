#include "consumer/lifecycle.hpp"
#include <doctest/doctest.h>

TEST_CASE("The shared consumer scenario for inherited activation, lifecycle, scheduling and persistence passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    lifecycle_test::run();
}
