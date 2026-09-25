#pragma once
#include <algorithm>
#include <anima/assets/evaluation.hpp>

namespace anima {
// Each participant maps one shared interaction phase to its own authored clip.
// Additional keys align meaningful markers (for example a gait contact) even
// when the two performances put those markers at different clip times.
struct PhaseKey {
    double phase{}, time{};
};
class PhaseTrack {
  public:
    explicit PhaseTrack(std::vector<PhaseKey> keys) : keys_(std::move(keys)) {
        if (keys_.size() < 2 || keys_.front().phase != 0 || keys_.back().phase != 1 || keys_.front().time != 0)
            throw std::invalid_argument("Interaction phase track must cover phase 0..1 and begin at time zero");
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            const auto &k = keys_[i];
            if (!std::isfinite(k.phase) || !std::isfinite(k.time) || k.time < 0 ||
                (i && (k.phase <= keys_[i - 1].phase || k.time <= keys_[i - 1].time)))
                throw std::invalid_argument("Interaction marker phases and times must increase strictly");
        }
    }
    [[nodiscard]] double time(double phase, bool loop) const {
        if (!std::isfinite(phase) || phase < 0)
            throw std::invalid_argument("Invalid interaction phase");
        phase = loop ? phase - std::floor(phase) : std::min(phase, 1.);
        if (phase == 1)
            return keys_.back().time;
        const auto end = std::upper_bound(keys_.begin(), keys_.end(), phase,
                                          [](double p, const PhaseKey &key) { return p < key.phase; });
        const auto &a = *(end - 1);
        const auto &b = *end;
        return a.time + (b.time - a.time) * (phase - a.phase) / (b.phase - a.phase);
    }

  private:
    std::vector<PhaseKey> keys_;
};

struct PoseFrame {
    std::size_t node{};
    Mat4 local = identity();
};
[[nodiscard]] inline Mat4 pose_frame(const Pose &pose, const PoseFrame &frame) {
    const auto result = pose.world.at(frame.node) * frame.local;
    (void)affine_rotation(result); // Require a finite, invertible affine frame.
    return result;
}
// Align an authored participant anchor with another participant's moving
// attachment. No human/mount anatomy is implied. The caller owns actor state,
// phase selection, transition policy and gameplay collision/authority.
[[nodiscard]] inline Mat4 align_interaction(const Mat4 &leader_world, const Pose &leader, const PoseFrame &attachment,
                                            const Pose &follower, const PoseFrame &anchor) {
    (void)affine_rotation(leader_world);
    const auto rigid = [](const Mat4 &value) {
        Transform frame;
        frame.translation = point(value, {});
        frame.rotation = affine_rotation(value);
        return matrix(frame);
    };
    // A large mount or a scaled seat joint must not resize its rider. Transfer
    // the attachment's position and rotation, retaining the follower's anatomy.
    return rigid(leader_world * pose_frame(leader, attachment)) * inverse(rigid(pose_frame(follower, anchor)));
}
// Convert another participant's animated marker into this actor's model space
// for a contact solve. Include inverse(hand socket) when a marker targets a grip.
[[nodiscard]] inline Mat4 interaction_contact(const Mat4 &actor_world, const Mat4 &other_world, const Pose &other,
                                              const PoseFrame &marker) {
    (void)affine_rotation(actor_world);
    (void)affine_rotation(other_world);
    return inverse(actor_world) * other_world * pose_frame(other, marker);
}
} // namespace anima
