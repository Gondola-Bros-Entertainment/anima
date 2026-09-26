#pragma once
#include <algorithm>
#include <anima/assets/evaluation.hpp>

/// @file
/// Alignment between independently animated participants of an interaction: shared phase timing
/// and frames on posed nodes.
///
/// Part of the `anima::assets` target. No anatomy is implied; callers own actor state, phase
/// selection, transitions, collision and authority. Failures throw `std::invalid_argument` (or
/// anima::MathError, which derives from it); node indices outside a pose throw
/// `std::out_of_range`.

namespace anima {
/// Maps a shared interaction phase to one participant's clip time.
struct PhaseKey {
    /// Shared phase, in [0, 1].
    double phase{};
    /// Participant clip time in seconds.
    double time{};
};
/// Piecewise-linear map from a shared interaction phase to one participant's clip time.
///
/// Interior keys align meaningful markers, for example a gait contact, when participants place
/// them at different clip times.
class PhaseTrack {
  public:
    /// Throws unless there are at least 2 finite keys whose phases and times strictly increase,
    /// the first key is (0, 0) and the last has phase 1.
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
    /// Clip time at @p phase, which must be finite and at least 0. With @p loop the phase wraps
    /// into [0, 1); otherwise phases above 1 give the last key's time.
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

/// A frame attached to a node of a posed participant.
struct PoseFrame {
    /// Node index in the pose.
    std::size_t node{};
    /// Frame relative to the node.
    Mat4 local = identity();
};
/// Model-space matrix of @p frame in @p pose. Throws unless the result is a finite affine frame
/// with a positive determinant.
[[nodiscard]] inline Mat4 pose_frame(const Pose &pose, const PoseFrame &frame) {
    const auto result = pose.world.at(frame.node) * frame.local;
    (void)affine_rotation(result); // Require a finite, invertible affine frame.
    return result;
}
/// World placement of the follower that puts its @p anchor frame on the leader's moving
/// @p attachment frame, with the leader placed at @p leader_world.
///
/// Only position and rotation transfer, so a large leader or a scaled attachment joint does not
/// resize the follower.
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
/// Another participant's animated @p marker in this actor's model space, for a contact solve.
/// When the marker targets a grip, post-multiply the result by the inverse of the holding
/// socket's local frame.
[[nodiscard]] inline Mat4 interaction_contact(const Mat4 &actor_world, const Mat4 &other_world, const Pose &other,
                                              const PoseFrame &marker) {
    (void)affine_rotation(actor_world);
    (void)affine_rotation(other_world);
    return inverse(actor_world) * other_world * pose_frame(other, marker);
}
} // namespace anima
