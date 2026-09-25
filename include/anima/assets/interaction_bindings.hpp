#pragma once
#include <anima/assets/evaluation.hpp>
#include <map>
#include <set>

namespace anima {
// A role is an independently evaluated actor. These bindings move actor world
// frames, never transfer one actor's joints or skin palette to another actor.
struct InteractionRole {
    std::string id;
    std::shared_ptr<const Asset> asset;
};
struct InteractionSocket {
    std::size_t node{};
    Mat4 local = identity();
};
struct InteractionAttachment {
    std::string child, parent;
    InteractionSocket child_socket, parent_socket;
};
struct InteractionFrame {
    Pose pose;
    Mat4 world = identity();
};
struct InteractionPlacement {
    float weight{};
    Mat4 offset = identity(); // In the parent's evaluated socket frame.
};

// Root attachment sockets retain evaluated position and orientation. Skin
// stretch/shear remains inside that actor's pose; it cannot stretch the rider.
[[nodiscard]] inline Mat4 interaction_socket(const Pose &pose, const InteractionSocket &socket) {
    const auto frame = pose.world.at(socket.node) * socket.local;
    return matrix(Transform{point(frame, {}), affine_rotation(frame), {1, 1, 1}});
}
[[nodiscard]] inline Mat4 interaction_world(const InteractionFrame &parent, const InteractionSocket &parent_socket,
                                            const Pose &child, const InteractionSocket &child_socket,
                                            const Mat4 &free_world, const InteractionPlacement &placement) {
    if (placement.weight == 0)
        return free_world;
    const auto desired = parent.world * interaction_socket(parent.pose, parent_socket) * placement.offset *
                         inverse(interaction_socket(child, child_socket));
    return placement.weight == 1 ? desired : blend_affine(free_world, desired, placement.weight);
}

class InteractionBindings {
  public:
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

    [[nodiscard]] std::size_t role(std::string_view id) const {
        const auto found = names_.find(id);
        if (found == names_.end())
            throw std::invalid_argument("Unknown interaction role: " + std::string(id));
        return found->second;
    }
    const auto &roles() const { return roles_; }
    const auto &attachments() const { return attachments_; }
    const auto &role_order() const { return role_order_; }

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
