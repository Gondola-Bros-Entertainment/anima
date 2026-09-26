#pragma once
#include <anima/animation.hpp>

/// @file
/// Character asset manifests, skin compatibility checks and a manifest-driven model viewer. Part
/// of the `anima::assets` target.

namespace anima {
/// One `equipment` entry of a manifest. Anima checks only that ids are unique and that #model is
/// a filename; the other fields are caller data.
struct EquipmentMetadata {
    /// Unique within the manifest.
    std::string id;
    std::string slot;
    /// Optional model filename beside the manifest.
    std::string model;
    std::string mesh;
    std::string skeleton;
    std::string socket;
    bool included{};
};
/// A character asset manifest; see read_manifest.
struct Manifest {
    /// Absolute directory of the manifest file; #model and #motion_contract resolve against it.
    std::filesystem::path directory;
    std::string asset_id;
    /// Model GLB filename.
    std::string model;
    /// Skeleton identity that motion contracts, socket documents and fitted catalogs must match.
    std::string skeleton_id;
    /// Bind pose identity, 64 lowercase hexadecimal characters, matched by the same documents.
    std::string bind_signature;
    /// Joints in the model's single skin, in [1, 512].
    std::size_t joint_count{};
    /// Playback metadata for the model's clips; validate_manifest requires one per clip.
    std::vector<ClipMetadata> clips;
    std::vector<EquipmentMetadata> equipment;
    /// Optional motion contract filename, read by MotionRuntime::load.
    std::string motion_contract;
};
/// Reads the manifest file at @p path: JSON of 1 byte to 1 MiB with at most 32 nesting levels.
///
/// Requires `schema_version` 1, `units` `"meters"`, `asset_id`, `model`, `skeleton` (`id`,
/// `bind_signature` and an integer `joint_count`), `clips` and `equipment`, and accepts a
/// `motion_contract`. Filenames must name files beside the manifest, without directories. Each
/// clip has a unique `name`, `loop`, an optional positive finite `reference_speed` and optional
/// `events` of `time_seconds` (at least 0) and a nonempty `event` name, sorted by time. Each
/// equipment entry has a unique `id`, a `slot` and optional `model`, `mesh`, `skeleton`, `socket`
/// and `included`. Duplicate fields are rejected and unknown fields ignored.
///
/// Throws `std::runtime_error` for an unreadable file or invalid content, including missing fields
/// and fields of the wrong JSON type.
[[nodiscard]] Manifest read_manifest(const std::filesystem::path &path);
/// Checks @p asset against @p manifest: one skin of Manifest::joint_count joints, as many clips as
/// the manifest, each manifest clip naming exactly one of them, events inside their clips and
/// positive finite reference speeds. Throws `std::runtime_error` otherwise.
void validate_manifest(const Manifest &manifest, const Asset &asset);
/// Pairs each joint node of @p equipment's skin with the @p character joint node of the same
/// name, as (equipment node, character node) in @p equipment's joint order.
///
/// Both assets need exactly one skin, with equal joint counts and unique joint names. Matching
/// joints need the same ancestor names and inverse bind and rest world matrices within `1e-4`, and
/// every @p equipment primitive must use the skin. Throws `std::runtime_error` otherwise.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> compatible_skin(const Asset &character,
                                                                               const Asset &equipment);
/// Model viewer driven by a manifest: owns a Scene with one object that an Animator poses.
/// Equipment assembly belongs to applications.
class AssetPreview {
  public:
    /// Loads the manifest at @p manifest and its model, then plays the first manifest clip, or
    /// shows the bind pose when there is none. Throws as read_manifest, load_asset and
    /// validate_manifest do.
    explicit AssetPreview(const std::filesystem::path &manifest);
    AssetPreview(const AssetPreview &) = delete;
    AssetPreview &operator=(const AssetPreview &) = delete;
    AssetPreview(AssetPreview &&) noexcept = default;
    AssetPreview &operator=(AssetPreview &&) noexcept = default;
    /// Selects the manifest clip named @p clip with its manifest metadata, playing it from the
    /// start when @p play is true. Throws `std::runtime_error` for a clip the manifest does not
    /// declare.
    void select(const std::string &clip, bool play = true);
    /// Shows the rest pose and pauses.
    void bind_pose();
    /// Resumes the clip when paused or showing the bind pose, otherwise pauses it. Does nothing
    /// before a clip is selected.
    void toggle_play();
    /// Plays the clip from its start. Does nothing before a clip is selected.
    void restart();
    /// Moves the clip to @p time seconds without events; see Playback::seek.
    void seek(double time);
    /// Updates the preview scene by @p elapsed seconds and returns the clip events crossed.
    [[nodiscard]] std::vector<ClipEvent> advance(double elapsed);
    /// The scene to render.
    [[nodiscard]] std::shared_ptr<const Scene> render_scene() const noexcept { return scene_; }
    /// The last published pose.
    [[nodiscard]] const Pose &pose() const noexcept { return animator_->pose(); }
    [[nodiscard]] const Playback &playback() const noexcept { return animator_->playback(); }
    /// Whether the bind pose is shown.
    [[nodiscard]] bool is_bind() const noexcept { return bind_; }
    /// One-line human-readable state: the clip and time, or the bind pose, and whether it plays.
    [[nodiscard]] std::string status() const;
    [[nodiscard]] const Manifest &manifest() const noexcept { return manifest_; }

  private:
    Manifest manifest_;
    std::shared_ptr<const Asset> character_;
    std::shared_ptr<Scene> scene_ = std::make_shared<Scene>();
    ComponentRef<Animator> animator_;
    bool bind_{};
};
} // namespace anima
