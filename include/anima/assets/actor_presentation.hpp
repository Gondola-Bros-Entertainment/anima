#pragma once
#include <anima/assets/interaction_runtime.hpp>
#include <anima/scene.hpp>

/// @file
/// Actor profiles, which bundle a model, its motion and named sockets.
///
/// Part of the `anima::assets` target. Documents are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Invalid documents, including JSON syntax
/// errors and values of the wrong JSON type, throw `std::invalid_argument` unless stated.
/// Socket names are caller data.

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

    /// Loads the version 2 profile at @p profile and everything it names.
    ///
    /// The profile has a nonempty `id`; `manifest`, a relative UTF-8 path without a root, colon,
    /// backslash or `..` component, resolved beside the profile unless @p manifest_override
    /// replaces it; and `sockets`, mapping each nonempty name to `node`, which must name exactly one
    /// model node, and `frame`. A frame is a rigid, right-handed model-space bind frame of 16
    /// column-major numbers, or null for the node's bind position with the model's axes. The manifest's model is
    /// validated with validate_manifest and its motion loaded with MotionRuntime::load.
    ///
    /// Throws `std::invalid_argument` also for a missing profile or one larger than 4 MiB, and as
    /// read_manifest, load_asset, validate_manifest, MotionRuntime::load and Mesh::compile do.
    explicit ActorPresentation(const std::filesystem::path &profile,
                               std::optional<std::filesystem::path> manifest_override = {});
};
} // namespace anima
