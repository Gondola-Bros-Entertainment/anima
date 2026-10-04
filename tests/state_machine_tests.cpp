#include "near.hpp"
#include <anima/animation_state_machine.hpp>
#include <anima/prefab.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
using Machine = AnimationStateMachine;
using ParameterType = Machine::ParameterType;
using ConditionMode = Machine::ConditionMode;
namespace {
constexpr double pose_tolerance = 1e-5; // Absolute error allowed in pose translations and weights.

// One root node, one triangle, and clips that move the root along +X: idle stays at 0, walk moves 1 unit over its
// 1 s, run moves 2 units over its 0.5 s, and jump (1 s) and hit (0.5 s) hold at 10 and -8. The poses aim_low,
// aim_high and guard, clips of zero duration, hold it at -3, 3 and 6.
std::shared_ptr<const Asset> clip_asset() {
    auto asset = std::make_shared<Asset>();
    asset->nodes.resize(1);
    asset->nodes[0].name = "root";
    Material surface;
    surface.name = "surface";
    asset->materials.push_back(surface);
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset->primitives.push_back(primitive);
    const auto add_clip = [&](const char *name, std::vector<double> times, std::vector<float> positions) {
        Animation clip;
        clip.name = name;
        clip.duration = times.back();
        AnimationChannel channel{0, ChannelPath::translation, Interpolation::linear, std::move(times), {}};
        for (const auto x : positions)
            channel.values.push_back({x, 0, 0, 0});
        clip.channels.push_back(std::move(channel));
        asset->animations.push_back(std::move(clip));
    };
    add_clip("idle", {0, 1}, {0, 0});
    add_clip("walk", {0, 1}, {0, 1});
    add_clip("run", {0, .5}, {0, 2});
    add_clip("jump", {0, 1}, {10, 10});
    add_clip("hit", {0, .5}, {-8, -8});
    add_clip("aim_low", {0}, {-3});
    add_clip("aim_high", {0}, {3});
    add_clip("guard", {0}, {6});
    return asset;
}
std::vector<ClipMetadata> clip_policies() {
    return {{"idle", true, {}, {}},
            {"walk", true, {{.5, "step"}}, {}},
            {"run", true, {{.25, "stride"}}, {}},
            {"jump", false, {{0, "takeoff"}, {1, "land"}}, {}},
            {"hit", false, {}, {}},
            {"aim_low", false, {{0, "aimed"}}, {}},
            {"aim_high", false, {}, {}},
            {"guard", true, {{0, "guard"}}, {}}};
}
Machine::Parameter parameter(std::string name, ParameterType type, double initial = 0) {
    Machine::Parameter result;
    result.name = std::move(name);
    result.type = type;
    result.initial = initial;
    return result;
}
Machine::State clip_state(std::string name, std::string clip) {
    Machine::State result;
    result.name = std::move(name);
    result.clip = std::move(clip);
    return result;
}
Machine::Condition condition(std::string name, ConditionMode mode, double threshold = 0) {
    Machine::Condition result;
    result.parameter = std::move(name);
    result.mode = mode;
    result.threshold = threshold;
    return result;
}
Machine::Transition transition(std::optional<std::string> from, std::string to,
                               std::vector<Machine::Condition> conditions, double duration = 0) {
    Machine::Transition result;
    result.from = std::move(from);
    result.to = std::move(to);
    result.conditions = std::move(conditions);
    result.duration = duration;
    return result;
}
// Parameters and one state per clip, with no transitions; @p first is the entry state.
Machine::Definition states(std::string_view first = "idle") {
    Machine::Definition definition;
    definition.parameters = {
        parameter("speed", ParameterType::real),          parameter("stance", ParameterType::integer),
        parameter("grounded", ParameterType::boolean, 1), parameter("ready", ParameterType::boolean),
        parameter("jump", ParameterType::trigger),        parameter("go", ParameterType::trigger),
        parameter("stop", ParameterType::trigger)};
    definition.states.push_back(clip_state(std::string(first), std::string(first)));
    for (const auto *name : {"idle", "walk", "run", "jump", "hit"})
        if (name != first)
            definition.states.push_back(clip_state(name, name));
    return definition;
}
// An object posed by the clip asset, and the machine that animates it.
struct Actor {
    std::shared_ptr<const Asset> asset = clip_asset();
    Scene scene;
    GameObject object = scene.create("actor", Mesh::compile(*asset));
    std::shared_ptr<const Machine> machine;
    ComponentRef<StateMachineAnimator> animator;
    explicit Actor(Machine::Definition definition)
        : machine(std::make_shared<const Machine>(asset, clip_policies(), std::move(definition))),
          animator(object.add_component<StateMachineAnimator>(machine)) {}
    // Root translation along X of the published pose, checked against what the renderer holds.
    float x() const {
        const auto published = animator->pose().world.at(0)[12];
        CHECK(scene.instance(object.id()).palette.at(0)[12] == published);
        return published;
    }
};
std::string changed(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
} // namespace

TEST_CASE("A machine starts in its first state and publishes each update through the renderer") {
    Actor actor(states("walk"));
    auto &animator = actor.animator;
    CHECK(animator->state() == "walk");
    CHECK(animator->time() == 0);
    CHECK_FALSE(animator->crossfade());
    CHECK(actor.x() == 0);
    actor.scene.update(.25);
    CHECK(animator->time() == .25);
    CHECK(actor.x() == Near{.25, pose_tolerance});
    CHECK(animator->events().empty());
    actor.scene.update(.5);
    REQUIRE(animator->events().size() == 1);
    const auto &step = animator->events()[0];
    CHECK(step.offset == .25);
    CHECK(step.state == "walk");
    CHECK(step.clip == "walk");
    CHECK(step.event.name == "step");
    CHECK(step.event.time == .5);
    CHECK(step.weight == 1);
    actor.scene.fixed_update(.02);
    CHECK(animator->time() == .75);
    actor.scene.update(.5);
    CHECK(animator->time() == 1.25);
    CHECK(actor.x() == Near{.25, pose_tolerance}); // A looping state wraps its clip.
}

TEST_CASE("Conditions on float, integer, bool and trigger parameters start transitions in priority order") {
    auto definition = states();
    definition.transitions = {
        transition({}, "hit", {condition("stance", ConditionMode::equals, 2)}),
        transition({}, "jump", {condition("jump", ConditionMode::is_true)}),
        transition("idle", "walk", {condition("speed", ConditionMode::greater, .5)}),
        transition("walk", "idle", {condition("speed", ConditionMode::less, .5)}),
        transition("walk", "run",
                   {condition("speed", ConditionMode::greater, 2.5), condition("grounded", ConditionMode::is_true)}),
        transition("run", "walk", {condition("speed", ConditionMode::less, 2.5)}),
        transition("hit", "idle", {condition("stance", ConditionMode::not_equal, 2)}),
        transition("idle", "run",
                   {condition("grounded", ConditionMode::is_false), condition("stance", ConditionMode::greater, 0)}),
        transition("run", "idle", {condition("stance", ConditionMode::less, -5)}),
    };
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    (void)animator->update(.1);
    CHECK(animator->state() == "idle");

    // A zero-duration transition switches at the start of the update, which then advances the new state.
    animator->set_float("speed", 1);
    (void)animator->update(.125);
    CHECK(animator->state() == "walk");
    CHECK(animator->time() == .125);
    animator->set_float("speed", 3);
    animator->set_bool("grounded", false);
    (void)animator->update(.125);
    CHECK(animator->state() == "walk"); // Every condition must hold.
    animator->set_bool("grounded", true);
    (void)animator->update(.125);
    CHECK(animator->state() == "run");
    animator->set_float("speed", 1);
    (void)animator->update(.125);
    CHECK(animator->state() == "walk");

    // A trigger starts one transition and is reset by it.
    animator->set_trigger("jump");
    CHECK(animator->get_bool("jump"));
    (void)animator->update(.25);
    CHECK(animator->state() == "jump");
    CHECK_FALSE(animator->get_bool("jump"));
    (void)animator->update(.25);
    CHECK(animator->state() == "jump");
    CHECK(animator->time() == .5);
    // A transition from any state skips its own target, so the trigger stays set.
    animator->set_trigger("jump");
    (void)animator->update(.25);
    CHECK(animator->time() == .75);
    CHECK(animator->get_bool("jump"));
    animator->reset_trigger("jump");
    CHECK_FALSE(animator->get_bool("jump"));

    // The earlier transition from any state wins, and the later one leaves its trigger set.
    animator->set_integer("stance", 2);
    animator->set_trigger("jump");
    (void)animator->update(.125);
    CHECK(animator->state() == "hit");
    CHECK(animator->get_integer("stance") == 2);
    CHECK(animator->get_bool("jump"));
    animator->reset_trigger("jump");
    animator->set_integer("stance", 1);
    (void)animator->update(.125);
    CHECK(animator->state() == "idle");
    animator->set_float("speed", 0);
    animator->set_bool("grounded", false);
    (void)animator->update(.125);
    CHECK(animator->state() == "run");
    CHECK(animator->get_float("speed") == 0);
    CHECK_FALSE(animator->get_bool("grounded"));
    animator->set_float("speed", 3);
    animator->set_integer("stance", -6);
    (void)animator->update(.125);
    CHECK(animator->state() == "idle");
}

TEST_CASE("An exit time starts its transition in the update after its state crosses it") {
    auto definition = states("walk");
    definition.parameters.push_back(parameter("clear", ParameterType::boolean));
    definition.transitions = {
        transition("walk", "idle", {condition("ready", ConditionMode::is_true)}),
        transition("idle", "jump", {condition("go", ConditionMode::is_true)}),
        transition("jump", "run", {}),
        transition("run", "walk", {condition("clear", ConditionMode::is_true)}),
    };
    definition.transitions[0].exit_time = .75;
    definition.transitions[2].exit_time = 1;
    definition.transitions[3].exit_time = 2.5;
    Actor actor(std::move(definition));
    auto &animator = actor.animator;

    // Below 1, a looping state crosses the exit time once per loop, and the conditions must hold then.
    (void)animator->update(.5);
    (void)animator->update(.5); // Crosses 0.75 while ready is false.
    (void)animator->update(.25);
    CHECK(animator->state() == "walk");
    animator->set_bool("ready", true);
    (void)animator->update(.25);
    (void)animator->update(.25); // Crosses 1.75.
    CHECK(animator->state() == "walk");
    CHECK(animator->time() == 1.75);
    (void)animator->update(.25);
    CHECK(animator->state() == "idle");
    CHECK(animator->time() == .25);

    // A state that does not loop crosses 1 once, as it reaches its end.
    animator->set_trigger("go");
    (void)animator->update(.5);
    CHECK(animator->state() == "jump");
    (void)animator->update(.5);
    CHECK(animator->state() == "jump");
    CHECK(animator->time() == 1);
    (void)animator->update(.125);
    CHECK(animator->state() == "run");
    CHECK(animator->time() == .25);

    // From 1 up, a looping state crosses the exit time only once.
    (void)animator->update(.5);
    (void)animator->update(.5);
    (void)animator->update(.25); // Crosses 2.5.
    (void)animator->update(.25); // Checks the crossing while clear is false.
    CHECK(animator->time() == 3.25);
    animator->set_bool("clear", true);
    for (int loop = 0; loop < 8; ++loop)
        (void)animator->update(.25);
    CHECK(animator->state() == "run");
    CHECK(animator->time() == 7.25);
}

TEST_CASE("A transition from any state restarts its own target only when it may transition to itself") {
    for (const bool restarts : {false, true}) {
        CAPTURE(restarts);
        auto definition = states("walk");
        definition.transitions = {transition({}, "jump", {condition("jump", ConditionMode::is_true)})};
        definition.transitions[0].to_self = restarts;
        Actor actor(std::move(definition));
        actor.animator->set_trigger("jump");
        (void)actor.animator->update(.5);
        CHECK(actor.animator->state() == "jump");
        actor.animator->set_trigger("jump");
        (void)actor.animator->update(.25);
        CHECK(actor.animator->time() == (restarts ? .25 : .75));
        CHECK(actor.animator->get_bool("jump") == !restarts);
    }
}

TEST_CASE("A crossfade blends the poses of the state it leaves and the state it enters by elapsed time") {
    auto definition = states("walk");
    definition.transitions = {transition("walk", "run", {condition("go", ConditionMode::is_true)}, .5)};
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    animator->set_trigger("go");
    // Walk's X is its time; run's is 4 times its clip time, and run advances 2 normalized units a second.
    const std::vector<std::pair<double, float>> expected{
        {.125, .75F * .125F + .25F * .5F}, {.25, .5F * .25F + .5F * 1.F}, {.375, .25F * .375F + .75F * 1.5F}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        (void)animator->update(.125);
        const auto fade = animator->crossfade();
        REQUIRE(fade);
        CHECK(fade->transition == 0);
        CHECK(fade->source == "walk");
        CHECK(fade->source_time == expected[i].first);
        CHECK(fade->elapsed == expected[i].first);
        CHECK(fade->weight == static_cast<float>(expected[i].first / .5));
        CHECK(animator->state() == "run");
        CHECK(animator->time() == 2 * expected[i].first);
        CHECK(actor.x() == Near{expected[i].second, pose_tolerance});
    }
    (void)animator->update(.125);
    CHECK_FALSE(animator->crossfade());
    CHECK(animator->time() == 1);
    CHECK(actor.x() == Near{0, pose_tolerance});
}

TEST_CASE("A crossfade reports the events of both clips in time order, with their weights") {
    auto definition = states("walk");
    definition.transitions = {transition("walk", "run", {condition("go", ConditionMode::is_true)}, 1)};
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    animator->play("walk", .125);
    animator->set_trigger("go");
    const auto events = animator->update(.5);
    REQUIRE(events.size() == 2);
    // Run crosses its stride (clip time 0.25 of 0.5 s) 0.25 s in; walk crosses its step at 0.5 after 0.375 s.
    CHECK(events[0].clip == "run");
    CHECK(events[0].event.name == "stride");
    CHECK(events[0].offset == .25);
    CHECK(events[0].weight == .25F);
    CHECK(events[1].clip == "walk");
    CHECK(events[1].event.name == "step");
    CHECK(events[1].offset == .375);
    CHECK(events[1].weight == .625F);

    // Events at the same offset keep the outgoing state's first.
    animator->play("walk", .25);
    animator->set_trigger("go");
    const auto tied = animator->update(.5);
    REQUIRE(tied.size() == 2);
    CHECK(tied[0].clip == "walk");
    CHECK(tied[1].clip == "run");
    CHECK(tied[0].offset == tied[1].offset);

    // The state left stops reporting once the crossfade ends.
    animator->play("walk");
    animator->set_trigger("go");
    CHECK(animator->update(.75).size() == 3); // Walk's step at 0.5 s and run's strides at 0.25 s and 0.75 s.
    const auto later = animator->update(1);
    CHECK_FALSE(animator->crossfade());
    // Run's strides at 0.5 s and 1 s; walk's next step, 0.75 s in, would follow the crossfade's end at 0.25 s.
    REQUIRE(later.size() == 2);
    CHECK(later[0].clip == "run");
    CHECK(later[1].clip == "run");
    CHECK(later[1].offset == 1);
    CHECK(later[1].weight == 1);
}

TEST_CASE("Events at a state's entry point are reported once, by its first advance") {
    auto definition = states("jump");
    definition.transitions = {transition("jump", "walk", {condition("go", ConditionMode::is_true)})};
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    CHECK(animator->update(0).empty()); // A zero step advances nothing.
    const auto first = animator->update(.5);
    REQUIRE(first.size() == 1);
    CHECK(first[0].event.name == "takeoff");
    CHECK(first[0].offset == 0);
    const auto landing = animator->update(.75);
    REQUIRE(landing.size() == 1);
    CHECK(landing[0].event.name == "land");
    CHECK(landing[0].offset == .5);
    CHECK(animator->update(1).empty()); // A state that does not loop holds its end.
    CHECK(actor.x() == Near{10, pose_tolerance});
    animator->play("jump");
    CHECK(animator->update(.25).size() == 1);
}

TEST_CASE("A crossfade is interrupted only as its interruption rule admits") {
    const auto machine_with = [](Machine::Interruption rule) {
        auto definition = states("walk");
        definition.parameters.push_back(parameter("halt", ParameterType::trigger));
        definition.parameters.push_back(parameter("leap", ParameterType::trigger));
        definition.transitions = {
            transition({}, "hit", {condition("stance", ConditionMode::equals, 9)}, .25),
            transition("walk", "idle", {condition("halt", ConditionMode::is_true)}, .5),
            transition("walk", "run", {condition("go", ConditionMode::is_true)}, 1),
            transition("walk", "jump", {condition("leap", ConditionMode::is_true)}, .5),
            transition("run", "jump", {condition("halt", ConditionMode::is_true)}, .5),
            transition("run", "idle", {condition("stop", ConditionMode::is_true)}, .5),
        };
        definition.transitions[2].interruption = rule;
        return definition;
    };
    const auto start = [](Actor &actor) {
        actor.animator->set_trigger("go");
        (void)actor.animator->update(.25);
        REQUIRE(actor.animator->crossfade());
        CHECK(actor.animator->crossfade()->transition == 2);
    };

    SUBCASE("none") {
        Actor actor(machine_with(Machine::Interruption::none));
        start(actor);
        actor.animator->set_trigger("stop");
        actor.animator->set_trigger("halt");
        actor.animator->set_integer("stance", 9);
        (void)actor.animator->update(.25);
        CHECK(actor.animator->crossfade()->transition == 2);
        (void)actor.animator->update(.5); // The crossfade ends; then transitions from run apply.
        CHECK_FALSE(actor.animator->crossfade());
        (void)actor.animator->update(0);
        CHECK(actor.animator->state() == "hit");
    }
    SUBCASE("source") {
        Actor actor(machine_with(Machine::Interruption::source));
        start(actor);
        actor.animator->set_trigger("leap"); // walk -> jump follows walk -> run in priority.
        actor.animator->set_trigger("stop"); // run -> idle leaves the destination.
        (void)actor.animator->update(.25);
        CHECK(actor.animator->crossfade()->transition == 2);
        actor.animator->set_trigger("halt"); // walk -> idle precedes it.
        (void)actor.animator->update(0);
        const auto fade = actor.animator->crossfade();
        REQUIRE(fade);
        CHECK(fade->transition == 1);
        CHECK(fade->source == "walk");
        CHECK_FALSE(fade->source_time);
        CHECK(actor.animator->state() == "idle");
    }
    SUBCASE("destination") {
        Actor actor(machine_with(Machine::Interruption::destination));
        start(actor);
        actor.animator->set_trigger("halt"); // Only run -> jump leaves the destination.
        (void)actor.animator->update(0);
        REQUIRE(actor.animator->crossfade());
        CHECK(actor.animator->crossfade()->transition == 4);
        CHECK(actor.animator->crossfade()->source == "run");
        CHECK(actor.animator->state() == "jump");
    }
    SUBCASE("source then destination") {
        Actor actor(machine_with(Machine::Interruption::source_then_destination));
        start(actor);
        actor.animator->set_trigger("halt");
        (void)actor.animator->update(0);
        CHECK(actor.animator->crossfade()->transition == 1);
    }
    SUBCASE("destination then source") {
        Actor actor(machine_with(Machine::Interruption::destination_then_source));
        start(actor);
        actor.animator->set_trigger("halt");
        (void)actor.animator->update(0);
        CHECK(actor.animator->crossfade()->transition == 4);
    }
    SUBCASE("from any state") {
        Actor actor(machine_with(Machine::Interruption::destination));
        start(actor);
        actor.animator->set_integer("stance", 9);
        (void)actor.animator->update(0);
        CHECK(actor.animator->crossfade()->transition == 0);
        CHECK(actor.animator->crossfade()->source == "run");
        // Once it ends, its condition still holds, but its target is the state the machine plays.
        (void)actor.animator->update(.25);
        CHECK_FALSE(actor.animator->crossfade());
        (void)actor.animator->update(.125);
        CHECK(actor.animator->state() == "hit");
        CHECK_FALSE(actor.animator->crossfade());
        CHECK(actor.animator->time() == .75);
    }
}

TEST_CASE("The pose stays continuous through a crossfade and its interruption") {
    auto definition = states("walk");
    definition.transitions = {
        transition("walk", "jump", {condition("go", ConditionMode::is_true)}, .5),
        transition("jump", "hit", {condition("stop", ConditionMode::is_true)}, .5),
    };
    definition.transitions[0].interruption = Machine::Interruption::destination;
    Actor actor(definition);
    auto &animator = actor.animator;
    constexpr double step = 1. / 512;
    float previous = actor.x(), largest = 0;
    for (int frame = 0; frame < 512; ++frame) {
        if (frame == 64)
            animator->set_trigger("go");
        if (frame == 192) {
            // Mid-crossfade, the interrupting crossfade starts from the pose last published.
            animator->set_trigger("stop");
            (void)animator->update(0);
            CHECK_FALSE(animator->crossfade()->source_time);
            CHECK(actor.x() == Near{previous, pose_tolerance});
        }
        (void)animator->update(step);
        largest = std::max(largest, std::abs(actor.x() - previous));
        previous = actor.x();
    }
    CHECK(animator->state() == "hit");
    CHECK(previous == Near{-8, pose_tolerance});
    // Walk moves 1/512 per frame and each crossfade at most 18 units over 256 frames.
    CHECK(largest < .08F);

    // Without a crossfade the same switch jumps by the whole difference.
    definition.transitions[0].duration = 0;
    Actor instant(std::move(definition));
    (void)instant.animator->update(.25);
    const auto before = instant.x();
    instant.animator->set_trigger("go");
    (void)instant.animator->update(step);
    CHECK(std::abs(instant.x() - before) > 9);
}

TEST_CASE("A blend state weights its two nearest clips by its parameter and keeps them in phase") {
    auto definition = states();
    Machine::Blend blend;
    blend.parameter = "speed";
    blend.clips = {{"walk", 1}, {"run", 3}};
    definition.states[0] = clip_state("move", "");
    definition.states[0].blend = blend;
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    CHECK(animator->state() == "move");

    // Halfway, the state takes the mean of 1 s and 0.5 s, 0.75 s, a pass, and both clips sample the same fraction:
    // walk's X is that fraction, and run's is 4 times its clip time, half the fraction.
    animator->set_float("speed", 2);
    (void)animator->update(.3);
    CHECK(animator->time() == Near{.4, 1e-12});
    CHECK(actor.x() == Near{.5 * .4 + .5 * 4 * .2, pose_tolerance});
    const auto events = animator->update(.15);
    CHECK(animator->time() == Near{.6, 1e-12});
    // Both clips cross their events at half a pass, walk's step and run's stride.
    REQUIRE(events.size() == 2);
    CHECK(events[0].clip == "walk");
    CHECK(events[1].clip == "run");
    CHECK(events[0].offset == Near{.075, 1e-12});
    CHECK(events[1].offset == Near{.075, 1e-12});
    CHECK(events[0].weight == .5F);
    CHECK(events[1].weight == .5F);

    // A quarter of the way, and at or beyond the ends, where one clip plays and reports alone.
    animator->set_float("speed", 1.5F);
    (void)animator->update(0);
    CHECK(actor.x() == Near{.75 * .6 + .25 * 4 * .3, pose_tolerance});
    animator->set_float("speed", 1);
    CHECK(animator->update(.3).empty());
    CHECK(animator->time() == Near{.9, 1e-12});
    CHECK(actor.x() == Near{.9, pose_tolerance});
    animator->set_float("speed", 0);
    const auto alone = animator->update(.8);
    REQUIRE(alone.size() == 1);
    CHECK(alone[0].clip == "walk");
    CHECK(alone[0].weight == 1);
    animator->set_float("speed", 5);
    CHECK(animator->update(.125).empty());
    CHECK(animator->time() == Near{1.95, 1e-12});
    CHECK(actor.x() == Near{4 * .475, pose_tolerance});
}

TEST_CASE("A state's speed and speed parameter scale its rate") {
    auto definition = states("walk");
    definition.parameters.push_back(parameter("rate", ParameterType::real, 1));
    definition.states[0].speed = 2;
    definition.states[0].speed_parameter = "rate";
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    (void)animator->update(.25);
    CHECK(animator->time() == .5);
    animator->set_float("rate", .25F);
    (void)animator->update(.25);
    CHECK(animator->time() == .625);
    animator->set_float("rate", 0);
    (void)animator->update(.25);
    CHECK(animator->time() == .625);
    animator->set_float("rate", -1);
    CHECK_THROWS_WITH_AS((void)animator->update(.25), "Animation state speed must be at least 0: walk",
                         std::invalid_argument);
    CHECK(animator->time() == .625);
}

TEST_CASE("A state of a clip with zero duration holds its pose and reaches its exit times at its speed") {
    auto definition = states("walk");
    definition.states.push_back(clip_state("aim", "aim_low"));
    definition.states.push_back(clip_state("guard", "guard"));
    definition.states.back().speed = 2;
    definition.transitions = {transition("walk", "aim", {condition("go", ConditionMode::is_true)}),
                              transition("aim", "guard", {})};
    definition.transitions[1].exit_time = .75;
    Actor actor(std::move(definition));
    auto &animator = actor.animator;

    // At speed 1 the pose advances one normalized unit per second, and reports its event once, at its entry.
    animator->set_trigger("go");
    const auto entered = animator->update(.25);
    CHECK(animator->state() == "aim");
    CHECK(animator->time() == .25);
    CHECK(actor.x() == Near{-3, pose_tolerance});
    REQUIRE(entered.size() == 1);
    CHECK(entered[0].event.name == "aimed");
    CHECK(entered[0].offset == 0);
    CHECK(animator->update(.5).empty()); // Crosses the exit time, 0.75.
    CHECK(animator->state() == "aim");
    CHECK(animator->time() == .75);
    CHECK(actor.x() == Near{-3, pose_tolerance});

    // A looping pose at speed 2 takes half a second a pass and crosses its event at 0 once per pass.
    const auto guarded = animator->update(.125);
    CHECK(animator->state() == "guard");
    CHECK(animator->time() == .25);
    CHECK(actor.x() == Near{6, pose_tolerance});
    REQUIRE(guarded.size() == 1);
    CHECK(guarded[0].offset == 0);
    const auto passes = animator->update(1);
    CHECK(animator->time() == 2.25);
    CHECK(actor.x() == Near{6, pose_tolerance});
    REQUIRE(passes.size() == 2);
    CHECK(passes[0].event.name == "guard");
    CHECK(passes[0].offset == .375);
    CHECK(passes[1].offset == .875);
}

TEST_CASE("A blend of clips with zero duration blends their poses and advances at its speed") {
    auto definition = states();
    Machine::Blend blend;
    blend.parameter = "speed";
    blend.clips = {{"aim_low", -1}, {"aim_high", 1}};
    definition.states[0] = clip_state("aim", "");
    definition.states[0].blend = blend;
    definition.transitions = {transition("aim", "walk", {})};
    definition.transitions[0].exit_time = 1;
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    CHECK(actor.x() == Near{0, pose_tolerance}); // Halfway between -3 and 3.
    animator->set_float("speed", .5F);
    const auto events = animator->update(.5);
    CHECK(animator->time() == .5);
    CHECK(actor.x() == Near{.25 * -3 + .75 * 3, pose_tolerance});
    REQUIRE(events.size() == 1);
    CHECK(events[0].clip == "aim_low");
    CHECK(events[0].weight == .25F);
    (void)animator->update(.5); // Reaches its end, and the exit time.
    CHECK(animator->state() == "aim");
    CHECK(animator->time() == 1);
    (void)animator->update(.25);
    CHECK(animator->state() == "walk");
}

TEST_CASE("play enters a state at a time and ends a crossfade") {
    auto definition = states("walk");
    definition.transitions = {transition("walk", "idle", {condition("go", ConditionMode::is_true)}, 1)};
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    animator->set_trigger("go");
    (void)animator->update(.25);
    REQUIRE(animator->crossfade());
    animator->play("run", .5);
    CHECK_FALSE(animator->crossfade());
    CHECK(animator->state() == "run");
    CHECK(animator->time() == .5);
    CHECK(actor.x() == Near{1, pose_tolerance});
    const auto events = animator->update(.25);
    REQUIRE(events.size() == 1); // The stride at the entry point.
    CHECK(events[0].offset == 0);
}

TEST_CASE("The codec persists the machine key, parameters and current state") {
    auto definition = states("walk");
    definition.transitions = {transition("walk", "run", {condition("go", ConditionMode::is_true)}, 1)};
    Actor actor(std::move(definition));
    auto &animator = actor.animator;
    const std::map<std::string, std::shared_ptr<const Machine>, std::less<>> machines{{"locomotion", actor.machine}};
    ComponentCodecs codecs;
    add_state_machine_animator_codec(
        codecs,
        [&](const std::shared_ptr<const Machine> &machine) { return machine == actor.machine ? "locomotion" : ""; },
        [&](std::string_view name) -> std::shared_ptr<const Machine> {
            const auto found = machines.find(name);
            return found == machines.end() ? nullptr : found->second;
        });
    animator->set_float("speed", 2.5F);
    animator->set_integer("stance", -3);
    animator->set_bool("grounded", false);
    animator->set_trigger("jump");
    animator->set_trigger("go");
    (void)animator->update(.25);
    REQUIRE(animator->crossfade());

    const auto captured = codecs.capture(actor.object, {});
    REQUIRE(captured.size() == 1);
    CHECK(captured[0].type == "anima.state-machine-animator.v1");
    CHECK(captured[0].state == R"({"machine":"locomotion","parameters":{"go":false,"grounded":false,"jump":true,)"
                               R"("ready":false,"speed":2.5,"stance":-3,"stop":false},"state":"run","time":0.5})");

    // A restored animator plays the state it was entering, alone, and does not report events at its time again.
    auto copy = actor.scene.create("copy", actor.object.renderer().mesh());
    codecs.restore(copy, captured, {});
    auto restored = copy.get_component<StateMachineAnimator>();
    REQUIRE(restored);
    CHECK(restored->machine() == actor.machine);
    CHECK(restored->state() == "run");
    CHECK(restored->time() == .5);
    CHECK_FALSE(restored->crossfade());
    CHECK(restored->get_float("speed") == 2.5F);
    CHECK(restored->get_integer("stance") == -3);
    CHECK_FALSE(restored->get_bool("grounded"));
    CHECK(restored->get_bool("jump"));
    CHECK_FALSE(restored->get_bool("go"));
    CHECK(restored->pose().world.at(0)[12] == Near{1, pose_tolerance});
    CHECK(actor.scene.instance(copy.id()).palette.at(0)[12] == Near{1, pose_tolerance});
    CHECK(restored->update(.125).empty()); // The stride at 0.5 was reported before the capture.

    // A scene document keeps it too.
    const auto document =
        serialize_scene(actor.scene, [](const std::shared_ptr<const Mesh> &) { return std::string("actor"); }, codecs);
    const auto mesh = actor.object.renderer().mesh();
    const auto loaded = load_scene(document, [&](std::string_view) { return mesh; }, codecs);
    const auto reloaded = loaded->components<StateMachineAnimator>();
    REQUIRE(reloaded.size() == 2);
    for (const auto &component : reloaded)
        CHECK(component->state() == "run");
}

TEST_CASE("The codec rejects invalid registrations, keys and payloads") {
    Actor actor(states());
    const auto machine = actor.machine;
    const auto name = [&](const std::shared_ptr<const Machine> &) { return std::string("locomotion"); };
    const auto resolve = [&](std::string_view key) { return key == "locomotion" ? machine : nullptr; };
    ComponentCodecs codecs;
    CHECK_THROWS_WITH_AS(add_state_machine_animator_codec(codecs, {}, resolve),
                         "State machine animator codec requires naming and resolution callbacks",
                         std::invalid_argument);
    add_state_machine_animator_codec(codecs, name, resolve);
    CHECK_THROWS_WITH_AS(add_state_machine_animator_codec(codecs, name, resolve), "Duplicate component codec",
                         std::invalid_argument);

    ComponentCodecs unnamed;
    add_state_machine_animator_codec(
        unnamed, [](const std::shared_ptr<const Machine> &) { return std::string(); }, resolve);
    CHECK_THROWS_WITH_AS((void)unnamed.capture(actor.object, {}), "Invalid animation state machine key",
                         std::invalid_argument);

    const std::string valid = R"({"machine":"locomotion","parameters":{"go":false,"grounded":true,"jump":false,)"
                              R"("ready":false,"speed":0,"stance":0,"stop":false},"state":"idle","time":0})";
    const auto restore = [&](std::string payload) {
        auto target = actor.scene.create("target", actor.object.renderer().mesh());
        const ComponentData data{"anima.state-machine-animator.v1", std::move(payload), true};
        codecs.restore(target, std::span(&data, 1), {});
    };
    REQUIRE_NOTHROW(restore(valid));
    const std::vector<std::pair<std::string, std::string>> rejected{
        {changed(valid, R"("time":0)", R"("time":0,"time":0)"), "Duplicate JSON document field"},
        {changed(valid, R"(,"time":0)", ""), "Missing JSON field: time"},
        {changed(valid, R"("time":0)", R"("time":0,"pose":[])"), "Unknown JSON field: pose"},
        {changed(valid, R"("locomotion")", R"("")"), "Invalid animation state machine key"},
        {changed(valid, R"("locomotion")", R"("elsewhere")"), "Animation state machine key could not be resolved"},
        {changed(valid, R"("idle")", R"("flying")"), "Unknown animation state: flying"},
        {changed(valid, R"("time":0)", R"("time":-0.5)"), "Invalid animation state time"},
        {changed(valid, R"("time":0)", R"("time":"0")"), "JSON value must be a number"},
        {changed(changed(valid, R"({"go")", R"([{"go")"), R"(false},"state")", R"(false}],"state")"),
         "State machine animator parameters must be an object"},
        {changed(valid, R"("stop":false})", R"("stop":false,"extra":1})"), "Unknown animation parameter: extra"},
        {changed(valid, R"("speed":0,)", ""), "Missing animation parameter: speed"},
        {changed(valid, R"("speed":0)", R"("speed":"fast")"), "Invalid animation parameter value: speed"},
        {changed(valid, R"("speed":0)", R"("speed":1e39)"), "Invalid animation parameter value: speed"},
        {changed(valid, R"("stance":0)", R"("stance":1.5)"), "Invalid animation parameter value: stance"},
        {changed(valid, R"("stance":0)", R"("stance":2147483648)"), "Invalid animation parameter value: stance"},
        {changed(valid, R"("grounded":true)", R"("grounded":1)"), "Invalid animation parameter value: grounded"},
        {changed(valid, R"("jump":false)", R"("jump":null)"), "Invalid animation parameter value: jump"},
    };
    for (const auto &[payload, message] : rejected) {
        CAPTURE(payload);
        CHECK_THROWS_WITH_AS(restore(payload), message.c_str(), std::invalid_argument);
    }

    // The machine's source must match the object it is restored onto.
    auto other = std::make_shared<Asset>(*actor.asset);
    other->nodes[0].name = "other";
    auto mismatched = actor.scene.create("mismatched", Mesh::compile(*other));
    const ComponentData data{"anima.state-machine-animator.v1", valid, true};
    CHECK_THROWS_WITH_AS(codecs.restore(mismatched, std::span(&data, 1), {}),
                         "StateMachineAnimator source does not match the object's mesh hierarchy and bind",
                         std::invalid_argument);

    // A capture that could not be restored is rejected.
    auto oversized = states();
    oversized.parameters.push_back(parameter(std::string(16 * 1024 * 1024, 'p'), ParameterType::boolean));
    const auto large = std::make_shared<const Machine>(actor.asset, clip_policies(), std::move(oversized));
    auto heavy = actor.scene.create("heavy", actor.object.renderer().mesh());
    (void)heavy.add_component<StateMachineAnimator>(large);
    CHECK_THROWS_WITH_AS((void)codecs.capture(heavy, {}), "State machine animator payload exceeds 16 MiB",
                         std::invalid_argument);
}

namespace {
// Every document field, and the definition it decodes to.
constexpr std::string_view full_document = R"({
  "version": 2,
  "kind": "anima.animation-state-machine",
  "parameters": [
    {"name": "speed", "type": "float", "initial": 0.5},
    {"name": "stance", "type": "int", "initial": -2},
    {"name": "grounded", "type": "bool", "initial": true},
    {"name": "jump", "type": "trigger", "initial": false}
  ],
  "states": [
    {"name": "move", "clip": null, "blend": {"parameter": "speed", "clips": [
      {"clip": "walk", "threshold": 1}, {"clip": "run", "threshold": 3}]}, "speed": 1.5, "speed_parameter": "speed"},
    {"name": "air", "clip": "jump", "blend": null, "speed": 1, "speed_parameter": null}
  ],
  "transitions": [
    {"from": null, "to": "air", "conditions": [{"parameter": "jump", "mode": "is_true", "threshold": 0}],
     "exit_time": null, "duration": 0.25, "offset": 0, "interruption": "destination_then_source", "to_self": true},
    {"from": "air", "to": "move", "exit_time": 1, "duration": 0, "offset": 0.5, "interruption": "source",
     "conditions": [{"parameter": "grounded", "mode": "is_true", "threshold": 0}, {"parameter": "stance",
      "mode": "not_equal", "threshold": 3}], "to_self": false},
    {"from": "move", "to": "move", "conditions": [{"parameter": "speed", "mode": "less", "threshold": -1}],
     "exit_time": null, "duration": 0, "offset": 0, "interruption": "none", "to_self": false}
  ]
})";
} // namespace

TEST_CASE("A document decodes every field of a definition") {
    const auto asset = clip_asset();
    const auto machine = Machine::deserialize(asset, clip_policies(), full_document);
    CHECK(machine.source() == asset);
    const auto &definition = machine.definition();
    REQUIRE(definition.parameters.size() == 4);
    CHECK(definition.parameters[0].name == "speed");
    CHECK(definition.parameters[0].type == ParameterType::real);
    CHECK(definition.parameters[0].initial == .5);
    CHECK(definition.parameters[1].type == ParameterType::integer);
    CHECK(definition.parameters[1].initial == -2);
    CHECK(definition.parameters[2].type == ParameterType::boolean);
    CHECK(definition.parameters[2].initial == 1);
    CHECK(definition.parameters[3].type == ParameterType::trigger);
    CHECK(definition.parameters[3].initial == 0);
    REQUIRE(definition.states.size() == 2);
    const auto &move = definition.states[0];
    CHECK(move.clip.empty());
    REQUIRE(move.blend);
    CHECK(move.blend->parameter == "speed");
    REQUIRE(move.blend->clips.size() == 2);
    CHECK(move.blend->clips[1].clip == "run");
    CHECK(move.blend->clips[1].threshold == 3);
    CHECK(move.speed == 1.5);
    CHECK(move.speed_parameter == "speed");
    CHECK(definition.states[1].clip == "jump");
    CHECK(definition.states[1].speed == 1);
    REQUIRE(definition.transitions.size() == 3);
    const auto &any = definition.transitions[0];
    CHECK_FALSE(any.from);
    CHECK(any.to == "air");
    CHECK(any.duration == .25);
    CHECK(any.interruption == Machine::Interruption::destination_then_source);
    CHECK(any.to_self);
    const auto &land = definition.transitions[1];
    CHECK(land.from == "air");
    CHECK(land.exit_time == 1);
    CHECK(land.offset == .5);
    CHECK(land.duration == 0);
    CHECK(land.interruption == Machine::Interruption::source);
    REQUIRE(land.conditions.size() == 2);
    CHECK(land.conditions[1].mode == ConditionMode::not_equal);
    CHECK(land.conditions[1].threshold == 3);
    CHECK(land.conditions[0].threshold == 0);
    const auto &again = definition.transitions[2];
    CHECK(again.from == again.to);
    CHECK_FALSE(again.exit_time);
    CHECK(again.interruption == Machine::Interruption::none);
    CHECK_FALSE(again.to_self);
}

TEST_CASE("Invalid documents are rejected with their reason") {
    const auto asset = clip_asset();
    const std::string valid(full_document);
    const auto decode = [&](const std::string &document) {
        return Machine::deserialize(asset, clip_policies(), document);
    };
    REQUIRE_NOTHROW((void)decode(valid));
    std::vector<std::pair<std::string, std::string>> rejected{
        {changed(valid, R"("version": 2)", R"("version": 2, "version": 2)"), "Duplicate JSON document field"},
        {changed(valid, R"("version": 2)", R"("version": 1)"), "Unsupported animation state machine document version"},
        {changed(valid, R"("version": 2)", R"("version": 2.0)"),
         "Unsupported animation state machine document version"},
        {changed(valid, R"("version": 2,)", ""), "Missing JSON field: version"},
        {changed(valid, R"("anima.animation-state-machine")", R"("anima.scene")"),
         "Invalid animation state machine document kind"},
        // Another version is reported before a field that it lacks or adds.
        {changed(changed(valid, R"("version": 2)", R"("version": 1)"), R"("states")", R"("modes")"),
         "Unsupported animation state machine document version"},
        {changed(valid, R"("version": 2)", R"("version": 2, "layers": [])"), "Unknown JSON field: layers"},
        {changed(valid, R"("type": "float")", R"("type": "double")"), "Unknown animation parameter type: double"},
        // A trigger's initial value is a flag that starts unset.
        {changed(valid, R"("initial": false)", R"("initial": 0)"),
         "[json.exception.type_error.302] type must be boolean, but is number"},
        {changed(valid, R"("initial": false)", R"("initial": true)"),
         "Invalid initial value of animation parameter: jump"},
        {changed(valid, R"("initial": true)", R"("initial": 1)"),
         "[json.exception.type_error.302] type must be boolean, but is number"},
        {changed(valid, R"("initial": 0.5)", R"("initial": true)"), "JSON value must be a number"},
        {changed(valid, R"("clip": "jump", "blend": null)", R"("clip": "jump", "blend": {})"),
         "Animation state needs exactly one of a clip and a blend: air"},
        {changed(valid, R"("clip": "jump", "blend": null)", R"("clip": null, "blend": null)"),
         "Animation state needs exactly one of a clip and a blend: air"},
        // null is the one spelling of no speed parameter.
        {changed(valid, R"("speed_parameter": null)", R"("speed_parameter": "")"), "Unknown animation parameter: "},
        {changed(valid, R"("threshold": 3})", R"("threshold": "3"})"), "JSON value must be a number"},
        {changed(valid, R"("threshold": 3})", R"("threshold": 1e39})"), "JSON number outside the float range"},
        {changed(valid, R"("threshold": 3})", R"("threshold": 3, "weight": 1})"), "Unknown JSON field: weight"},
        {changed(valid, R"("from": null, )", ""), "Missing JSON field: from"},
        {changed(valid, R"("from": null)", R"("from": 0)"),
         "[json.exception.type_error.302] type must be string, but is number"},
        {changed(valid, R"("mode": "not_equal")", R"("mode": "differs")"), "Unknown animation condition mode: differs"},
        {changed(valid, R"("destination_then_source")", R"("always")"),
         "Unknown animation transition interruption: always"},
        {changed(valid, R"("to_self": true)", R"("to_self": 1)"),
         "[json.exception.type_error.302] type must be boolean, but is number"},
        {changed(valid, R"("exit_time": 1)", R"("exit_time": "1")"), "JSON value must be a number"},
        // The constructor's rules apply to decoded definitions.
        {changed(valid, R"("to": "air")", R"("to": "sky")"), "Unknown animation state: sky"},
        {changed(valid, R"("offset": 0.5)", R"("offset": 1)"), "Invalid animation transition offset"},
        {std::string(4 * 1024 * 1024 + 1, ' '), "JSON document exceeds byte limit"},
        {"{",
         "[json.exception.parse_error.101] parse error at line 1, column 2: syntax error while parsing object key - "
         "unexpected end of input; expected string literal"},
    };
    // Every field is required, so each that version 1 left optional is missing when renamed.
    for (const std::string field : {"initial", "clip", "blend", "speed", "speed_parameter", "conditions", "exit_time",
                                    "duration", "offset", "interruption", "to_self"})
        rejected.emplace_back(changed(valid, '"' + field + "\":", '"' + field + "_renamed\":"),
                              "Missing JSON field: " + field);
    rejected.emplace_back(changed(valid, R"("mode": "is_true", "threshold": 0)", R"("mode": "is_true")"),
                          "Missing JSON field: threshold");
    // Lists that must be arrays, and nesting, in the smallest document.
    const std::string idle = R"({"name": "idle", "clip": "idle", "blend": null, "speed": 1, "speed_parameter": null})";
    const std::string smallest =
        R"({"version": 2, "kind": "anima.animation-state-machine", "parameters": [], "states": [)" + idle +
        R"(], "transitions": []})";
    REQUIRE_NOTHROW((void)decode(smallest));
    const std::vector<std::pair<std::string, std::string>> structure{
        {changed(smallest, R"("parameters": [])", R"("parameters": {})"),
         "Animation state machine field must be an array: parameters"},
        {changed(smallest, R"("states": [)" + idle + "]", R"("states": "idle")"),
         "Animation state machine field must be an array: states"},
        {changed(smallest, R"("transitions": [])", R"("transitions": null)"),
         "Animation state machine field must be an array: transitions"},
        {changed(smallest, R"("clip": "idle", "blend": null)",
                 R"("clip": null, "blend": {"parameter": "speed", "clips": {}})"),
         "Animation state machine field must be an array: clips"},
        {changed(smallest, R"("transitions": [])",
                 R"("transitions": [{"from": null, "to": "idle", "conditions": {}, "exit_time": null, "duration": 0,)"
                 R"( "offset": 0, "interruption": "none", "to_self": false}])"),
         "Animation state machine field must be an array: conditions"},
        {changed(smallest, R"("transitions": [])", R"("transitions": [[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]])"),
         "JSON document exceeds nesting limit"},
    };
    rejected.insert(rejected.end(), structure.begin(), structure.end());
    for (const auto &[document, message] : rejected) {
        CAPTURE(document);
        CHECK_THROWS_WITH_AS((void)decode(document), message.c_str(), std::invalid_argument);
    }
}

TEST_CASE("Invalid definitions are rejected with their reason") {
    const auto asset = clip_asset();
    const auto policies = clip_policies();
    const auto valid = [] {
        auto definition = states();
        Machine::Blend blend;
        blend.parameter = "speed";
        blend.clips = {{"walk", 1}, {"run", 3}};
        definition.states.push_back(clip_state("move", ""));
        definition.states.back().blend = blend;
        definition.transitions = {transition("idle", "walk", {condition("speed", ConditionMode::greater, 1)}, .5)};
        return definition;
    }();
    REQUIRE_NOTHROW(Machine(asset, policies, valid));
    CHECK_THROWS_WITH_AS(Machine(nullptr, policies, valid), "Animation state machine requires a source asset",
                         std::invalid_argument);
    auto repeated = policies;
    repeated.push_back(policies[0]);
    CHECK_THROWS_WITH_AS(Machine(asset, repeated, valid), "Duplicate animation clip metadata: idle",
                         std::invalid_argument);

    using Change = void (*)(Machine::Definition &);
    const std::vector<std::pair<Change, const char *>> rejected{
        {[](Machine::Definition &d) { d.parameters[0].name.clear(); }, "Empty animation parameter name"},
        {[](Machine::Definition &d) { d.parameters[1].name = "speed"; }, "Duplicate animation parameter: speed"},
        {[](Machine::Definition &d) { d.parameters[0].type = static_cast<ParameterType>(7); },
         "Invalid animation parameter type: speed"},
        {[](Machine::Definition &d) { d.parameters[0].initial = std::numeric_limits<double>::infinity(); },
         "Invalid initial value of animation parameter: speed"},
        {[](Machine::Definition &d) { d.parameters[0].initial = 1e39; },
         "Invalid initial value of animation parameter: speed"},
        {[](Machine::Definition &d) { d.parameters[1].initial = .5; },
         "Invalid initial value of animation parameter: stance"},
        {[](Machine::Definition &d) { d.parameters[1].initial = 2147483648.; },
         "Invalid initial value of animation parameter: stance"},
        {[](Machine::Definition &d) { d.parameters[2].initial = 2; },
         "Invalid initial value of animation parameter: grounded"},
        {[](Machine::Definition &d) { d.parameters[4].initial = 1; },
         "Invalid initial value of animation parameter: jump"},
        {[](Machine::Definition &d) { d.states.clear(); }, "Animation state machine needs a state"},
        {[](Machine::Definition &d) { d.states[1].name.clear(); }, "Empty animation state name"},
        {[](Machine::Definition &d) { d.states[1].name = "idle"; }, "Duplicate animation state: idle"},
        {[](Machine::Definition &d) { d.states[1].clip.clear(); },
         "Animation state needs exactly one of a clip and a blend: walk"},
        {[](Machine::Definition &d) { d.states.back().clip = "walk"; },
         "Animation state needs exactly one of a clip and a blend: move"},
        {[](Machine::Definition &d) { d.states[1].speed = -1; }, "Invalid animation state speed: walk"},
        {[](Machine::Definition &d) { d.states[1].speed = std::numeric_limits<double>::quiet_NaN(); },
         "Invalid animation state speed: walk"},
        {[](Machine::Definition &d) { d.states[1].speed_parameter = "pace"; }, "Unknown animation parameter: pace"},
        {[](Machine::Definition &d) { d.states[1].speed_parameter = "stance"; },
         "Animation parameter must be a float: stance"},
        {[](Machine::Definition &d) { d.states.back().blend->parameter = "grounded"; },
         "Animation parameter must be a float: grounded"},
        {[](Machine::Definition &d) { d.states.back().blend->clips.pop_back(); },
         "Animation blend needs two or more clips: move"},
        {[](Machine::Definition &d) { d.states.back().blend->clips[1].threshold = 1; },
         "Animation blend thresholds must be finite and increasing: move"},
        {[](Machine::Definition &d) {
             d.states.back().blend->clips[0].threshold = -std::numeric_limits<float>::infinity();
         },
         "Animation blend thresholds must be finite and increasing: move"},
        {[](Machine::Definition &d) { d.states.back().blend->clips[1].clip = "jump"; },
         "Animation blend clips must all loop or all hold: move"},
        {[](Machine::Definition &d) { d.states.back().blend->clips[1].clip = "guard"; },
         "Animation blend mixes clips of zero and positive duration: move"},
        {[](Machine::Definition &d) { d.states[1].clip = "swim"; }, "Animation clip has no metadata: swim"},
        {[](Machine::Definition &d) { d.transitions[0].from = "sky"; }, "Unknown animation state: sky"},
        {[](Machine::Definition &d) { d.transitions[0].to = "sea"; }, "Unknown animation state: sea"},
        {[](Machine::Definition &d) { d.transitions[0].conditions.clear(); },
         "Animation transition needs an exit time or a condition"},
        {[](Machine::Definition &d) {
             d.transitions[0].from.reset();
             d.transitions[0].exit_time = .5;
         },
         "A transition from any state has no exit time"},
        {[](Machine::Definition &d) { d.transitions[0].to_self = true; },
         "Only a transition from any state can set to_self"},
        {[](Machine::Definition &d) { d.transitions[0].exit_time = -.5; }, "Invalid animation transition exit time"},
        {[](Machine::Definition &d) { d.transitions[0].exit_time = std::numeric_limits<double>::infinity(); },
         "Invalid animation transition exit time"},
        {[](Machine::Definition &d) { d.transitions[0].duration = -1; }, "Invalid animation transition duration"},
        {[](Machine::Definition &d) { d.transitions[0].offset = -.25; }, "Invalid animation transition offset"},
        {[](Machine::Definition &d) { d.transitions[0].interruption = static_cast<Machine::Interruption>(5); },
         "Invalid animation transition interruption"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0].parameter = "pace"; },
         "Unknown animation parameter: pace"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0].mode = ConditionMode::equals; },
         "Animation condition mode does not apply to parameter: speed"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0] = condition("grounded", ConditionMode::greater); },
         "Animation condition mode does not apply to parameter: grounded"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0] = condition("jump", ConditionMode::is_false); },
         "Animation condition mode does not apply to parameter: jump"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0] = condition("stance", ConditionMode::is_true); },
         "Animation condition mode does not apply to parameter: stance"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0].mode = static_cast<ConditionMode>(9); },
         "Animation condition mode does not apply to parameter: speed"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0].threshold = 1e39; },
         "Invalid animation condition threshold: speed"},
        {[](Machine::Definition &d) { d.transitions[0].conditions[0] = condition("stance", ConditionMode::less, 1.5); },
         "Invalid animation condition threshold: stance"},
        {[](Machine::Definition &d) {
             d.transitions[0].conditions[0] = condition("grounded", ConditionMode::is_true, 1);
         },
         "Invalid animation condition threshold: grounded"},
    };
    for (std::size_t i = 0; i < rejected.size(); ++i) {
        CAPTURE(i);
        auto definition = valid;
        rejected[i].first(definition);
        CHECK_THROWS_WITH_AS(Machine(asset, policies, std::move(definition)), rejected[i].second,
                             std::invalid_argument);
    }

    // A clip named by no source clip or by several, or whose events leave it.
    auto ambiguous = std::make_shared<Asset>(*asset);
    ambiguous->animations.push_back(ambiguous->animations[1]);
    CHECK_THROWS_WITH_AS(Machine(ambiguous, policies, valid), "Animation clip must name exactly one source clip: walk",
                         std::invalid_argument);
    auto missing = std::make_shared<Asset>(*asset);
    missing->animations.erase(missing->animations.begin() + 1);
    CHECK_THROWS_WITH_AS(Machine(missing, policies, valid), "Animation clip must name exactly one source clip: walk",
                         std::invalid_argument);
    auto late = policies;
    late[1].events.push_back({1.5, "late"});
    CHECK_THROWS_WITH_AS(Machine(asset, late, valid), "Animation clip event outside its duration: walk",
                         std::invalid_argument);
    for (const auto duration : {-1., std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(duration);
        auto invalid = std::make_shared<Asset>(*asset);
        invalid->animations[0].duration = duration;
        CHECK_THROWS_WITH_AS(Machine(invalid, policies, valid),
                             "Animation clip duration must be finite and nonnegative: idle", std::invalid_argument);
    }
}

TEST_CASE("The animator rejects invalid bindings, parameters, states and steps") {
    Actor actor(states("run"));
    auto &animator = actor.animator;
    auto spare = actor.scene.create("spare", actor.object.renderer().mesh());
    CHECK_THROWS_WITH_AS(StateMachineAnimator(spare, nullptr), "StateMachineAnimator requires a state machine",
                         std::invalid_argument);
    auto other = std::make_shared<Asset>(*actor.asset);
    other->nodes[0].rest.translation = {0, 1, 0};
    auto moved = actor.scene.create("moved", Mesh::compile(*other));
    CHECK_THROWS_WITH_AS(StateMachineAnimator(moved, actor.machine),
                         "StateMachineAnimator source does not match the object's mesh hierarchy and bind",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(StateMachineAnimator(actor.scene.create("bare"), actor.machine),
                         "GameObject has no MeshRenderer", std::logic_error);

    CHECK_THROWS_WITH_AS(animator->set_float("pace", 1), "Unknown animation parameter: pace", std::out_of_range);
    CHECK_THROWS_WITH_AS((void)animator->get_bool("pace"), "Unknown animation parameter: pace", std::out_of_range);
    CHECK_THROWS_WITH_AS(animator->set_float("stance", 1), "Animation parameter has another type: stance",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->set_integer("speed", 1), "Animation parameter has another type: speed",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->set_bool("jump", true), "Animation parameter has another type: jump",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->set_trigger("grounded"), "Animation parameter has another type: grounded",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->reset_trigger("grounded"), "Animation parameter has another type: grounded",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)animator->get_float("stance"), "Animation parameter has another type: stance",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)animator->get_integer("speed"), "Animation parameter has another type: speed",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)animator->get_bool("speed"), "Animation parameter has another type: speed",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->set_float("speed", std::numeric_limits<float>::infinity()),
                         "Animation float parameter must be finite: speed", std::invalid_argument);
    CHECK(animator->get_float("speed") == 0);

    CHECK_THROWS_WITH_AS(animator->play("swim"), "Unknown animation state: swim", std::out_of_range);
    CHECK_THROWS_WITH_AS(animator->play("walk", -1), "Invalid animation state time", std::invalid_argument);
    CHECK_THROWS_WITH_AS(animator->play("walk", std::numeric_limits<double>::quiet_NaN()),
                         "Invalid animation state time", std::invalid_argument);
    CHECK(animator->state() == "run");

    CHECK_THROWS_WITH_AS((void)animator->update(-1), "Invalid StateMachineAnimator time step", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)animator->update(std::numeric_limits<double>::infinity()),
                         "Invalid StateMachineAnimator time step", std::invalid_argument);
    // Run takes 0.5 s a pass, so 5,000 s is 10,000 passes and a little more is too long.
    REQUIRE_NOTHROW((void)animator->update(5000));
    CHECK_THROWS_WITH_AS((void)animator->update(5000.25),
                         "Animation state step exceeds 10000 loops; split large offline advances",
                         std::invalid_argument);
    CHECK(animator->time() == 10000);

    auto fast = states("jump");
    fast.states[0].speed = std::numeric_limits<double>::max();
    Actor overflowing(std::move(fast));
    CHECK_THROWS_WITH_AS((void)overflowing.animator->update(2), "Animation state time overflow", std::runtime_error);
    CHECK(overflowing.animator->time() == 0);

    actor.object.renderer().set_mesh(Mesh::compile(*actor.asset));
    CHECK_THROWS_WITH_AS((void)animator->update(0),
                         "StateMachineAnimator mesh was replaced; bind a new StateMachineAnimator explicitly",
                         std::logic_error);
    CHECK_THROWS_WITH_AS(animator->play("walk"),
                         "StateMachineAnimator mesh was replaced; bind a new StateMachineAnimator explicitly",
                         std::logic_error);
}
