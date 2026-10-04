#pragma once
#include <anima/animation.hpp>
#include <anima/assets/manifest.hpp>

/// @file
/// A manifest-driven model viewer. Part of the `anima::assets` target.

namespace anima {
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
    void select(const std::string &clip, PlaybackStart start = PlaybackStart::playing);
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
