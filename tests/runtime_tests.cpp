#include "consumer/runtime.hpp"
#include <doctest/doctest.h>

TEST_CASE("The shared consumer scenario for shared-world scene phases, physics, audio and transitions passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    runtime_consumer::run();
}
