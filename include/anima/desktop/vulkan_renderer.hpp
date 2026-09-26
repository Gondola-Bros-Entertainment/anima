#pragma once
#include <anima/desktop/renderer_failure.hpp>

#include <anima/environment.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

/// @file
/// Desktop renderer for one SDL3 window over a private Vulkan 1.1 backend.
///
/// Part of the optional `anima::desktop` target (`ANIMA_BUILD_DESKTOP=ON`); no Vulkan type appears in the
/// public API. Scenes, environments and mesh preparation also need asset support (`ANIMA_BUILD_ASSETS=ON`).
/// Without it anima::VulkanRenderer draws only its clear color and the diagnostic triangle, with a UiContext's UI
/// composited over them; its set_scenes, prepare_meshes and prepare_mesh members throw `std::logic_error`, and
/// set_environment validates its argument, then throws `std::logic_error`.

struct SDL_Window;

namespace anima {

class Scene;
class Mesh;
class MeshPreparation;
class UiContext;
namespace detail {
struct UiFrame;
}

/// Construction options for VulkanRenderer.
struct RendererOptions {
    /// Enables `VK_LAYER_KHRONOS_validation` through `VK_EXT_debug_utils`; construction throws
    /// `std::runtime_error` when either is missing. Warnings and errors are printed to standard error and
    /// counted in RenderStats.
    bool validation = false;
    /// Initial capture path, handled as VulkanRenderer::request_capture does; empty requests none.
    std::filesystem::path capture;
    /// Failure injection for lifecycle tests. `instance`, `surface`, `device` and `resources` throw from
    /// construction, as do `texture` and `texture_upload` when the initial selection uploads a mesh;
    /// `swapchain` makes the first draw() that creates a swapchain throw RendererFatalError, as a real failure
    /// there does. Construction throws `std::invalid_argument` for any other stage, and for `texture` and
    /// `texture_upload` when #scenes is empty or asset support is off, since no initial upload can fire them.
    RendererFailureStage fail_after = RendererFailureStage::none;
    /// Retires presentation with a device wait-idle even where `VK_EXT_swapchain_maintenance1` present
    /// fences are available.
    bool disable_present_fences = false;
    /// Initial selection, under the rules of VulkanRenderer::set_scenes. Without asset support it must be
    /// empty, or construction throws `std::invalid_argument`.
    std::vector<std::shared_ptr<const Scene>> scenes;
    /// Draws a built-in triangle while no scene is selected, even without asset support. A selected scene
    /// with no instances shows only the background.
    bool diagnostic_triangle = false;
    /// Enables FrameProfile timings, with five GPU timestamp queries when the graphics queue supports
    /// them. When false there is no query pool and no clock is read.
    bool profile = false;
    /// Initial main-view culling state; see VulkanRenderer::set_frustum_culling.
    bool frustum_culling = true;
};

/// Failure injection for VulkanRenderer::set_scenes, for lifecycle tests.
struct SceneReplacementOptions {
    /// `none`, `vertex`, `index`, `texture`, `texture_upload`, `descriptors`, `palette`, `ready`,
    /// `upload_timeout` or `device_lost`; VulkanRenderer::set_scenes throws `std::invalid_argument` for any
    /// other stage.
    RendererFailureStage fail_after = RendererFailureStage::none;
};

/// Failure injection for VulkanRenderer::prepare_meshes and VulkanRenderer::prepare_mesh, for lifecycle tests.
struct ResourcePreparationOptions {
    /// As SceneReplacementOptions::fail_after, except that `palette` also throws `std::invalid_argument`.
    RendererFailureStage fail_after = RendererFailureStage::none;
};

/// A failure after which the renderer accepts only shutdown.
///
/// Thrown for device loss, surface loss, a frame, upload or presentation fence that does not signal within
/// 5 seconds, any other failed Vulkan call in draw() outside resource preparation, and any failure to build a
/// swapchain once draw() has released the previous one. Afterwards the members that VulkanRenderer lists throw
/// it again, while shutdown() and the destructor still release everything. There is no automatic recovery.
class RendererFatalError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
/// draw() could not prepare the selected scenes or the environment's shadow maps.
///
/// Thrown before an image is acquired, so no frame was submitted. The renderer stays usable and the next
/// draw() prepares again: repair the selected scenes or environment, or change the selection.
class SceneResourceError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Cumulative counters returned by VulkanRenderer::shutdown.
struct RenderStats {
    /// Frames presented. A frame whose presentation reports the swapchain out of date is not counted; a capture
    /// that could not be written does not change whether its frame is counted.
    std::uint64_t presented_frames{};
    /// Swapchains created, including recreations.
    std::uint32_t swapchain_generations{};
    /// Validation warnings; nonzero only with RendererOptions::validation.
    std::uint32_t validation_warnings{};
    /// Validation errors, plus failed waits during teardown.
    std::uint32_t validation_errors{};
    /// Whether the latest requested capture has been written.
    bool captured{};
    /// Captures written.
    std::uint32_t capture_count{};
    /// Accepted selections, counting the initial one from construction (even an empty one).
    std::uint32_t scene_generations{};
};

/// Timings of the latest draw(), in milliseconds.
///
/// The timing fields need RendererOptions::profile and are best read after a draw() that returned true,
/// since each draw() resets them. CPU fields are elapsed intervals, not CPU utilization. CPU and GPU
/// intervals overlap, so their sum is not frame latency.
struct FrameProfile {
    /// From draw() entry through the wait for the previous frame's fence, including swapchain recreation.
    double fence_wait_ms{};
    /// Resource preparation: culling, new mesh uploads and palette writes.
    double upload_ms{};
    /// Swapchain image acquisition.
    double acquire_ms{};
    /// Command recording and queue submission; driver calls may block here.
    double record_submit_ms{};
    /// The presentation call.
    double present_ms{};
    /// Palette bytes written for this frame, reported even without profiling; excludes UI geometry and push
    /// constants.
    std::uint64_t uploaded_bytes{};
    /// Whether the GPU fields are set. They time the previously submitted command buffer, read after its
    /// fence, and exclude presentation. False means unsupported or not yet available, not zero cost.
    bool gpu_available{};
    /// The whole command buffer.
    double gpu_ms{};
    /// Both shadow regions.
    double gpu_shadow_ms{};
    /// Sky and meshes into the scene target.
    double gpu_scene_ms{};
    /// Display conversion and UI.
    double gpu_resolve_ms{};
    /// Capture copy and the transition for presentation.
    double gpu_transfer_ms{};
};

/// Resource residency and draw counters.
///
/// Byte counts cover only the listed allocations, not staging, UI, swapchain or driver memory, so they are
/// not total device or process memory. Frame counters describe the latest preparation:
/// VulkanRenderer::set_scenes prepares without culling, and VulkanRenderer::draw with it.
struct ResourceStats {
    /// Live material samplers; images with identical sampling settings and mip level count share one.
    std::uint64_t resident_material_samplers{};
    /// Meshes uploaded since construction; cache hits are not counted.
    std::uint64_t mesh_uploads{};
    /// Vertex and index bytes uploaded since construction.
    std::uint64_t geometry_uploaded_bytes{};
    /// Device allocation bytes of cached vertex and index buffers.
    std::uint64_t resident_geometry_bytes{};
    /// Device allocation bytes of cached material images.
    std::uint64_t resident_texture_bytes{};
    /// Allocation bytes of the pose buffer, which holds every prepared instance's palette.
    std::uint64_t pose_buffer_bytes{};
    /// Palette bytes written by the latest preparation, including instances that only cast shadows.
    std::uint64_t pose_uploaded_bytes{};
    /// Main-view draw calls of the latest frame.
    std::uint64_t draw_calls{};
    /// Instances with at least one main-view draw.
    std::uint64_t instances{};
    /// Meshes in the GPU cache.
    std::uint64_t cached_assets{};
    /// Visible, active instances with at least one visible, nonempty draw.
    std::uint64_t candidate_instances{};
    /// Candidate instances left with no main-view draw by frustum culling.
    std::uint64_t culled_instances{};
    /// Visible, nonempty draws of candidate instances.
    std::uint64_t candidate_draws{};
    /// Candidate draws rejected by frustum culling.
    std::uint64_t culled_draws{};
    /// Indices submitted by main-view draws of the latest frame.
    std::uint64_t submitted_indices{};
    /// Draw calls of both shadow regions in the latest frame.
    std::uint64_t shadow_draw_calls{};
    /// Indices submitted to both shadow regions in the latest frame.
    std::uint64_t shadow_submitted_indices{};
    /// Device allocation bytes of both shadow depth images.
    std::uint64_t shadow_bytes{};
    /// Allocation bytes of the scene color and depth targets; excludes the swapchain and shadow images.
    std::uint64_t world_target_bytes{};
};

/// Renders selected scenes into one borrowed SDL window through a fixed pipeline.
///
/// Each frame renders the shadow regions, then the optional sky and the opaque and masked meshes into a linear
/// `RGBA16F` target, converts it for display and composites UI last. Custom shaders, material layouts and
/// render passes are not supported. The window must outlive the renderer, which never destroys it.
///
/// Use the renderer from the application's SDL video thread, with no concurrent calls. Scenes and settings
/// change only between draws, on that thread; a `const` Scene pointer does not synchronize access. One frame
/// is in flight, and palettes, descriptors and cached resources are replaced or destroyed only after its
/// fence. Transfers go through staging buffers, which are freed only after their upload completes.
///
/// The GPU mesh cache is keyed by Mesh object: each cached Mesh owns one vertex buffer, one index buffer and
/// its own material images, even when another Mesh has identical content. Once the previous frame finishes,
/// draw() releases every cached Mesh that only the renderer still references; a draw() that returns early
/// because the window is not drawable releases nothing. Selection, culling and visibility never evict, and
/// there is no size budget.
///
/// Materials render double-sided with glTF metallic-roughness shading: isotropic GGX, height-correlated Smith
/// visibility and Schlick Fresnel, perceptual roughness floored at `0.045` before squaring and `0.04`
/// reflectance for dielectrics. Base color is the base color texture, decoded from sRGB before filtering,
/// times the vertex color and material factor, clamped to [0, 1]. The metallic and roughness factors multiply
/// the blue and green channels of the metallic-roughness texture, occlusion (red) scales only ambient light,
/// and emission is added before fog. Textures use their Sampler filters and wrap modes, with mip chains built
/// on the CPU when mipmapped. Missing textures sample white, and a primitive's textures share one UV set.
/// Normal maps use the authored tangent frame, whose handedness survives skinning and mirrored transforms, or
/// else a screen-derivative frame; degenerate UVs keep the interpolated normal. Masked materials discard
/// fragments whose texture alpha times material alpha times vertex alpha is below the cutoff, in the color
/// and shadow passes alike. Unlit materials show their base color without lighting, shadows or emission, cast
/// no shadows and still receive fog and exposure. Skinned vertices blend up to four joint matrices on the GPU.
/// Normals use the inverse transpose of the vertex's matrix, and a singular matrix gives the normal +Y.
/// Triangles wound counterclockwise on screen face the viewer, and a back face shades with its normal
/// reversed. A matrix with a negative determinant, such as a scale of (-1, 1, 1), reverses the winding of the
/// triangles it places, as glTF specifies for mirrored nodes, so a mirrored draw shades as the mirror image of
/// its original; in a skinned triangle, the first vertex's blended matrix decides. Nothing is culled by facing,
/// so shadow casting does not depend on it.
///
/// Swapchains follow the window's pixel size with FIFO presentation. Recreation after a resize or an
/// out-of-date or suboptimal result waits for the device to go idle, so it can stall briefly. Display output is
/// sRGB-encoded once: by an sRGB swapchain format when the surface offers one, otherwise in the display shader
/// for 8-bit UNORM formats. Presentation semaphores belong to swapchain images. Where the instance and device
/// support `VK_EXT_swapchain_maintenance1`, presentation fences are waited before swapchain resources are
/// destroyed; otherwise, or with RendererOptions::disable_present_fences, a device wait-idle is used, which
/// unextended Vulkan does not guarantee to cover presentation. Portability enumeration and
/// `VK_KHR_portability_subset` are enabled when advertised, as on MoltenVK. Diagnostics are printed to
/// standard output.
///
/// After shutdown(), request_capture(), set_view(), set_frustum_culling(), set_environment(), set_scenes(),
/// prepare_meshes(), prepare_mesh() and draw() throw `std::logic_error`; after a RendererFatalError they throw
/// RendererFatalError.
class VulkanRenderer {
  public:
    /// Creates the Vulkan instance, surface and device for @p window, then selects RendererOptions::scenes.
    ///
    /// @p window must be live and created with `SDL_WINDOW_VULKAN`. Uses the first Vulkan 1.1 device that
    /// supports swapchains and can present to the window; the first draw() with a drawable window creates the
    /// swapchain. Throws `std::invalid_argument` for a RendererOptions::fail_after stage that the option says
    /// construction rejects, before anything else, and for a null @p window; what set_scenes() throws for the
    /// initial selection; InjectedRendererFailure for RendererOptions::fail_after; and `std::runtime_error` for
    /// other failures, including failed Vulkan calls. Completed stages are released before the exception
    /// propagates.
    VulkanRenderer(SDL_Window *window, RendererOptions options);
    /// Performs shutdown() if it has not run.
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    /// Makes the next draw() recreate the swapchain for the window's current pixel size.
    void request_resize() noexcept;
    /// Writes the next frame that draw() submits to @p path as a binary PPM, 8-bit RGB, creating missing
    /// parent directories; replaces any pending request. Throws `std::invalid_argument` for an empty path.
    ///
    /// The swapchain is recreated first if it cannot be copied from. A request that fails is consumed: one
    /// draw() throws `std::runtime_error` for it, RenderStats::captured stays false, and later draws neither
    /// retry nor report it. When the surface has no 8-bit BGRA or RGBA format usable as a copy source, that
    /// draw() keeps the current swapchain and presents nothing; when the file cannot be written, it has already
    /// submitted the frame and, unless presentation reported the swapchain out of date, presented and counted
    /// it.
    void request_capture(std::filesystem::path path);
    /// Sets the view used for shading, fog, the sky and culling from a column-major Vulkan view-projection
    /// (clip Y down, depth 0 to 1), such as anima::view_matrix returns.
    ///
    /// A perspective matrix supplies the eye position, an orthographic one the view direction. Until the first
    /// call the view-projection is all zeros. Throws anima::MathError (a `std::invalid_argument`) when
    /// @p view_projection is not finite or not invertible, and `std::invalid_argument` when its eye position
    /// exceeds the finite range, keeping the previous view. Without asset support only finiteness is checked.
    void set_view(const std::array<float, 16> &view_projection);
    /// Enables or disables main-view frustum culling, which starts as RendererOptions::frustum_culling.
    ///
    /// Culling tests each instance's bounds against the view, then the bounds of each visible part of the
    /// instances that pass. The test is conservative, so some invisible geometry is still drawn. It never
    /// changes scenes, visibility or the mesh cache. Shadow casters are always culled against their shadow
    /// regions instead, so casters outside the view still cast. Disable it for an unculled reference.
    void set_frustum_culling(bool enabled);
    /// Replaces the lighting environment from the next draw(); it starts as a default Environment.
    ///
    /// Validates @p environment with validate_environment() and both regions, enabled or not, with
    /// directional_shadow_matrix(). Each enabled region's DirectionalShadow::resolution must also fit the
    /// device's 2D image and framebuffer limits; a disabled region keeps a 1x1 map, so its resolution meets that
    /// check only in a call that enables it. Invalid input throws `std::invalid_argument` or anima::MathError
    /// and keeps the previous environment. The next draw() allocates changed shadow maps and throws
    /// SceneResourceError if that fails.
    void set_environment(const Environment &environment);
    /// Selects the scenes to draw, or clears the selection with an empty list; the renderer keeps the pointers.
    /// It never follows SceneSet::active(); SceneSet::render_scenes() lists a set's scenes.
    ///
    /// Throws `std::invalid_argument` for a null or repeated scene before any work. Then waits for the frame in
    /// flight and uploads the meshes of every visible, active instance, whatever the view. If that fails, the
    /// previous selection stays and meshes uploaded so far stay cached. It throws `std::invalid_argument` for a
    /// mesh beyond device limits (more vertices than the indexed-draw range, or a texture larger than the 2D
    /// image limit), `std::length_error` when the palettes exceed the storage-buffer range, `std::runtime_error`
    /// for other failures, including failed Vulkan calls such as allocations, InjectedRendererFailure as
    /// SceneReplacementOptions::fail_after requests, and RendererFatalError for device loss or a fence timeout.
    /// The swapchain is untouched, so this works while the window is minimized.
    ///
    /// Selected scenes remain the caller's to change between draws; each draw() prepares their current content.
    /// A scene that its SceneSet unloads or replaces stays selected but is empty.
    void set_scenes(std::vector<std::shared_ptr<const Scene>> scenes, SceneReplacementOptions options = {});
    /// Uploads @p assets into the mesh cache without selecting or drawing them, for example while loading.
    /// Cached or repeated meshes are reused.
    ///
    /// Throws `std::invalid_argument` for a null pointer anywhere in @p assets before any upload; an empty span
    /// does nothing. Waits for the frame in flight and leaves the selection, view and poses unchanged. Not
    /// atomic: after a failure, meshes uploaded earlier stay cached while they have other owners. Throws
    /// `std::invalid_argument` for a mesh beyond device limits, `std::runtime_error` for other upload failures,
    /// including failed Vulkan calls, InjectedRendererFailure as ResourcePreparationOptions::fail_after
    /// requests, and RendererFatalError for device loss or a fence timeout.
    void prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets, ResourcePreparationOptions options = {});
    /// Uploads MeshPreparation::asset() as prepare_meshes() does, using the preparation's mip chains instead of
    /// computing them here. Allocation and upload still run synchronously on this thread. @p preparation is
    /// read only during the call, and not at all if the mesh is already cached.
    void prepare_mesh(const MeshPreparation &preparation, ResourcePreparationOptions options = {});
    /// Current cache and allocation sizes with the latest frame counters; all zero without asset support.
    [[nodiscard]] ResourceStats resource_stats() const noexcept;
    /// Prepares, records, submits and presents one frame; does not advance simulation or animation.
    ///
    /// Returns false while the window is hidden, minimized or zero-sized, when no image is acquired within
    /// 100 ms, and when the swapchain is out of date, which the next call recreates; keep running the event
    /// loop and call again. Returns true when the frame was presented.
    ///
    /// Each call waits for the previous frame, releases unowned cache entries, culls, uploads meshes that
    /// became visible or cast shadows (prepare_meshes() can upload them earlier) and writes every prepared
    /// instance's palette. The palettes of one frame must fit the device's storage-buffer range. Throws
    /// SceneResourceError when that preparation fails recoverably; RendererFatalError for device or surface
    /// loss, a fence timeout, any other Vulkan failure, or any failure to build a new swapchain once the
    /// previous one is released; and `std::runtime_error` for other failures, such as a surface that offers no
    /// usable format, which leaves the current swapchain in place, or a capture request that fails, which only
    /// that call reports (see request_capture()).
    [[nodiscard]] bool draw();
    /// Timings of the latest draw(); see FrameProfile.
    [[nodiscard]] FrameProfile frame_profile() const noexcept;
    /// Waits for the device and presentation to finish, destroys every Vulkan object and returns the final
    /// counters, including failures during this cleanup. Idempotent. The waits have no timeout, so a hung
    /// driver blocks here and in the destructor.
    [[nodiscard]] RenderStats shutdown();

  private:
    friend class UiContext;
    // Whether the surface offers the sRGB format that UI blending needs; UiContext checks it at construction.
    [[nodiscard]] bool srgb_presentation();
    [[nodiscard]] bool draw_ui(const detail::UiFrame &frame);
    // RenderStats::presented_frames so far, from which UiContext counts the frames each render presents.
    [[nodiscard]] std::uint64_t presented_frames() const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anima
