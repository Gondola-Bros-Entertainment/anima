#pragma once
#include <anima/animation.hpp>
#include <anima/assets/asset.hpp>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

/// @file
/// Model asset manifests: reading one and checking a model against it. Part of the
/// `anima::assets` target.

namespace anima {
/// A model asset manifest; see read_manifest.
struct Manifest {
    /// Absolute directory of the manifest file; #model and #motion_contract resolve against it.
    std::filesystem::path directory;
    std::string asset_id;
    /// Model GLB filename, decoded from the manifest's UTF-8.
    std::filesystem::path model;
    /// Skeleton identity that motion contracts, socket documents and fitted catalogs must match.
    std::string skeleton_id;
    /// Bind pose identity, 64 lowercase hexadecimal characters, matched by the same documents.
    std::string bind_signature;
    /// Joints in the model's single skin, in [1, 512].
    std::size_t joint_count{};
    /// Playback metadata for the model's clips; validate_manifest requires one per clip.
    std::vector<ClipMetadata> clips;
    /// Motion contract filename, decoded from the manifest's UTF-8 and read by MotionRuntime::load;
    /// empty when the manifest's `motion_contract` is null.
    std::filesystem::path motion_contract;
};
/// Reads the manifest file at @p path: JSON of 1 byte to 1 MiB with at most 32 nesting levels.
///
/// Requires the integer `version` 4, checked before any other field, then `units` `"meters"`,
/// `asset_id`, `model`, `motion_contract` (a filename, or null for none), `skeleton` (`id`,
/// `bind_signature` and an integer `joint_count`) and `clips`. Filenames are UTF-8 and name files
/// beside the manifest: nonempty, without a directory, colon, backslash or NUL, and neither `.`
/// nor `..`. Each clip has a unique `name`, `loop`, `reference_speed` (positive and finite, or null
/// for none) and `events`, each a `time` of at least 0 seconds and a nonempty `name`, as ClipEvent
/// has them; the result lists them by time. Every field is required, and unknown and repeated
/// fields are rejected.
///
/// Throws `std::invalid_argument` for a missing, empty or oversized file and for invalid content,
/// including missing, unknown and mistyped fields, and `std::runtime_error` when an opened file
/// cannot be read.
[[nodiscard]] Manifest read_manifest(const std::filesystem::path &path);
/// Checks @p asset against @p manifest: one skin of Manifest::joint_count joints, as many clips as
/// the manifest, each manifest clip naming exactly one of them, events inside their clips and
/// positive finite reference speeds. Throws `std::invalid_argument` otherwise.
void validate_manifest(const Manifest &manifest, const Asset &asset);
} // namespace anima
