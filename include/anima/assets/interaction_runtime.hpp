#pragma once
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/interaction_bindings.hpp>

/// @file
/// Coordinated interactions: actors with their own assets and motion, driven by one shared
/// timeline and placed through a role graph.
///
/// Part of the `anima::assets` target. Documents are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Invalid documents and arguments, including
/// JSON syntax errors, missing role or phase entries and values of the wrong JSON type, throw
/// `std::invalid_argument` unless stated.

namespace anima {
/// One participant's resources.
struct InteractionActor {
    /// Model asset; not null.
    std::shared_ptr<const anima::Asset> asset;
    /// Motion bound to #asset; not null.
    std::shared_ptr<const MotionRuntime> motion;
    /// Named sockets that documents refer to.
    std::map<std::string, anima::InteractionSocket, std::less<>> sockets;
};
/// Result of InteractionRuntime::sample.
struct InteractionSample {
    anima::ActionTime clock;
    /// One frame per role, in InteractionBindings::roles order.
    std::vector<anima::InteractionFrame> frames;
    /// Outcome of one contact solve.
    struct Contact {
        /// Role whose chain was solved.
        std::string role;
        /// Chain name.
        std::string chain;
        /// Contact weight at the current phase progress.
        float weight{};
        /// See MotionEvaluation::Contact::error.
        float error{};
        bool reachable{};
    };
    /// Contact outcomes, in solve order.
    std::vector<Contact> contacts;
    /// Weight of each attachment at the current phase progress, in document order.
    std::vector<float> attachment_weights;
};

/// A decoded interaction bound to its actors. Immutable after construction; copies share it.
class InteractionRuntime {
  public:
    using Actors = std::map<std::string, InteractionActor, std::less<>>;
    /// Decodes @p document for @p actors, keyed by role id.
    ///
    /// The document has `version` 1, a nonempty `id`, `phases` (each with `id`, `duration` and
    /// optional `held` and `cues`, following the ActionTimeline rules) and `roles`, which has one
    /// entry per actor mapping every phase id to `{"layers": [...]}`, with layers as in an
    /// ActionRuntime document. `attachments` lists `child`, `parent`, `child_socket`,
    /// `parent_socket` (socket names of those actors) and `weights`. `contacts` has at most
    /// `8 * actors.size()` entries, each with `child`, `parent`, `chain`, `target_socket` (a parent
    /// socket), `pole` (three finite numbers in the child's model space), `weights` and optional
    /// `orientation` (default false; true also matches the socket's rotation). Every `weights`
    /// maps each phase id to a curve as in ActionRuntime::weight.
    ///
    /// There are 1 to 64 actors. A contact needs an attachment from its child to its parent, a
    /// chain used once per child, and a chain that does not move the child's attachment socket.
    InteractionRuntime(Actors actors, std::string_view document);
    const ActionTimeline &timeline() const;
    /// Role graph built from the attachments.
    const InteractionBindings &bindings() const;
    /// Document id.
    const std::string &id() const;
    /// Evaluates every role at @p elapsed seconds, with the held phase released at @p released.
    ///
    /// Each role's layers are evaluated over its rest pose. Roles are then placed from their
    /// @p free_worlds entries, rigid model-to-world matrices keyed by role id, through the
    /// attachments. In parent-before-child order, each role is placed on its parent's final pose
    /// and then solves its contacts toward its parent's target sockets. Throws unless
    /// @p free_worlds has one entry per role, `std::out_of_range` when its keys differ from the
    /// role ids, and as ActionTimeline::sample and InteractionBindings::sample do.
    InteractionSample sample(double elapsed, std::optional<double> released,
                             const std::map<std::string, Mat4, std::less<>> &free_worlds) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
