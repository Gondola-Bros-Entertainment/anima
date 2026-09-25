#include <anima/assets/action.hpp>
#include <anima/assets/interaction_bindings.hpp>
#include <iostream>
#include <limits>

namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid interaction was accepted");
}
float error(const anima::Mat4 &a, const anima::Mat4 &b) {
    float result = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}
} // namespace
int main() {
    try {
        using namespace anima;
        auto vehicle = std::make_shared<Asset>(), rider = std::make_shared<Asset>(),
             passenger = std::make_shared<Asset>();
        vehicle->nodes.resize(3);
        rider->nodes.resize(7);
        passenger->nodes.resize(2);
        const std::vector<InteractionRole> roles{{"rider", rider}, {"vehicle", vehicle}, {"passenger", passenger}};
        const std::vector<InteractionAttachment> attachments{{"passenger", "rider", {0}, {6}},
                                                             {"rider", "vehicle", {5}, {2}}};
        InteractionBindings bindings(roles, attachments);
        ActionTimeline timeline({{"enter", .5, false, {}}, {"ride", .8, true, {}}, {"exit", .5, false, {}}});
        const auto evaluate = [&](double time) {
            const auto clock = timeline.sample(time, 2.1);
            const auto weight = static_cast<float>(clock.phase == 0   ? clock.progress
                                                   : clock.phase == 1 ? 1
                                                                      : 1 - clock.progress);
            std::vector<InteractionFrame> frames{
                {sample_pose(*rider)}, {sample_pose(*vehicle)}, {sample_pose(*passenger)}};
            frames[0].pose.world[5] = matrix(Transform{{0, 1, 0}});
            frames[0].pose.world[6] = matrix(Transform{{0, 1.3F, -.2F}});
            frames[1].pose.world[2] = matrix(
                Transform{{0, 1.4F + .1F * std::sin(static_cast<float>(time)), 0}, {0, 0, 0, 1}, {1.3F, .8F, 1.1F}});
            frames[1].world = matrix(
                Transform{{static_cast<float>(time), 0, -2},
                          {0, std::sin(static_cast<float>(time) * .1F), 0, std::cos(static_cast<float>(time) * .1F)}});
            frames[0].world = matrix(Transform{{-1, 0, 0}});
            frames[2].world = matrix(Transform{{1, 0, 0}});
            const std::array<InteractionPlacement, 2> placements{{{weight}, {weight}}};
            const auto result = bindings.sample(frames, placements);
            for (std::size_t i = 0; i < result.size(); ++i)
                check(result[i].pose.world == frames[i].pose.world, "Attachment changed a participant's anatomy");
            check(result[1].world == frames[1].world, "Attachment changed driver placement");
            if (weight == 0) {
                check(result[0].world == frames[0].world && result[2].world == frames[2].world,
                      "Zero contact weight changed free placement");
            }
            if (weight == 1)
                for (const auto &attachment : attachments) {
                    const auto &child = result[bindings.role(attachment.child)];
                    const auto &parent = result[bindings.role(attachment.parent)];
                    check(error(child.world * interaction_socket(child.pose, attachment.child_socket),
                                parent.world * interaction_socket(parent.pose, attachment.parent_socket)) < 1e-5F,
                          "Moving socket alignment drifted");
                    check(std::abs(length(Vec3{child.world[0], child.world[1], child.world[2]}) - 1) < 1e-5F,
                          "Scaled seat changed rider size");
                }
            return result;
        };
        for (unsigned frame = 0; frame <= 156; ++frame)
            (void)evaluate(frame / 60.);
        const auto sought = evaluate(1.7), replayed = evaluate(1.7);
        check(sought[0].world == replayed[0].world, "Late seek depends on previous frames");
        auto cancelled = sought;
        const std::array<InteractionPlacement, 2> released{{{0}, {0}}};
        const auto detached = bindings.sample(cancelled, released);
        check(detached[0].world == sought[0].world, "Cancellation reset the actor placement");
        rejects([&] {
            (void)InteractionBindings(roles, {{"rider", "vehicle", {5}, {2}}, {"vehicle", "rider", {2}, {5}}});
        });
        rejects([&] { (void)InteractionBindings(roles, {{"rider", "missing", {5}, {2}}}); });
        rejects([&] { (void)InteractionBindings(roles, {{"rider", "vehicle", {99}, {2}}}); });
        auto invalid = released;
        invalid[0].weight = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { (void)bindings.sample(cancelled, invalid); });
        cancelled[0].world[0] = 2;
        rejects([&] { (void)bindings.sample(cancelled, released); });
        std::cout
            << "PASS coordinated interaction ownership, moving sockets, phase entry/exit, cancellation and late seek\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
