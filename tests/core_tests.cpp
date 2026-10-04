#include <anima/core/fixed_step.hpp>
#include <anima/core/math.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <numbers>
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

TEST_CASE("Vectors of any finite length normalize, and nonfinite or tiny ones fall back to world up") {
    using anima::normalized;
    // Squared in float, a component of 1e20 overflowed to infinity and normalized to zero.
    constexpr float huge = 1e20F;
    CHECK(anima::length({huge, 0, 0}) == huge);
    const auto large = normalized({huge, 0, 0});
    CHECK(large.x == 1);
    CHECK(large.y == 0);
    CHECK(large.z == 0);
    const auto diagonal = normalized({-huge, huge, 0});
    CHECK(diagonal.x == doctest::Approx(-std::numbers::sqrt2_v<float> / 2));
    CHECK(diagonal.y == doctest::Approx(std::numbers::sqrt2_v<float> / 2));
    for (const anima::Vec3 v :
         {anima::Vec3{std::numeric_limits<float>::infinity(), 0, 0},
          anima::Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0}, anima::Vec3{1e-13F, 0, 0}}) {
        const auto fallback = normalized(v);
        CHECK(fallback.x == anima::world_up.x);
        CHECK(fallback.y == anima::world_up.y);
        CHECK(fallback.z == anima::world_up.z);
    }
}

TEST_CASE("Normals keep their direction through mirroring and stay defined when an axis collapses") {
    using anima::identity;
    using anima::normal;
    auto scaled = identity();
    scaled[0] = 2;
    scaled[5] = 3;
    scaled[10] = 4;
    CHECK(normal(scaled, {0, 0, 1}).z == doctest::Approx(1));
    auto mirrored = identity();
    mirrored[0] = -1;
    CHECK(normal(mirrored, {1, 0, 0}).x == doctest::Approx(-1));
    // A joint scaled to zero along z flattens the surface into the xy plane, whose normal is z.
    auto flattened = identity();
    flattened[10] = 0;
    CHECK(normal(flattened, {0, 0, 1}).z == doctest::Approx(1));
    // With every axis collapsed no direction remains, and normalized() falls back to +Y.
    const auto collapsed = normal(anima::Mat4{}, {0, 0, 1});
    CHECK(collapsed.x == 0);
    CHECK(collapsed.y == 1);
    CHECK(collapsed.z == 0);
}
