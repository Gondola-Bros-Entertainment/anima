#pragma once
#include <anima/prefab.hpp>

/// @file
/// Scene cameras and explicit view selection. Part of the `anima::assets` target; no SDL, Vulkan,
/// physics backend or display is required. Applications own camera movement, switching policy and
/// the drawable's aspect ratio.

namespace anima {
class SceneSet;
/// Camera lens type.
enum class CameraProjection {
    perspective, ///< Uses CameraSettings::vertical_fov_degrees.
    orthographic ///< Uses CameraSettings::orthographic_height; the width follows the aspect ratio.
};

/// Lens of a Camera. Every field must be finite and valid even when the projection does not use it,
/// so switching projection keeps the authored values.
struct CameraSettings {
    CameraProjection projection = CameraProjection::perspective;
    /// Full vertical field of view, in [1, 179] degrees.
    float vertical_fov_degrees = 45;
    /// Full vertical extent in world units, in [0.0001, 1,000,000,000].
    float orthographic_height = 10;
    /// Near clip distance, at least 0.0001.
    float near_plane = .1F;
    /// Far clip distance, greater than #near_plane and at most 1,000,000,000.
    float far_plane = 1000;
};

/// Component that projects from its object's world transform, looking along local -Z with local +Y
/// up and +X right.
///
/// When a view is resolved, the object's world axes must be at least 0.0001 long, orthogonal within
/// 0.0001 after normalization, and right-handed; their lengths (positive scale) do not affect the
/// view. Shear, which a scaled parent with a rotated child can produce, and reflection are rejected
/// then, not when the transform is set.
class Camera {
  public:
    /// Throws `std::invalid_argument` for invalid @p settings.
    explicit Camera(CameraSettings settings = {});
    [[nodiscard]] const CameraSettings &settings() const { return settings_; }
    /// Replaces the settings after validating all of them, including the unused lens field. Throws
    /// `std::invalid_argument` for invalid @p settings, keeping the previous ones.
    void configure(CameraSettings settings);
    /// Projection of the current settings for @p aspect (width / height), for view space looking down -Z: perspective()
    /// with CameraSettings::vertical_fov_degrees converted to radians and rounded to `float`, or orthographic() with
    /// CameraSettings::orthographic_height, each between the near and far planes. The result is column-major in Vulkan
    /// clip space: framebuffer Y down, reversed depth, 1 at the near plane and 0 at the far plane. It reads only the
    /// settings, so it needs no scene or object.
    ///
    /// Throws `std::invalid_argument` when @p aspect is not finite and positive. An element too large for `float`, as
    /// from a tiny @p aspect, is infinite, as those builders document; resolve_camera() rejects it.
    [[nodiscard]] Mat4 projection(float aspect) const;

  private:
    CameraSettings settings_;
};

/// Component that selects the Camera for resolve_camera.
///
/// Any number of views and cameras may exist, but resolve_camera needs exactly one active view. The
/// link addresses whichever Camera its object has when the view is resolved: removing that
/// component makes resolution fail until a Camera is attached again, while destroying the object
/// breaks the link for good; resolution never looks the object up by name or key. When the
/// object's scene leaves a SceneSet, SceneSet::replace rebinds the link to the replacement's object
/// with the same key and SceneSet::unload sets it to null, given codecs from
/// add_camera_component_codecs; otherwise the link expires with the scene. A null link can be
/// persisted but cannot produce a view.
struct CameraView {
    /// Object whose Camera is used. `add_component<CameraView>(camera)` sets it; without an argument
    /// it is null.
    GameObject camera;
};

/// Matrices and world position of the camera that resolve_camera() selects, column-major.
struct CameraMatrices {
    /// World to view space: the inverse of the camera object's world rotation and translation, without its scale.
    /// View space has +X right, +Y up and the view direction along -Z, as look_at() gives.
    Mat4 view{};
    /// Camera::projection() of the selected camera for the resolved aspect.
    Mat4 projection{};
    /// `projection * view`, finite and invertible in `float`.
    Mat4 view_projection{};
    /// The camera object's world position, the eye of #view.
    Vec3 position{};
};

/// Resolves the one active CameraView in @p scene and returns its camera's matrices and position for @p aspect
/// (width / height).
///
/// Exactly one CameraView must be active, even when several point at the same camera, and its object must be live in
/// @p scene with an active Camera; other cameras need not be disabled. CameraMatrices::view_projection is in Vulkan
/// clip space: framebuffer Y down, reversed depth, 1 at the near plane and 0 at the far plane. Call it on idle scenes
/// after updates; it runs no hooks, advances no time, keeps no reference and touches no renderer.
///
/// Throws `std::invalid_argument` for a missing, ambiguous, stale or foreign selection, a missing or inactive Camera,
/// an aspect that is not finite and positive, invalid camera axes, or a view-projection that is not finite and
/// invertible in `float`; there is no fallback camera. Throws `std::logic_error` while the scene is running component
/// hooks or cleanup, constructing a component, held by a scene driver or being destroyed.
[[nodiscard]] CameraMatrices resolve_camera(Scene &scene, float aspect);
/// Resolves the view across every member of @p scenes as resolve_camera(Scene &, float) does; the camera may be in a
/// different member than its view, and SceneSet::active plays no part. Also throws `std::logic_error` while the set is
/// busy, as SceneSet describes.
[[nodiscard]] CameraMatrices resolve_camera(SceneSet &scenes, float aspect);
/// The CameraMatrices::view_projection of resolve_camera(Scene &, float) for @p scene and @p aspect, with its
/// failures. VulkanRenderer::set_view takes this matrix.
[[nodiscard]] Mat4 view_projection(Scene &scene, float aspect);
/// The CameraMatrices::view_projection of resolve_camera(SceneSet &, float) for @p scenes and @p aspect, with its
/// failures.
[[nodiscard]] Mat4 view_projection(SceneSet &scenes, float aspect);

/// Registers the `anima.camera.v1` and `anima.camera-view.v1` codecs in @p codecs, both or neither.
///
/// A camera payload is a JSON object with exactly `projection` (`"perspective"` or
/// `"orthographic"`), `vertical_fov_degrees`, `orthographic_height`, `near_plane` and `far_plane`
/// (numbers). A view payload has exactly `camera`: the target's key from the operation's
/// ObjectReferences as a decimal string, `"0"` for null. Decoding throws `std::invalid_argument` for
/// malformed JSON, payloads over 64 KiB or nested deeper than 16, duplicate, missing or unknown
/// fields, and invalid settings. A view whose target lacks an active Camera still loads;
/// resolve_camera rejects it. The view codec reports CameraView::camera as a link, so SceneSet::replace
/// and SceneSet::unload repair it (see ComponentCodecs::add). Registration throws
/// `std::invalid_argument` when either type or key is already registered.
void add_camera_component_codecs(ComponentCodecs &codecs);
} // namespace anima
