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

  private:
    CameraSettings settings_;
};

/// Component that selects the Camera for view_matrix.
///
/// Any number of views and cameras may exist, but view_matrix needs exactly one active view. The
/// link addresses whichever Camera its object has when the view is resolved: removing that
/// component makes resolution fail until a Camera is attached again, while destroying the object or
/// unloading its scene breaks the link for good; nothing looks the object up by name or key. A null
/// link can be persisted but cannot produce a view.
struct CameraView {
    /// Null link; GameObject::add_component does not pass the owning object here.
    CameraView() = default;
    /// Object whose Camera is used.
    GameObject camera;
};

/// Resolves the one active CameraView in @p scene and returns its camera's view-projection matrix
/// for @p aspect (width / height).
///
/// Exactly one CameraView must be active, even when several point at the same camera, and its
/// object must be live in @p scene with an active Camera; other cameras need not be disabled. The
/// result is column-major in Vulkan clip space: framebuffer Y down, depth 0 at the near plane and 1
/// at the far plane. Call it on idle scenes after updates; it runs no hooks, advances no time,
/// keeps no reference and touches no renderer.
///
/// Throws `std::invalid_argument` for a missing, ambiguous, stale or foreign selection, a missing
/// or inactive Camera, invalid camera axes, an aspect that is not finite and positive, or a
/// matrix that is not finite and invertible in `float`; there is no fallback camera. Throws
/// `std::logic_error` while the scene is running component hooks or cleanup, constructing a
/// component, held by a scene driver or being destroyed.
[[nodiscard]] Mat4 view_matrix(Scene &scene, float aspect);
/// Resolves the view across every member of @p scenes as view_matrix(Scene &, float) does; the
/// camera may be in a different member than its view, and SceneSet::active plays no part. Also
/// throws `std::logic_error` during membership changes.
[[nodiscard]] Mat4 view_matrix(SceneSet &scenes, float aspect);

/// Registers the `anima.camera.v1` and `anima.camera-view.v1` codecs in @p codecs, both or neither.
///
/// A camera payload is a JSON object with exactly `projection` (`"perspective"` or
/// `"orthographic"`), `vertical_fov_degrees`, `orthographic_height`, `near_plane` and `far_plane`
/// (numbers). A view payload has exactly `camera`: the target's key from the operation's
/// ObjectReferences as a decimal string, `"0"` for null. Decoding throws `std::invalid_argument` for
/// payloads over 64 KiB or nested deeper than 16, duplicate, missing or unknown fields, and invalid
/// settings; malformed JSON throws the JSON parser's own exception. A view whose target lacks an
/// active Camera still loads; view_matrix rejects it. Registration throws `std::invalid_argument`
/// when either type or key is already registered.
void add_camera_component_codecs(ComponentCodecs &codecs);
} // namespace anima
