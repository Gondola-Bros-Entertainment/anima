#include <anima/core/fixed_step.hpp>
#include <anima/core/math.hpp>
#include <anima/core/math_error.hpp>
#include <anima/core/transform.hpp>
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>

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

TEST_CASE("Two-component vectors add, subtract, scale, measure and normalize") {
    const anima::Vec2 a{1, 2}, b{3, -4};
    const auto sum = a + b;
    CHECK(sum.x == 4);
    CHECK(sum.y == -2);
    const auto difference = a - b;
    CHECK(difference.x == -2);
    CHECK(difference.y == 6);
    const auto negated = -a;
    CHECK(negated.x == -1);
    CHECK(negated.y == -2);
    const auto scaled = b * .5F;
    CHECK(scaled.x == 1.5F);
    CHECK(scaled.y == -2);
    CHECK(dot(a, b) == -5);
    CHECK(length(b) == 5);
    const auto unit = normalized(b);
    CHECK(unit.x == doctest::Approx(.6F));
    CHECK(unit.y == doctest::Approx(-.8F));
    // Squared in float, a component of 1e20 would overflow to infinity.
    constexpr float huge = 1e20F;
    CHECK(length(anima::Vec2{huge, 0}) == huge);
    const auto large = normalized(anima::Vec2{-huge, 0});
    CHECK(large.x == -1);
    CHECK(large.y == 0);
    for (const anima::Vec2 v : {anima::Vec2{std::numeric_limits<float>::infinity(), 0},
                                anima::Vec2{0, std::numeric_limits<float>::quiet_NaN()}, anima::Vec2{1e-13F, 0}}) {
        const auto fallback = normalized(v);
        CHECK(fallback.x == 0);
        CHECK(fallback.y == 1);
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

// This file is outside namespace anima and declares no using-declaration for its operators, so only
// argument-dependent lookup can find the matrix product.
TEST_CASE("Matrices and quaternions are distinct types whose operators argument-dependent lookup finds") {
    const anima::Mat4 scale{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1};
    const anima::Mat4 shift{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
    // Applied to a column vector, the right operand acts first.
    CHECK(shift * scale == anima::Mat4{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 1, 2, 3, 1});
    CHECK(scale * shift == anima::Mat4{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 2, 4, 6, 1});
    CHECK(anima::Mat4{} == anima::Mat4{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    // Equality is the elements' float equality.
    auto signed_zero = anima::Mat4{};
    signed_zero[0] = -0.F;
    CHECK(signed_zero == anima::Mat4{});
    auto not_a_number = anima::identity();
    not_a_number[15] = std::numeric_limits<float>::quiet_NaN();
    CHECK(not_a_number != not_a_number);

    // A GPU buffer receives the column-major elements byte for byte.
    static_assert(sizeof(anima::Mat4) == 16 * sizeof(float) && anima::Mat4::size() == 16);
    static_assert(std::is_standard_layout_v<anima::Mat4> && std::is_trivially_copyable_v<anima::Mat4>);
    std::array<float, 16> bytes{};
    std::memcpy(bytes.data(), &shift, sizeof shift);
    CHECK(bytes == shift.elements);
    CHECK(bytes[13] == 2);

    // Neither converts from a std::array, so a tangent() or view_origin() result is not a rotation.
    static_assert(!std::is_convertible_v<std::array<float, 16>, anima::Mat4>);
    static_assert(!std::is_convertible_v<std::array<float, 4>, anima::Quat>);
    static_assert(!std::is_convertible_v<decltype(anima::view_origin(shift)), anima::Quat>);

    // Quat{} is all zeros, which is not a rotation; a transform starts at the identity rotation.
    constexpr anima::Quat zero{};
    static_assert(zero.x == 0 && zero.y == 0 && zero.z == 0 && zero.w == 0);
    CHECK(anima::Transform{}.rotation == anima::identity_rotation);
    CHECK(anima::identity_rotation == anima::Quat{0, 0, 0, 1});
    CHECK_THROWS_WITH_AS((void)anima::unit_quaternion(zero), "Cannot normalize zero quaternion", anima::MathError);
    // Indices follow XYZW.
    constexpr anima::Quat q{1, 2, 3, 4};
    static_assert(q[0] == q.x && q[1] == q.y && q[2] == q.z && q[3] == q.w);
    auto written = zero;
    written[3] = 1;
    CHECK(written == anima::identity_rotation);
}

namespace {
// Checks that @p actual is @p expected within doctest's default tolerance, component by component.
void check_near(anima::Vec3 actual, anima::Vec3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}
// Checks that @p actual and @p expected are the same rotation; q and -q are.
void check_rotation(const anima::Quat &actual, const anima::Quat &expected) {
    float cosine = 0;
    for (std::size_t i = 0; i < 4; ++i)
        cosine += actual[i] * expected[i];
    CHECK(std::abs(cosine) == doctest::Approx(1));
}
constexpr float quarter_turn = std::numbers::pi_v<float> / 2;
const auto invalid_axis_angle = anima::math_error_message(anima::MathErrorCode::invalid_axis_angle);
const auto invalid_look = anima::math_error_message(anima::MathErrorCode::invalid_look);
} // namespace

TEST_CASE("Axis-angle rotations follow the right-hand rule, and Euler angles apply roll, then pitch, then yaw") {
    using anima::axis_angle;
    using anima::rotate;
    // A quarter turn about +Y takes +X to -Z, counterclockwise seen from above.
    check_near(rotate(axis_angle(anima::world_up, quarter_turn), {1, 0, 0}), {0, 0, -1});
    // The axis need not have unit length.
    CHECK(axis_angle({0, 2, 0}, .5F) == axis_angle(anima::world_up, .5F));
    CHECK(anima::math_error_name(anima::MathErrorCode::invalid_axis_angle) == "invalid_axis_angle");
    CHECK_THROWS_WITH_AS((void)axis_angle({}, 1), invalid_axis_angle, anima::MathError);
    CHECK_THROWS_WITH_AS((void)axis_angle({1e-13F, 0, 0}, 1), invalid_axis_angle, anima::MathError);
    CHECK_THROWS_WITH_AS((void)axis_angle({std::numeric_limits<float>::infinity(), 0, 0}, 1), invalid_axis_angle,
                         anima::MathError);
    CHECK_THROWS_WITH_AS((void)axis_angle({1, 0, 0}, std::numeric_limits<float>::quiet_NaN()), invalid_axis_angle,
                         anima::MathError);

    constexpr float pitch = .3F, yaw = -1.1F, roll = .7F;
    CHECK(anima::euler_radians(pitch, yaw, roll) ==
          axis_angle(anima::world_up, yaw) * axis_angle({1, 0, 0}, pitch) * axis_angle({0, 0, 1}, roll));
    // Pitch before yaw: a quarter turn up, then a quarter turn left, still faces up; yaw first would face -X.
    check_near(rotate(anima::euler_radians(quarter_turn, quarter_turn, 0), anima::view_forward), {0, 1, 0});
    // Roll before yaw: +X rolls onto +Y, which yaw keeps; yaw first would turn it onto -Z.
    check_near(rotate(anima::euler_radians(0, quarter_turn, quarter_turn), {1, 0, 0}), {0, 1, 0});
    CHECK(rotate(anima::euler_radians(.2F, 0, 0), anima::view_forward).y > 0);
    CHECK(rotate(anima::euler_radians(0, .2F, 0), anima::view_forward).x < 0);
    CHECK_THROWS_WITH_AS((void)anima::euler_radians(0, std::numeric_limits<float>::infinity(), 0), invalid_axis_angle,
                         anima::MathError);
}

TEST_CASE("Quaternion products compose like matrices, conjugates undo them and rotate() normalizes") {
    using anima::rotate;
    const auto a = anima::axis_angle({1, 2, 3}, .8F), b = anima::axis_angle({-2, 0, 1}, 2.1F);
    const anima::Vec3 v{.5F, -1, 2};
    check_near(rotate(a * b, v), rotate(a, rotate(b, v)));
    const auto product = anima::matrix({{}, a * b, {1, 1, 1}}),
               composed = anima::matrix({{}, a, {1, 1, 1}}) * anima::matrix({{}, b, {1, 1, 1}});
    for (std::size_t i = 0; i < product.size(); ++i)
        CHECK(product[i] == doctest::Approx(composed[i]));
    check_near(rotate(anima::conjugate(a), rotate(a, v)), v);
    CHECK(anima::conjugate(anima::Quat{1, 2, 3, 4}) == anima::Quat{-1, -2, -3, 4});
    // A quaternion of any nonzero length rotates as its unit quaternion does.
    check_near(rotate({a.x * 3, a.y * 3, a.z * 3, a.w * 3}, v), rotate(a, v));
    CHECK_THROWS_WITH_AS((void)rotate(anima::Quat{}, v), "Cannot normalize zero quaternion", anima::MathError);
}

TEST_CASE("Look rotations turn -Z onto the direction, look_at() inverts them, and up must not be parallel") {
    using anima::look_rotation;
    using anima::rotate;
    const auto east = look_rotation({3, 0, 0});
    check_near(rotate(east, anima::view_forward), {1, 0, 0});
    check_near(rotate(east, {0, 1, 0}), {0, 1, 0});
    // Up is made perpendicular to the direction.
    const auto tilted = look_rotation({0, 0, -1}, {1, 1, 1});
    check_near(rotate(tilted, {0, 1, 0}), {std::numbers::sqrt2_v<float> / 2, std::numbers::sqrt2_v<float> / 2, 0});

    // Straight down is parallel to world_up; another up resolves it.
    CHECK(anima::math_error_name(anima::MathErrorCode::invalid_look) == "invalid_look");
    CHECK_THROWS_WITH_AS((void)look_rotation({0, -2, 0}), invalid_look, anima::MathError);
    const auto down = look_rotation({0, -2, 0}, {0, 0, -1});
    check_near(rotate(down, anima::view_forward), {0, -1, 0});
    check_near(rotate(down, {0, 1, 0}), {0, 0, -1});
    CHECK_THROWS_WITH_AS((void)look_rotation({}), invalid_look, anima::MathError);
    CHECK_THROWS_WITH_AS((void)look_rotation({1, 0, 0}, {}), invalid_look, anima::MathError);
    CHECK_THROWS_WITH_AS((void)look_rotation({1, 0, 0}, {0, std::numeric_limits<float>::quiet_NaN(), 0}), invalid_look,
                         anima::MathError);

    // look_at() is the inverse of the rigid placement that look_rotation() gives the eye.
    const anima::Vec3 eye{1, 2, 3}, target{-4, 0, 1}, up{0, 1, 1};
    const auto view = anima::look_at(eye, target, up),
               placed = anima::inverse(anima::matrix({eye, look_rotation(target - eye, up), {1, 1, 1}}));
    for (std::size_t i = 0; i < view.size(); ++i)
        CHECK(view[i] == doctest::Approx(placed[i]));
    // The default up keeps the view matrix of a camera at the origin facing -Z exact.
    CHECK(anima::look_at({}, {0, 0, -1}) == anima::identity());
    CHECK_THROWS_WITH_AS((void)anima::look_at(eye, eye + anima::Vec3{0, 5, 0}), invalid_look, anima::MathError);
    CHECK_THROWS_WITH_AS((void)anima::look_at(eye, eye), invalid_look, anima::MathError);
    check_near(anima::point(anima::look_at(eye, eye + anima::Vec3{0, 5, 0}, {1, 0, 0}), eye + anima::Vec3{0, 1, 0}),
               {0, 0, -1});
}

TEST_CASE("Decomposition recovers translation, rotation and scale, and refuses shear and collapsed axes") {
    const anima::Transform placed{{1, -2, 3}, anima::euler_radians(.3F, .5F, .7F), {2, 3, 4}};
    const auto parts = anima::decompose(anima::matrix(placed));
    REQUIRE(parts);
    CHECK(parts->translation.x == placed.translation.x);
    CHECK(parts->translation.y == placed.translation.y);
    CHECK(parts->translation.z == placed.translation.z);
    check_rotation(parts->rotation, placed.rotation);
    check_near(parts->scale, placed.scale);

    // A reflection becomes a negative X scale, whichever axis the matrix mirrors.
    auto mirrored = placed;
    mirrored.scale.x = -2;
    const auto reflected = anima::decompose(anima::matrix(mirrored));
    REQUIRE(reflected);
    check_rotation(reflected->rotation, placed.rotation);
    check_near(reflected->scale, mirrored.scale);
    auto flipped_z = anima::identity();
    flipped_z[10] = -1;
    const auto flipped = anima::decompose(flipped_z);
    REQUIRE(flipped);
    check_near(flipped->scale, {-1, 1, 1});
    check_near(anima::rotate(flipped->rotation, {0, 0, 1}), {0, 0, -1});

    // Shear beyond a cosine of 1e-5 between columns, a collapsed axis, a projective row or a nonfinite element
    // has no T * R * S form.
    auto sheared = anima::identity();
    sheared[4] = 2e-5F;
    CHECK_FALSE(anima::decompose(sheared));
    sheared[4] = 5e-6F;
    CHECK(anima::decompose(sheared));
    CHECK_FALSE(anima::decompose(anima::matrix({{}, anima::identity_rotation, {1, 0, 1}})));
    auto projective = anima::identity();
    projective[3] = .5F;
    CHECK_FALSE(anima::decompose(projective));
    auto nonfinite = anima::identity();
    nonfinite[12] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(anima::decompose(nonfinite));
}

TEST_CASE("Affine rotation takes the nearest rotation and rejects reflections and collapsed transforms") {
    const auto turn = anima::axis_angle({1, 1, 0}, .9F);
    auto stretched = anima::matrix({{4, 5, 6}, turn, {1, 2, 3}});
    check_rotation(anima::affine_rotation(stretched), turn);
    auto mirrored = anima::identity();
    mirrored[0] = -1;
    CHECK_THROWS_WITH_AS((void)anima::affine_rotation(mirrored), "Affine rotation needs a transform without reflection",
                         std::invalid_argument);
    CHECK(anima::math_error_name(anima::MathErrorCode::collapsed_transform) == "collapsed_transform");
    CHECK_THROWS_WITH_AS((void)anima::affine_rotation(anima::matrix({{}, turn, {1, 0, 1}})),
                         "Affine transform is collapsed", anima::MathError);
    stretched[7] = .5F;
    CHECK_THROWS_WITH_AS((void)anima::affine_rotation(stretched), "Affine rotation needs an affine transform",
                         std::invalid_argument);
    stretched[7] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS((void)anima::affine_rotation(stretched), "Nonfinite affine transform", std::invalid_argument);
}
