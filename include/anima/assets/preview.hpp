#pragma once
#include <anima/animation.hpp>

/// @file
/// Model asset manifests, skin compatibility checks and a manifest-driven model viewer. Part of
/// the `anima::assets` target.

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
/// Pairs each joint node of @p fitted's skin with the @p body joint node of the same name, as
/// (fitted node, body node) in @p fitted's joint order, so that @p fitted can copy @p body's pose.
///
/// Both assets need exactly one skin, with equal joint counts and unique joint names. Matching
/// joints need the same ancestor names and inverse bind and rest world matrices within `1e-4`, and
/// every @p fitted primitive must use the skin. Throws `std::runtime_error` otherwise.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> compatible_skin(const Asset &body, const Asset &fitted);
/// Model viewer driven by a manifest: owns a Scene with one object that an Animator poses. It
/// shows the model alone; applications assemble attachments and fitted meshes.
class AssetPreview {
  public:
    /// Loads the manifest at @p manifest and its model, compiles the model's Mesh with
    /// @p texel_retention and keeps only the model's nodes and clips for its Animator, so that the
    /// Mesh alone holds the geometry and textures; then plays the first manifest clip, or shows the
    /// bind pose when there is none. Throws as read_manifest, load_asset, validate_manifest and
    /// Mesh::compile do.
    explicit AssetPreview(const std::filesystem::path &manifest, TexelRetention texel_retention = TexelRetention::keep);
    AssetPreview(const AssetPreview &) = delete;
    AssetPreview &operator=(const AssetPreview &) = delete;
    AssetPreview(AssetPreview &&) noexcept = default;
    AssetPreview &operator=(AssetPreview &&) noexcept = default;
    /// Selects the manifest clip named @p clip with its manifest metadata at its start, playing or
    /// paused as @p start says. Throws `std::out_of_range` for a clip the manifest does not
    /// declare.
    void select(std::string_view clip, PlaybackStart start = PlaybackStart::playing);
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
    // The model's nodes and clips, which the Animator samples.
    std::shared_ptr<const Asset> model_;
    std::shared_ptr<Scene> scene_ = std::make_shared<Scene>();
    ComponentRef<Animator> animator_;
    bool bind_{};
};
} // namespace anima
