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
    /// Motion, which holds the model (MotionRuntime::model()), and sockets, ready for
    /// InteractionRuntime.
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
    /// imported with load_asset and @p options, validated with validate_manifest and compiled into #render with
    /// @p texel_retention; its motion is loaded with MotionRuntime::load and @p options. With
    /// TexelRetention::until_upload, the motion's model (MotionRuntime::model()) is a copy of the model whose
    /// textures are #render's, which have no texels. @p options cancel and count the two imports, which add to one
    /// StagingProgress, and not the work between and after them.
    ///
    /// Throws `std::invalid_argument` also for a missing profile or one larger than 4 MiB, and as
    /// read_manifest, load_asset, validate_manifest, Mesh::compile and MotionRuntime::load do, including
    /// StagingCancelled.
    ///
    /// May run concurrently on any thread. Reads the files it loads and the C locale, which must not
    /// change during the call, as prefab.hpp describes for documents.
    explicit ActorPresentation(const std::filesystem::path &profile,
                               std::optional<std::filesystem::path> manifest_override = {},
                               TexelRetention texel_retention = TexelRetention::keep,
                               const StagingOptions &options = {});
};
} // namespace anima
