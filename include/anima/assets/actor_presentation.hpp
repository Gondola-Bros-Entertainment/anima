#pragma once
#include <anima/assets/interaction_runtime.hpp>
#include <anima/scene.hpp>

/// @file
/// Actor profiles, which bundle a model, its motion, named sockets and capabilities, and action-set
/// catalogs, which resolve action variants by capability.
///
/// Part of the `anima::assets` target. Documents are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Invalid documents, including JSON syntax
/// errors and values of the wrong JSON type, throw `std::invalid_argument` unless stated.
/// Capability, action and socket names are caller data.

namespace anima {
/// A loaded actor profile.
struct ActorPresentation {
    /// Profile id.
    std::string id;
    /// The profile's manifest.
    anima::Manifest manifest;
    /// Model, motion and sockets, ready for InteractionRuntime.
    InteractionActor actor;
    /// Mesh compiled from the model.
    std::shared_ptr<const anima::Mesh> render;
    /// Capabilities the profile declares, for ActionSetCatalog::resolve.
    std::set<std::string, std::less<>> capabilities;

    /// Loads the version 1 profile at @p profile and everything it names.
    ///
    /// The profile has a nonempty `id`; `manifest`, a relative path without `..` resolved beside
    /// the profile, unless @p manifest_override replaces it; unique nonempty `capabilities`; and
    /// `sockets`, mapping each nonempty name to `node`, which must name exactly one model node, and
    /// `frame`. A frame is a rigid, right-handed model-space bind frame of 16 column-major numbers,
    /// or null for the node's bind position with the model's axes. The manifest's model is
    /// validated with validate_manifest and its motion loaded with MotionRuntime::load.
    ///
    /// Throws `std::invalid_argument` also for a missing profile or one larger than 4 MiB, and as
    /// read_manifest, load_asset, validate_manifest and MotionRuntime::load do.
    explicit ActorPresentation(const std::filesystem::path &profile,
                               std::optional<std::filesystem::path> manifest_override = {});
};
/// Named action sets whose slots resolve to action variants by capability.
class ActionSetCatalog {
  public:
    /// Decodes @p document, which has `version` 1 and `sets`: each nonempty set name maps to a
    /// nonempty object of nonempty slot names, and each slot to 1 to 64 variants of `action`,
    /// defined in @p actions, and `requires`, a list of unique capabilities.
    ActionSetCatalog(std::string_view document, const ActionRuntime &actions);
    /// Action of the most specific variant of @p set and @p slot: the one with the most
    /// requirements, all of which @p capabilities include; see resolve_action. Throws for an
    /// unknown set or slot, no compatible variant or a tie.
    const std::string &resolve(std::string_view set, std::string_view slot, const Capabilities &capabilities) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
