#include <anima/assets/action.hpp>
#include <anima/assets/action_runtime.hpp>
#include <iostream>
#include <limits>
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid timeline accepted");
}
} // namespace
int main() {
    try {
        using namespace anima;
        ActionTimeline timeline({{"windup", .2, false, {{"begin", 0}}},
                                 {"hold", .4, true, {{"sustain", 0}}},
                                 {"release", .3, false, {{"fire", 0}}},
                                 {"recover", .2, false, {}}});
        check(timeline.sample(.1).phase == 0, "Wind-up timing");
        const auto held = timeline.sample(.7);
        check(held.phase == 1 && held.cycle == 1 && std::abs(held.progress - .25) < 1e-12, "Held clip clock");
        const auto released = timeline.sample(.9, .85);
        check(released.phase == 2 && std::abs(released.progress - 1. / 6) < 1e-12, "Release retiming");
        check(timeline.sample(.21, .1).phase == 2, "Early release must finish wind-up and skip hold");
        check(timeline.sample(1.36, .85).complete, "Released action completion");
        ActionTimeline held_last({{"hold", .3, true, {}}});
        check(std::abs(held_last.sample(1., .8).elapsed - .8) < 1e-12, "Held final phase duration");
        ActionCueCursor cursor;
        check(cursor.advance("shot", 1, timeline, 0).size() == 1, "Initial cue missing");
        check(cursor.advance("shot", 1, timeline, .6).size() == 1, "Hold entry cue missing");
        check(cursor.advance("shot", 1, timeline, .9, .85).at(0).id == "fire", "Release cue missing");
        check(cursor.advance("shot", 1, timeline, .9, .85).empty(), "Repeated frame duplicated cue");
        // A new instance starts at 0, so its first frame reports the cues it covers wherever it lands.
        const auto first_frame = cursor.advance("shot", 2, timeline, .016);
        check(first_frame.size() == 1 && first_frame[0].id == "begin", "A first frame after 0 dropped the start cue");
        check(cursor.advance("shot", 3, timeline, .9, .85).size() == 3, "A first frame dropped cues since the start");
        // An observer that joins an instance under way seeks first, so history is not replayed.
        cursor.seek("shot", 4, timeline, .9, .85);
        check(cursor.advance("shot", 4, timeline, .9, .85).empty(), "Late join replayed historical cues");
        check(cursor.advance("shot", 4, timeline, .1).empty(), "Rewind emitted cues");
        cursor.reset();
        cursor.seek("shot", 5, timeline, 4.);
        check(cursor.advance("shot", 5, timeline, 4.).empty(), "Long hold emitted catch-up cues");
        check(cursor.advance("shot", 5, timeline, 4.1, 4.05).size() == 1, "Late-held release missing");
        rejects([&] { (void)timeline.sample(-1); });
        rejects([&] { (void)timeline.sample(0, std::numeric_limits<double>::infinity()); });
        rejects([&] { (void)timeline.duration(); });
        rejects([] { (void)ActionTimeline({{"same", 1, false, {}}, {"same", 1, false, {}}}); });
        rejects([] { (void)ActionTimeline({{"a", 1, true, {}}, {"b", 1, true, {}}}); });
        rejects([] { (void)ActionTimeline({{"a", 1, false, {{"late", .8}, {"early", .2}}}}); });
        ActionTimeline timed({{"one", .3, false, {}}, {"two", .7, false, {}}});
        check(timed.duration() == 1 && timed.sample(1).complete, "Timed duration");
        rejects([&] { (void)timed.sample(.1, .1); });
        ActionWeight weight;
        weight.keys = {{0., 0.F}, {.5, 1.F}, {1., 0.F}};
        check(weight.sample(.25) == .5F && weight.sample(-1) == 0 && weight.sample(2) == 0,
              "Action weight interpolation/clamping failed");
        rejects([&] { (void)weight.sample(std::numeric_limits<double>::quiet_NaN()); });
        weight.keys.clear();
        rejects([&] { (void)weight.sample(.5); });
        weight.keys = {{0., 0.F}, {0., 1.F}, {1., 0.F}};
        rejects([&] { (void)weight.sample(.5); });
        weight.keys = {{0., 0.F}, {1., std::numeric_limits<float>::infinity()}};
        rejects([&] { (void)weight.sample(.5); });
        std::cout << "PASS action phase, release, completion, seek and cue contracts\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
