#include <anima/core/fixed_step.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
constexpr auto step = 10ms;
constexpr std::uint32_t catch_up_limit = 4;
constexpr auto invalid_clock = "Invalid fixed-step interval or catch-up limit";
} // namespace

TEST_CASE("Render cadence does not change simulation ticks") {
    // One second of frames either way: 100 steady 10 ms frames, or 20 pairs of 7 ms and 43 ms.
    constexpr int steady_frames = 100, uneven_frame_pairs = 20;
    constexpr auto short_frame = 7ms, long_frame = 43ms;
    anima::FixedStepClock steady{step}, uneven{step};
    unsigned steady_ticks = 0, uneven_ticks = 0;
    for (int i = 0; i < steady_frames; ++i)
        steady_ticks += steady.advance(step).steps;
    for (int i = 0; i < uneven_frame_pairs; ++i) {
        uneven_ticks += uneven.advance(short_frame).steps;
        uneven_ticks += uneven.advance(long_frame).steps;
    }
    CHECK(steady_ticks == steady_frames);
    CHECK(uneven_ticks == steady_ticks);
    CHECK(uneven.advance(0ns).interpolation == 0.0);
}

TEST_CASE("Catch-up is bounded and keeps the fractional remainder") {
    anima::FixedStepClock bounded{step, catch_up_limit};
    CHECK(bounded.advance(step / 2).steps == 0);
    const auto stall = 1s;
    const auto stalled = bounded.advance(stall);
    CHECK(stalled.steps == catch_up_limit);
    CHECK(stalled.dropped == stall - catch_up_limit * step);
    CHECK(stalled.interpolation == doctest::Approx(0.5)); // The earlier half step survives the stall.
    bounded.reset();
    CHECK(bounded.advance(0ns).interpolation == 0.0);
    const auto extreme = bounded.advance(std::chrono::nanoseconds::max());
    CHECK(extreme.steps == catch_up_limit);
    CHECK(extreme.interpolation == 0.0);
}

TEST_CASE("Invalid clock inputs are rejected") {
    anima::FixedStepClock clock{step, catch_up_limit};
    CHECK_THROWS_WITH_AS(clock.advance(-1ns), "Elapsed simulation time cannot be negative", std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::FixedStepClock{0ns}, invalid_clock, std::invalid_argument);
    CHECK_THROWS_WITH_AS((anima::FixedStepClock{step, 0}), invalid_clock, std::invalid_argument);
    // A step so long that max_steps + 1 of them would overflow the nanosecond accumulator.
    CHECK_THROWS_WITH_AS((anima::FixedStepClock{std::chrono::nanoseconds::max(), 2}), invalid_clock,
                         std::invalid_argument);
}
