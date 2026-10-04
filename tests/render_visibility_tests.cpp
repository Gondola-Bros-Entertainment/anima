#include <anima/assets/render_visibility.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned trials_per_view = 4000;
anima::RenderBounds box(anima::Vec3 low, anima::Vec3 high) { return {low, high, true}; }
// Independent homogeneous-corner oracle: reject only if every corner lies
// beyond the same clip plane. No plane extraction or perspective divide.
bool reference(const anima::Mat4 &view, const anima::RenderBounds &bounds) {
    bool outside[6]{true, true, true, true, true, true};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const double point[]{corner & 1 ? bounds.maximum.x : bounds.minimum.x,
                             corner & 2 ? bounds.maximum.y : bounds.minimum.y,
                             corner & 4 ? bounds.maximum.z : bounds.minimum.z, 1};
        double clip[4]{};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column)
                clip[row] += view[column * 4 + row] * point[column];
        const double distances[]{clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1],
                                 clip[3] - clip[1], clip[2],           clip[3] - clip[2]};
        for (unsigned plane = 0; plane < 6; ++plane)
            outside[plane] &= distances[plane] < 0;
    }
    return std::none_of(std::begin(outside), std::end(outside), [](bool value) { return value; });
}
// Levels of detail with @p errors, in order.
std::vector<anima::DrawLevel> levels(std::initializer_list<float> errors) {
    std::vector<anima::DrawLevel> result;
    for (const auto error : errors)
        result.push_back({0, 0, error});
    return result;
}
anima::Mat4 projection() { return anima::perspective(std::numbers::pi_v<float> / 4, 1.5F, .5F, 20); }
// projection() with its far plane at infinity.
anima::Mat4 infinite_far() {
    auto infinite = projection();
    infinite[10] = -1;
    infinite[14] = -.5F;
    return infinite;
}
// projection() with depth 1 at the near plane and 0 at the far plane: clip z becomes w - z.
anima::Mat4 reversed_depth() {
    auto reverse = projection();
    for (unsigned column = 0; column < 4; ++column)
        reverse[column * 4 + 2] = reverse[column * 4 + 3] - reverse[column * 4 + 2];
    return reverse;
}
} // namespace

TEST_CASE("Boxes beyond one clip plane are rejected and boxes touching the volume are kept") {
    const anima::RenderFrustum unit(anima::identity());
    CHECK(unit.intersects(box({-.2F, -.2F, .2F}, {.2F, .2F, .8F})));
    // Beyond x = -w, x = w, y = -w, y = w, z = 0 and z = w in turn.
    const std::array outside{box({-3, -.2F, .2F}, {-2, .2F, .8F}),  box({2, -.2F, .2F}, {3, .2F, .8F}),
                             box({-.2F, -3, .2F}, {.2F, -2, .8F}),  box({-.2F, 2, .2F}, {.2F, 3, .8F}),
                             box({-.2F, -.2F, -2}, {.2F, .2F, -1}), box({-.2F, -.2F, 2}, {.2F, .2F, 3})};
    for (std::size_t plane = 0; plane < outside.size(); ++plane) {
        CAPTURE(plane);
        CHECK_FALSE(unit.intersects(outside[plane]));
    }
    // Touching each plane in the same order, then enclosing the volume, spanning it and a point on it.
    const std::array kept{box({-2, -.2F, .2F}, {-1, .2F, .8F}),
                          box({1, -.2F, .2F}, {2, .2F, .8F}),
                          box({-.2F, -2, .2F}, {.2F, -1, .8F}),
                          box({-.2F, 1, .2F}, {.2F, 2, .8F}),
                          box({-.2F, -.2F, -1}, {.2F, .2F, 0}),
                          box({-.2F, -.2F, 1}, {.2F, .2F, 2}),
                          box({-100, -100, -100}, {100, 100, 100}),
                          box({-2, -.1F, .5F}, {2, .1F, .5F}),
                          box({0, 0, 0}, {0, 0, 0})};
    for (std::size_t index = 0; index < kept.size(); ++index) {
        CAPTURE(index);
        CHECK(unit.intersects(kept[index]));
    }
}

TEST_CASE("Perspective, infinite-far and reversed-depth projections keep their visible volume") {
    const anima::RenderFrustum perspective(projection());
    CHECK(perspective.intersects(box({-.1F, -.1F, -2}, {.1F, .1F, -1})));
    CHECK_FALSE(perspective.intersects(box({-.1F, -.1F, 1}, {.1F, .1F, 2})));     // Behind the eye.
    CHECK(perspective.intersects(box({-1, -1, -1}, {1, 1, 1})));                  // Around the eye and near plane.
    CHECK_FALSE(perspective.intersects(box({-.1F, -.1F, -22}, {.1F, .1F, -21}))); // Beyond the far plane.
    CHECK(anima::RenderFrustum(infinite_far()).intersects(box({-1, -1, -1e8F}, {1, 1, -1e7F})));
    const anima::RenderFrustum reversed(reversed_depth());
    CHECK(reversed.intersects(box({-.1F, -.1F, -2}, {.1F, .1F, -1})));
    CHECK_FALSE(reversed.intersects(box({-.1F, -.1F, -22}, {.1F, .1F, -21})));
}

TEST_CASE("Unknown views and invalid bounds fail open") {
    const anima::RenderFrustum unit(anima::identity());
    CHECK(unit.intersects({}));
    CHECK(anima::RenderFrustum().intersects(box({1e10F, 0, 0}, {2e10F, 1, 1})));
    auto nonfinite = box({0, 0, 0}, {1, 1, 1});
    nonfinite.minimum.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(unit.intersects(nonfinite));
    CHECK(unit.intersects(box({2, 0, 0}, {1, 1, 1}))); // Minimum above maximum.
}

TEST_CASE("A nonfinite view projection is rejected") {
    auto view = anima::identity();
    view[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_WITH_AS(anima::RenderFrustum(view), "Culling view projection must be finite", std::invalid_argument);
}

TEST_CASE("A float clip contact survives cancellation in the camera rows") {
    // Separate float clip coordinates can touch even when their combined
    // plane is slightly negative in double precision. Its nearly cancelled
    // coefficients must not erase the original arithmetic's rounding margin.
    auto cancelling = anima::identity();
    cancelling[0] = -1e8F;
    cancelling[4] = 1;
    cancelling[12] = -2;
    cancelling[3] = 1e8F;
    const auto contact = box({1, 0, .5F}, {1, 0, .5F});
    volatile float clip_x = cancelling[0] * contact.minimum.x;
    clip_x = clip_x + cancelling[12];
    volatile float clip_w = cancelling[3] * contact.minimum.x;
    clip_w = clip_w + cancelling[15];
    REQUIRE(clip_x + clip_w == 0); // The fixture touches the float clip plane.
    CHECK(anima::RenderFrustum(cancelling).intersects(contact));
}

TEST_CASE("Culling never rejects a box that the homogeneous oracle keeps") {
    std::mt19937 random(7139);
    std::uniform_real_distribution<float> coordinate(-25, 25), size(0, .75F);
    const auto camera = anima::operator*(projection(), anima::look_at({3, 4, 7}, {-2, 1, 0}));
    const std::array views{anima::identity(), projection(), camera, infinite_far(), reversed_depth()};
    unsigned rejected_boxes = 0;
    for (std::size_t view = 0; view < views.size(); ++view) {
        CAPTURE(view);
        const anima::RenderFrustum frustum(views[view]);
        for (unsigned trial = 0; trial < trials_per_view; ++trial) {
            CAPTURE(trial);
            const anima::Vec3 center{coordinate(random), coordinate(random), coordinate(random)};
            const anima::Vec3 extent{size(random), size(random), size(random)};
            const auto bounds = box(center - extent, center + extent);
            if (!frustum.intersects(bounds)) {
                ++rejected_boxes;
                CHECK_FALSE(reference(views[view], bounds));
            }
        }
    }
    // Most random boxes lie outside each view, so the comparison exercises rejection.
    CHECK(rejected_boxes > views.size() * trials_per_view / 2);
}

TEST_CASE("Scaling a large-coordinate view keeps a touching box") {
    // A large translation stresses cancellation; projection scaling must not
    // change visibility or create NaNs by normalizing a degenerate far plane.
    auto translated = anima::identity();
    translated[12] = -100000;
    const auto touching = box({100001, -.1F, .4F}, {100002, .1F, .6F});
    for (float scale : {1e-20F, 1.F, 1e20F}) {
        CAPTURE(scale);
        auto view = translated;
        for (auto &entry : view)
            entry *= scale;
        CHECK(anima::RenderFrustum(view).intersects(touching));
    }
}

TEST_CASE("Distances measure to the nearest and the farthest point of the bounds") {
    const auto bounds = box({1, 2, 3}, {4, 6, 8});
    CHECK(anima::nearest_distance({0, 0, 0}, bounds) == std::sqrt(14.0));
    CHECK(anima::farthest_distance({0, 0, 0}, bounds) == std::sqrt(116.0));
    CHECK(anima::nearest_distance({2, 3, 10}, bounds) == 2); // Above the maximum along one axis only.
    // From inside, the nearest point is the eye and the farthest the corner that lies farthest along every axis.
    CHECK(anima::nearest_distance({2, 3, 4}, bounds) == 0);
    CHECK(anima::farthest_distance({2, 3, 4}, bounds) == std::sqrt(29.0));
    // Invalid bounds may hold any point.
    CHECK(anima::nearest_distance({5, 5, 5}, {}) == 0);
    CHECK(anima::farthest_distance({5, 5, 5}, {}) == std::numeric_limits<double>::infinity());
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    const auto endless = box({-infinity, 0, 0}, {infinity, 1, 1});
    CHECK(anima::nearest_distance({5, 3, 0}, endless) == 2);
    CHECK(anima::farthest_distance({5, 3, 0}, endless) == std::numeric_limits<double>::infinity());
    CHECK(std::isnan(anima::nearest_distance({infinity, 0, 0}, endless))); // At the maximum's infinity.
    auto nonfinite = bounds;
    nonfinite.maximum.y = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(anima::nearest_distance({0, 0, 0}, nonfinite)));
    CHECK(std::isnan(anima::farthest_distance({0, 0, 0}, nonfinite)));
}

TEST_CASE("Visibility ranges keep bounds that may hold a distance from the begin up to the end") {
    const anima::VisibilityRange range{2, 10, 0, 0};
    const auto at = [](float x) { return box({x, 0, 0}, {x, 0, 0}); };
    CHECK(anima::within_visibility_range(range, {}, at(2)));
    CHECK_FALSE(anima::within_visibility_range(range, {}, at(std::nextafter(2.F, 0.F))));
    CHECK(anima::within_visibility_range(range, {}, at(std::nextafter(10.F, 0.F))));
    CHECK_FALSE(anima::within_visibility_range(range, {}, at(10)));
    // Bounds spanning the begin or the end are kept, and so are bounds around the eye that reach the begin.
    CHECK(anima::within_visibility_range(range, {}, box({1, 0, 0}, {3, 0, 0})));
    CHECK(anima::within_visibility_range(range, {}, box({9, 0, 0}, {11, 0, 0})));
    CHECK(anima::within_visibility_range(range, {}, box({-5, -5, -5}, {5, 5, 5})));
    CHECK_FALSE(anima::within_visibility_range(range, {}, box({-1, -1, -1}, {1, 1, 1})));
    // An endless range culls only by its begin.
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    const anima::VisibilityRange endless{2, infinity, 0, 0};
    CHECK(anima::within_visibility_range(endless, {}, at(std::numeric_limits<float>::max())));
    CHECK_FALSE(anima::within_visibility_range(endless, {}, at(1)));
    CHECK(anima::within_visibility_range({}, {}, at(0)));
    // Invalid bounds and NaN distances are kept.
    CHECK(anima::within_visibility_range(range, {}, {}));
    auto nonfinite = at(20);
    nonfinite.minimum.z = std::numeric_limits<float>::quiet_NaN();
    CHECK(anima::within_visibility_range(range, {}, nonfinite));
}

TEST_CASE("The chosen level of detail is the last whose projected error fits the threshold") {
    // At 100 pixels per unit of error, a scale of 2 and a distance of 10, the errors cover 5, 10 and 20 pixels exactly.
    const auto chain = levels({.25F, .5F, 1});
    const auto choose = [&](float threshold, double distance = 10) {
        return anima::lod_level(chain, distance, 2, 100, true, threshold);
    };
    CHECK(choose(10) == 1U); // An error that covers the threshold exactly fits.
    CHECK(choose(std::nextafter(10.F, 0.F)) == 0U);
    CHECK(choose(20) == 2U);
    CHECK(choose(1e6F) == 2U);
    CHECK(choose(4) == std::nullopt);
    // A threshold that is not positive, or no levels, draws the full draw.
    CHECK(choose(0) == std::nullopt);
    CHECK(choose(-1) == std::nullopt);
    CHECK(choose(std::numeric_limits<float>::quiet_NaN()) == std::nullopt);
    CHECK(anima::lod_level({}, 10, 2, 100, true, 1e6F) == std::nullopt);
    // From inside the bounds, at a distance of 0, the full draw is drawn, and infinitely far the coarsest level.
    CHECK(choose(1e6F, 0) == std::nullopt);
    CHECK(choose(1e6F, -1) == std::nullopt);
    CHECK(choose(1e6F, std::numeric_limits<double>::quiet_NaN()) == std::nullopt);
    CHECK(choose(0.001F, std::numeric_limits<double>::infinity()) == 2U);
    // The choice stops at the first level that does not fit, and a NaN product does not.
    CHECK(anima::lod_level(levels({.25F, 1, .5F}), 10, 2, 100, true, 10) == 0U);
    CHECK(anima::lod_level(chain, 10, std::numeric_limits<double>::quiet_NaN(), 100, true, 10) == std::nullopt);
}

TEST_CASE("An orthographic view chooses the level of detail without distance") {
    // Without the division by distance, the errors cover 50, 100 and 200 pixels.
    const auto chain = levels({.25F, .5F, 1});
    for (const double distance : {10.0, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(distance);
        CHECK(anima::lod_level(chain, distance, 2, 100, false, 100) == 1U);
        CHECK(anima::lod_level(chain, distance, 2, 100, false, 99) == 0U);
        CHECK(anima::lod_level(chain, distance, 2, 100, false, 49) == std::nullopt);
    }
}
