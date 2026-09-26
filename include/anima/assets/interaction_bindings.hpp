#pragma once
#include <anima/assets/evaluation.hpp>
#include <map>
#include <set>

/// @file
/// Placement graph for coordinated interactions between independently evaluated actors.
///
/// Part of the `anima::assets` target. Each role is one actor. Attachments move actor world
/// frames; they never transfer one actor's joints or skin palette to another. Failures throw
/// `std::invalid_argument` (or anima::MathError, which derives from it) unless stated; a role id
/// argument that the bindings lack throws `std::out_of_range`.

namespace anima {
/// One participant.
struct InteractionRole {
    /// Unique, nonempty role id.
    std::string id;
    /// The participant's asset; not null.
    std::shared_ptr<const Asset> asset;
};
/// A frame on a node of a role's asset.
struct InteractionSocket {
    /// Node index in the role's asset.
    std::size_t node{};
    /// Finite, affine frame relative to the node, with a positive determinant. A bind-space frame
    /// may cancel an authored joint's stretch.
    Mat4 local = identity();
};
/// Places a child role by aligning its socket with a socket of its parent role.
struct InteractionAttachment {
    /// Role that is placed; each role has at most one parent.
    std::string child;
    /// Role it is placed on.
    std::string parent;
    /// Socket on the child's asset.
    InteractionSocket child_socket;
    /// Socket on the parent's asset.
    InteractionSocket parent_socket;
};
/// One role's evaluated pose and world placement.
struct InteractionFrame {
    /// Model-space pose; only the world matrices are used.
    Pose pose;
    /// Model-to-world placement.
    Mat4 world = identity();
};
/// How strongly one attachment applies.
struct InteractionPlacement {
    /// Blend from the child's free placement toward the attached one, in [0, 1].
    float weight{};
    /// Rigid offset in the parent's evaluated socket frame.
    Mat4 offset = identity();
};

/// Rigid model-space frame of @p socket in @p pose: its position and rotation without scale or
/// shear, so a stretched joint cannot stretch an attached actor. Throws `std::out_of_range` for a
/// node outside @p pose.
[[nodiscard]] inline Mat4 interaction_socket(const Pose &pose, const InteractionSocket &socket) {
    const auto frame = pose.world.at(socket.node) * socket.local;
    return matrix(Transform{point(frame, {}), affine_rotation(frame), {1, 1, 1}});
}
/// World placement of a child role: @p free_world at weight 0; at weight 1, the placement that puts
/// the child's socket on the parent's socket times the placement offset; in between, blend_affine
/// of the two.
[[nodiscard]] inline Mat4 interaction_world(const InteractionFrame &parent, const InteractionSocket &parent_socket,
                                            const Pose &child, const InteractionSocket &child_socket,
                                            const Mat4 &free_world, const InteractionPlacement &placement) {
    if (placement.weight == 0)
        return free_world;
    const auto desired = parent.world * interaction_socket(parent.pose, parent_socket) * placement.offset *
                         inverse(interaction_socket(child, child_socket));
    return placement.weight == 1 ? desired : blend_affine(free_world, desired, placement.weight);
}

/// A validated role graph that places child roles on their parents.
class InteractionBindings {
  public:
    /// Throws unless there are 1 to 64 roles with unique nonempty ids and assets, fewer attachments
    /// than roles, each attachment joins two different known roles, no role has two parents, the
    /// graph is acyclic, and every socket names a node of its role's asset with a valid local frame.
    InteractionBindings(std::vector<InteractionRole> roles, std::vector<InteractionAttachment> attachments)
        : roles_(std::move(roles)), attachments_(std::move(attachments)) {
        if (roles_.empty() || roles_.size() > 64 || attachments_.size() >= roles_.size())
            throw std::invalid_argument("Invalid interaction role or attachment count");
        for (std::size_t i = 0; i < roles_.size(); ++i) {
            const auto &role = roles_[i];
            if (role.id.empty() || !role.asset || !names_.emplace(role.id, i).second)
                throw std::invalid_argument("Missing or duplicate interaction role");
        }
        parents_.resize(roles_.size(), -1);
        for (std::size_t i = 0; i < attachments_.size(); ++i) {
            const auto &attachment = attachments_[i];
            const auto child = role(attachment.child), parent = role(attachment.parent);
            if (child == parent || parents_[child] != -1)
                throw std::invalid_argument("An interaction role must have one placement owner");
            validate_socket(child, attachment.child_socket);
            validate_socket(parent, attachment.parent_socket);
            parents_[child] = static_cast<int>(i);
        }
        std::vector<unsigned> marks(roles_.size());
        const auto visit = [&](const auto &self, std::size_t child) -> void {
            if (marks[child] == 2)
                return;
            if (marks[child] == 1)
                throw std::invalid_argument("Cyclic interaction placement ownership");
            marks[child] = 1;
            if (parents_[child] >= 0) {
                const auto binding = static_cast<std::size_t>(parents_[child]);
                self(self, role(attachments_[binding].parent));
                order_.push_back(binding);
            }
            marks[child] = 2;
            role_order_.push_back(child);
        };
        for (std::size_t i = 0; i < roles_.size(); ++i)
            visit(visit, i);
    }

    /// Index of role @p id. Throws `std::out_of_range` for an unknown id.
    [[nodiscard]] std::size_t role(std::string_view id) const {
        const auto found = names_.find(id);
        if (found == names_.end())
            throw std::out_of_range("Unknown interaction role: " + std::string(id));
        return found->second;
    }
    /// Roles in construction order; role indices refer to it.
    const auto &roles() const { return roles_; }
    const auto &attachments() const { return attachments_; }
    /// Role indices with every parent before its children.
    const auto &role_order() const { return role_order_; }

    /// Places each attached child on its parent, parents first, and returns the frames; poses are
    /// unchanged.
    ///
    /// @p frames needs one entry per role, in role order, with one finite world matrix per node of
    /// the role's asset and a rigid placement. @p placements needs one entry per attachment, with
    /// a weight in [0, 1] and a rigid offset. A weight of 0 keeps the child's free placement.
    /// Rigid means a rotation and translation within `1e-4`.
    [[nodiscard]] std::vector<InteractionFrame> sample(std::span<const InteractionFrame> frames,
                                                       std::span<const InteractionPlacement> placements) const {
        if (frames.size() != roles_.size() || placements.size() != attachments_.size())
            throw std::invalid_argument("Interaction frame/placement count mismatch");
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].pose.world.size() != roles_[i].asset->nodes.size())
                throw std::invalid_argument("Interaction pose belongs to a different role rig");
            rigid(frames[i].world);
            for (const auto &world : frames[i].pose.world)
                if (!std::all_of(world.begin(), world.end(), [](float value) { return std::isfinite(value); }))
                    throw std::invalid_argument("Non-finite interaction pose");
        }
        for (const auto &placement : placements) {
            if (!std::isfinite(placement.weight) || placement.weight < 0 || placement.weight > 1)
                throw std::invalid_argument("Interaction attachment weight must be 0..1");
            rigid(placement.offset);
        }
        std::vector<InteractionFrame> result(frames.begin(), frames.end());
        for (const auto index : order_) {
            const auto &attachment = attachments_[index];
            const auto &placement = placements[index];
            if (placement.weight == 0)
                continue;
            auto &child = result[role(attachment.child)];
            const auto &parent = result[role(attachment.parent)];
            child.world = interaction_world(parent, attachment.parent_socket, child.pose, attachment.child_socket,
                                            child.world, placement);
        }
        return result;
    }

  private:
    static void rigid(const Mat4 &value) {
        if (!std::all_of(value.begin(), value.end(), [](float x) { return std::isfinite(x); }))
            throw std::invalid_argument("Non-finite interaction placement");
        const auto rotation = affine_rotation(value);
        const auto expected = matrix(Transform{point(value, {}), rotation, {1, 1, 1}});
        for (std::size_t i = 0; i < value.size(); ++i)
            if (std::abs(value[i] - expected[i]) > 1e-4F)
                throw std::invalid_argument("Interaction actor placement and socket offsets must be rigid");
    }
    void validate_socket(std::size_t owner, const InteractionSocket &socket) const {
        if (socket.node >= roles_[owner].asset->nodes.size())
            throw std::invalid_argument("Unknown interaction socket node");
        // A bind-space calibration may cancel stretch on an authored joint.
        // interaction_socket produces the rigid evaluated frame for placement.
        (void)affine_rotation(socket.local);
    }
    std::vector<InteractionRole> roles_;
    std::vector<InteractionAttachment> attachments_;
    std::map<std::string, std::size_t, std::less<>> names_;
    std::vector<int> parents_;
    std::vector<std::size_t> order_, role_order_;
};
} // namespace anima
