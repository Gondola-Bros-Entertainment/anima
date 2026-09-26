#include <anima/assets/action.hpp>
#include <anima/assets/interaction_bindings.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace anima;
namespace {
constexpr float tolerance = 1e-5F;
float error(const Mat4 &a, const Mat4 &b) {
    float result = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}
// A rider attached to a vehicle, and a passenger attached to the rider.
struct Ride {
    std::shared_ptr<Asset> rider = std::make_shared<Asset>(), vehicle = std::make_shared<Asset>(),
                           passenger = std::make_shared<Asset>();
    std::vector<InteractionRole> roles{{"rider", rider}, {"vehicle", vehicle}, {"passenger", passenger}};
    std::vector<InteractionAttachment> attachments{{"passenger", "rider", {0}, {6}}, {"rider", "vehicle", {5}, {2}}};
    Ride() {
        rider->nodes.resize(7);
        vehicle->nodes.resize(3);
        passenger->nodes.resize(2);
    }
    // Every role's rest pose, in role order, at the origin.
    [[nodiscard]] std::vector<InteractionFrame> frames() const {
        return {{sample_pose(*rider)}, {sample_pose(*vehicle)}, {sample_pose(*passenger)}};
    }
};
} // namespace

TEST_CASE("Attachments follow moving sockets through entry, ride and exit, without depending on earlier frames") {
    const Ride ride;
    const InteractionBindings bindings(ride.roles, ride.attachments);
    const ActionTimeline timeline({{"enter", .5, false, {}}, {"ride", .8, true, {}}, {"exit", .5, false, {}}});
    const auto evaluate = [&](double time) {
        CAPTURE(time);
        const auto clock = timeline.sample(time, 2.1);
        const auto weight = static_cast<float>(clock.phase == 0   ? clock.progress
                                               : clock.phase == 1 ? 1
                                                                  : 1 - clock.progress);
        auto frames = ride.frames();
        frames[0].pose.world[5] = matrix(Transform{{0, 1, 0}});
        frames[0].pose.world[6] = matrix(Transform{{0, 1.3F, -.2F}});
        frames[1].pose.world[2] =
            matrix(Transform{{0, 1.4F + .1F * std::sin(static_cast<float>(time)), 0}, {0, 0, 0, 1}, {1.3F, .8F, 1.1F}});
        frames[1].world = matrix(
            Transform{{static_cast<float>(time), 0, -2},
                      {0, std::sin(static_cast<float>(time) * .1F), 0, std::cos(static_cast<float>(time) * .1F)}});
        frames[0].world = matrix(Transform{{-1, 0, 0}});
        frames[2].world = matrix(Transform{{1, 0, 0}});
        const std::array<InteractionPlacement, 2> placements{{{weight}, {weight}}};
        const auto result = bindings.sample(frames, placements);
        // Attachments move placements only: no participant's anatomy, and not the driving vehicle.
        for (std::size_t i = 0; i < result.size(); ++i) {
            CAPTURE(i);
            CHECK(result[i].pose.world == frames[i].pose.world);
        }
        CHECK(result[1].world == frames[1].world);
        if (weight == 0) {
            // Without contact weight, the attached roles keep their free placement.
            CHECK(result[0].world == frames[0].world);
            CHECK(result[2].world == frames[2].world);
        }
        if (weight == 1)
            for (const auto &attachment : ride.attachments) {
                CAPTURE(attachment.child);
                const auto &child = result[bindings.role(attachment.child)];
                const auto &parent = result[bindings.role(attachment.parent)];
                // The child's socket stays on the moving parent socket, and the scaled seat does not scale the rider.
                CHECK(error(child.world * interaction_socket(child.pose, attachment.child_socket),
                            parent.world * interaction_socket(parent.pose, attachment.parent_socket)) < tolerance);
                CHECK(std::abs(length(Vec3{child.world[0], child.world[1], child.world[2]}) - 1) < tolerance);
            }
        return result;
    };
    for (unsigned frame = 0; frame <= 156; ++frame)
        (void)evaluate(frame / 60.);
    const auto sought = evaluate(1.7), replayed = evaluate(1.7);
    CHECK(sought[0].world == replayed[0].world); // A late seek does not depend on previous frames.
    const std::array<InteractionPlacement, 2> released{{{0}, {0}}};
    const auto detached = bindings.sample(sought, released);
    CHECK(detached[0].world == sought[0].world); // Cancellation keeps the actor where it was placed.
}

TEST_CASE("Invalid attachment graphs, weights and placements are rejected") {
    const Ride ride;
    CHECK_THROWS_WITH_AS(
        InteractionBindings(ride.roles, {{"rider", "vehicle", {5}, {2}}, {"vehicle", "rider", {2}, {5}}}),
        "Cyclic interaction placement ownership", std::invalid_argument);
    CHECK_THROWS_WITH_AS(InteractionBindings(ride.roles, {{"rider", "missing", {5}, {2}}}),
                         "Unknown interaction role: missing", std::out_of_range);
    CHECK_THROWS_WITH_AS(InteractionBindings(ride.roles, {{"rider", "vehicle", {99}, {2}}}),
                         "Unknown interaction socket node", std::invalid_argument);
    const InteractionBindings bindings(ride.roles, ride.attachments);
    auto frames = ride.frames();
    const std::array<InteractionPlacement, 2> released{{{0}, {0}}};
    auto invalid = released;
    invalid[0].weight = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(bindings.sample(frames, invalid), "Interaction attachment weight must be 0..1",
                         std::invalid_argument);
    frames[0].world[0] = 2;
    CHECK_THROWS_WITH_AS(bindings.sample(frames, released),
                         "Interaction actor placement and socket offsets must be rigid", std::invalid_argument);
}
