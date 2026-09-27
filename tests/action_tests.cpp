#include "near.hpp"
#include <anima/assets/action.hpp>
#include <anima/assets/action_runtime.hpp>
#include <doctest/doctest.h>

#include <limits>
#include <stdexcept>

using namespace anima;
namespace {
constexpr double time_tolerance = 1e-12; // Timeline arithmetic is exact up to rounding.
constexpr auto invalid_time = "Invalid action presentation/release time";
constexpr auto invalid_phase = "Invalid/duplicate action phase or multiple held phases";
constexpr auto invalid_weight = "Action weight requires a finite phase and 2..32 keys covering 0..1";
constexpr auto invalid_keys = "Action weight keys must be ordered and normalized";

// A preparation, a held phase, a release and a settle; each of the first three has a cue at its start.
ActionTimeline gesture() {
    return ActionTimeline({{"prepare", .2, false, {{"begin", 0}}},
                           {"hold", .4, true, {{"sustain", 0}}},
                           {"release", .3, false, {{"emit", 0}}},
                           {"settle", .2, false, {}}});
}
} // namespace

TEST_CASE("A timeline loops its held phase and retimes the phases after release") {
    const auto timeline = gesture();
    CHECK(timeline.sample(.1).phase == 0);
    const auto held = timeline.sample(.7);
    CHECK(held.phase == 1);
    CHECK(held.cycle == 1);
    CHECK(held.progress == Near{.25, time_tolerance});
    const auto released = timeline.sample(.9, .85);
    CHECK(released.phase == 2);
    CHECK(released.progress == Near{1. / 6, time_tolerance});
    // A release during preparation finishes preparing and skips the hold.
    CHECK(timeline.sample(.21, .1).phase == 2);
    CHECK(timeline.sample(1.36, .85).complete);
    const ActionTimeline held_last({{"hold", .3, true, {}}});
    CHECK(held_last.sample(1., .8).elapsed == Near{.8, time_tolerance});
}

TEST_CASE("A timeline without a held phase has a fixed duration") {
    const ActionTimeline timed({{"one", .3, false, {}}, {"two", .7, false, {}}});
    CHECK(timed.duration() == 1);
    CHECK(timed.sample(1).complete);
}

TEST_CASE("A cue cursor reports each cue of an instance once") {
    const auto timeline = gesture();
    ActionCueCursor cursor;
    CHECK(cursor.advance("gesture", 1, timeline, 0).size() == 1);
    CHECK(cursor.advance("gesture", 1, timeline, .6).size() == 1); // The hold's cue.
    CHECK(cursor.advance("gesture", 1, timeline, .9, .85).at(0).id == "emit");
    CHECK(cursor.advance("gesture", 1, timeline, .9, .85).empty()); // A repeated frame.
    // A new instance starts at 0, so its first frame reports the cues it covers wherever it lands.
    const auto first_frame = cursor.advance("gesture", 2, timeline, .016);
    REQUIRE(first_frame.size() == 1);
    CHECK(first_frame[0].id == "begin");
    CHECK(cursor.advance("gesture", 3, timeline, .9, .85).size() == 3);
    // An observer that joins an instance under way seeks first, so history is not replayed.
    cursor.seek("gesture", 4, timeline, .9, .85);
    CHECK(cursor.advance("gesture", 4, timeline, .9, .85).empty());
    CHECK(cursor.advance("gesture", 4, timeline, .1).empty()); // A rewind.
    cursor.reset();
    cursor.seek("gesture", 5, timeline, 4.);
    CHECK(cursor.advance("gesture", 5, timeline, 4.).empty()); // A long hold has no catch-up cues.
    CHECK(cursor.advance("gesture", 5, timeline, 4.1, 4.05).size() == 1);
}

TEST_CASE("Invalid timelines and times are rejected") {
    const auto timeline = gesture();
    CHECK_THROWS_WITH_AS(timeline.sample(-1), invalid_time, std::invalid_argument);
    CHECK_THROWS_WITH_AS(timeline.sample(0, std::numeric_limits<double>::infinity()), invalid_time,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(timeline.duration(), "Held action has no fixed duration", std::logic_error);
    CHECK_THROWS_WITH_AS(ActionTimeline({{"same", 1, false, {}}, {"same", 1, false, {}}}), invalid_phase,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(ActionTimeline({{"a", 1, true, {}}, {"b", 1, true, {}}}), invalid_phase,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(ActionTimeline({{"a", 1, false, {{"late", .8}, {"early", .2}}}}),
                         "Invalid, unordered or excessive action cues", std::invalid_argument);
    // A release time needs a held phase to release.
    const ActionTimeline timed({{"one", .3, false, {}}, {"two", .7, false, {}}});
    CHECK_THROWS_WITH_AS(timed.sample(.1, .1), invalid_time, std::invalid_argument);
}

TEST_CASE("An action weight interpolates its keys and clamps outside them") {
    ActionWeight weight;
    weight.keys = {{0., 0.F}, {.5, 1.F}, {1., 0.F}};
    CHECK(weight.sample(.25) == .5F);
    CHECK(weight.sample(-1) == 0);
    CHECK(weight.sample(2) == 0);
}

TEST_CASE("Invalid action weight phases and keys are rejected") {
    ActionWeight weight;
    weight.keys = {{0., 0.F}, {.5, 1.F}, {1., 0.F}};
    CHECK_THROWS_WITH_AS(weight.sample(std::numeric_limits<double>::quiet_NaN()), invalid_weight,
                         std::invalid_argument);
    weight.keys.clear();
    CHECK_THROWS_WITH_AS(weight.sample(.5), invalid_weight, std::invalid_argument);
    weight.keys = {{0., 0.F}, {0., 1.F}, {1., 0.F}};
    CHECK_THROWS_WITH_AS(weight.sample(.5), invalid_keys, std::invalid_argument);
    weight.keys = {{0., 0.F}, {1., std::numeric_limits<float>::infinity()}};
    CHECK_THROWS_WITH_AS(weight.sample(.5), invalid_keys, std::invalid_argument);
}
