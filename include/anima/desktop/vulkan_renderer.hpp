#pragma once
#include <anima/desktop/renderer_failure.hpp>

#include <anima/environment.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
/// set_environment and set_time validate their arguments, then throw `std::logic_error`.

struct SDL_Window;

namespace anima {

class Scene;
class Mesh;
class MeshPreparation;
class UiContext;
namespace detail {
struct UiFrame;
}

/// How the display takes the frames that VulkanRenderer::draw() presents, as Vulkan's present modes of the same names
/// define it. A surface supports PresentMode::fifo always and the others only where its driver offers them; see
/// VulkanRenderer::set_present_mode.
enum class PresentMode {
    /// Frames wait in a queue and the display takes one at each vertical blank, so no frame tears and the display shows
    /// at most one frame per refresh.
    fifo,
    /// A frame replaces the displayed image at once, without waiting for a vertical blank, so frames can tear.
    immediate,
    /// A frame waits for the next vertical blank in a queue of one, replacing the frame that waits there, which is
    /// never displayed, so no frame tears and frames can be drawn faster than the display refreshes.
    mailbox,
    /// As fifo, except that a frame that arrives after a vertical blank at which the queue was empty replaces the
    /// displayed image at once, so a late frame can tear.
    fifo_relaxed,
};

/// Construction options for VulkanRenderer.
struct RendererOptions {
    /// Enables `VK_LAYER_KHRONOS_validation` through `VK_EXT_debug_utils`; construction throws
    /// `std::runtime_error` when either is missing. Warnings and errors are printed to standard error and
    /// counted in RenderStats.
    bool validation = false;
    /// Initial capture path, handled as VulkanRenderer::request_capture(std::filesystem::path) does; empty
    /// requests none.
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
    /// Enables FrameProfile timings, with six GPU timestamp queries when the graphics queue supports
    /// them, calibrated timestamps where the device also offers them (see VulkanRenderer::measures_gpu_idle), and
    /// `VK_GOOGLE_display_timing` where the device offers it (see VulkanRenderer::measures_present_interval). With
    /// that extension every present carries an id for its display time, and each draw() that prepares a frame reads
    /// the display times that arrived since the previous read with `vkGetPastPresentationTimingGOOGLE`, in
    /// FrameProfile::prepare_ms; an out-of-date result of that read makes the next draw() recreate the swapchain, and
    /// any other failure, such as surface loss, throws RendererFatalError as draw() describes. When false there is no
    /// query pool, no clock is read and no display time is read.
    bool profile = false;
    /// Initial main-view culling state; see VulkanRenderer::set_frustum_culling.
    bool frustum_culling = true;
    /// Uploads ImageFormat::bc7 images decoded to RGBA8, as on a device that cannot sample BC7, even where the
    /// device can; see VulkanRenderer::samples_bc7.
    bool decode_bc7 = false;
    /// Most samples that a material texture's filter takes along the direction in which a pixel's footprint on the
    /// texture is longest, as Vulkan's `maxAnisotropy`; 1 filters isotropically, choosing a mip level by the longest
    /// axis alone, which blurs surfaces seen at a glancing angle. Must be finite and at least 1, or construction
    /// throws `std::invalid_argument`. The renderer uses at most the device's limit; see
    /// VulkanRenderer::max_anisotropy.
    float max_anisotropy = 16;
    /// Initial level of detail threshold, in pixels; see VulkanRenderer::set_lod_threshold. Must be finite and
    /// nonnegative, or construction throws `std::invalid_argument`.
    float lod_threshold = 1;
    /// Frames that draw() may submit before the GPU finishes the earliest of them: 1 or 2, or construction throws
    /// `std::invalid_argument`. With 2, draw() prepares and records a frame while the GPU still runs the previous one,
    /// so that the CPU's work overlaps the GPU's, at the cost of a second copy of the per-frame buffers (palettes, the
    /// environment and custom material frame blocks, UI vertices) and of up to a frame more between the inputs that a
    /// frame shows and its display. With 1, each draw() waits for the previous frame before preparing the next. Where
    /// VulkanRenderer::waits_for_presents() is true, each draw() also waits for the present of the frame submitted one
    /// submission before the frame that it waits for, as VulkanRenderer::wait_for_frame() describes, which on a driver
    /// that reports presents at display bounds the presented frames that wait for display ahead of the next at this
    /// count. Either way, a draw() that uploads, as when a mesh first becomes visible, waits for the frames already in
    /// flight, since the fence that an upload waits for signals only after every earlier submission.
    std::uint32_t frames_in_flight = 2;
    /// Initial present mode request, under the rules of VulkanRenderer::set_present_mode. A value that is not a
    /// PresentMode enumerator makes construction throw `std::invalid_argument`.
    PresentMode present_mode = PresentMode::fifo;
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

/// No installed Vulkan driver can present to the window, so VulkanRenderer cannot run on this system.
///
/// VulkanRenderer's constructor throws it when the drivers lack a surface extension that the window system
/// needs, when the Vulkan loader reports no compatible driver, and when no device meets the requirements that
/// the constructor lists. Other construction failures, such as a missing validation layer, throw
/// `std::runtime_error`.
class RendererUnavailableError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
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

/// One frame read back into memory by VulkanRenderer::request_capture().
struct CapturedImage {
    /// Width in pixels, the swapchain's at that frame.
    std::uint32_t width{};
    /// Height in pixels.
    std::uint32_t height{};
    /// `3 * width * height` bytes: rows from top to bottom, each pixel's red, green and blue as displayed,
    /// sRGB-encoded, with no row padding.
    std::vector<std::uint8_t> rgb;
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
    /// Whether the latest capture request has completed: its file written, or its image read back into memory.
    bool captured{};
    /// Capture requests completed, to files and into memory.
    std::uint32_t capture_count{};
    /// Accepted selections, counting the initial one from construction (even an empty one).
    std::uint32_t scene_generations{};
    /// Waits for a present that VulkanRenderer::wait_for_frame() and VulkanRenderer::draw() began, one for each
    /// present that they wait for as wait_for_frame() describes; zero unless VulkanRenderer::waits_for_presents().
    std::uint64_t present_waits{};
    /// Waits counted in present_waits that ended at their 100 ms limit before the driver reported the present.
    std::uint64_t present_wait_timeouts{};
};

/// Timings of the latest draw(), in milliseconds.
///
/// The timing fields need RendererOptions::profile and are best read after a draw() that returned true,
/// since each draw() resets them. CPU fields are elapsed intervals, not CPU utilization. They follow one another
/// from draw() entry through the presentation call, so their sum is the call's duration, except that a draw() that
/// captures then waits for its frame and reads the image back, which no field times; a draw() that returns early
/// sets only the fields it reached. When fence_wait_ms ends, the GPU has finished the frame submitted
/// RendererOptions::frames_in_flight submissions before the one that the draw() submits, and every frame before that.
/// With one frame in flight that is the previous frame, so no later CPU field overlaps GPU frame work; with two, the
/// previous frame can still run on the GPU during every later field. Where VulkanRenderer::wait_for_frame() waits for
/// presents, the wait for the present of the frame submitted one submission before the finished one has ended too, as
/// it describes. The frame submitted at the end of record_submit_ms can run on the GPU during present_ms and after
/// draw() returns.
struct FrameProfile {
    /// From draw() entry through the wait for the fence of the frame submitted RendererOptions::frames_in_flight
    /// submissions earlier and, where VulkanRenderer::wait_for_frame() waits for presents, for the present of the frame
    /// submitted one submission before that, including swapchain recreation. After VulkanRenderer::wait_for_frame()
    /// those waits are done, so they return at once.
    double fence_wait_ms{};
    /// Frame preparation after that wait: destroying what earlier calls released once no frame in flight can use it,
    /// releasing unowned cache entries, which destroys them at once unless a frame that may use them is still in
    /// flight and otherwise in a later call, once that frame has finished, sizing the shadow maps and fitting their
    /// cascades to the view, writing the custom materials' frame block, reading the GPU timestamps of the frame that
    /// fence_wait_ms waited for, reading the display times that set present_interval_ms, and for a UiContext frame,
    /// copying its vertices, creating the UI pipeline when first needed and uploading each new UI texture, which waits
    /// for the GPU.
    double prepare_ms{};
    /// Scene preparation: culling, uploads of meshes that became visible or cast shadows and of their custom materials
    /// and placements, palette writes, sorting blended draws back to front, creating the opaque depth and color copies
    /// that custom materials read (on the first frame that needs them after each swapchain creation), and writing the
    /// frame's shadow cascades and environment.
    double upload_ms{};
    /// Swapchain image acquisition.
    double acquire_ms{};
    /// Command recording and queue submission; driver calls may block here, as MoltenVK's submission does until Core
    /// Animation frees a drawable for the frame, which in PresentMode::fifo paces frames to the display.
    double record_submit_ms{};
    /// The presentation call.
    double present_ms{};
    /// Palette bytes written for this frame, reported even without profiling; excludes UI geometry and push
    /// constants.
    std::uint64_t uploaded_bytes{};
    /// Whether the GPU fields are set. They time the frame that fence_wait_ms waited for, read after its fence, and
    /// exclude presentation. A frame's first timestamp is written once every command submitted before it has finished,
    /// so with two frames in flight they leave out the time that the GPU spent finishing the frame before it. False
    /// means unsupported or not yet available, not zero cost.
    bool gpu_available{};
    /// The whole command buffer.
    double gpu_ms{};
    /// The atmosphere's compute dispatches, which write its tables while it is enabled. The first frame drawn with it
    /// enabled, and each frame whose Atmosphere differs from the one the tables were last built for in a field other
    /// than `enabled`, `ground_height`, `mie_anisotropy` and `sun_angular_radius`, rebuilds the transmittance and
    /// multiple scattering tables and redraws the sky view table. Any other frame redraws only the sky view table, when
    /// the altitude that the sky is seen from (EnvironmentSettings::atmosphere), the sun's elevation or
    /// `mie_anisotropy` differs from that table's last write, and otherwise dispatches nothing.
    double gpu_atmosphere_ms{};
    /// Every shadow pass: each shadow cascade and the detail region.
    double gpu_shadow_ms{};
    /// Sky and meshes into the scene target, with the copy of opaque inputs on frames that make one.
    double gpu_scene_ms{};
    /// Display conversion and UI.
    double gpu_resolve_ms{};
    /// Capture copy and the transition for presentation.
    double gpu_transfer_ms{};
    /// From the last timestamp of the frame submitted before the one the GPU fields time to that frame's first
    /// timestamp: how long the graphics queue went without frame work between them. With one frame in flight the timed
    /// frame was submitted after the earlier one had finished, so this spans at least the prepare_ms, upload_ms and
    /// acquire_ms of the draw() that submitted it, and uploads that run in that gap count toward it. With two, the
    /// timed frame can be queued before the earlier one finishes; its first timestamp still follows the earlier frame's
    /// work, but Vulkan does not order it after that frame's last timestamp, and where it is the lower the field is 0.
    /// Set with the GPU fields where VulkanRenderer::measures_gpu_idle() is true and the frame submitted before the
    /// timed one was timed too, unless the device's timestamp counter may have wrapped in between: the field stays
    /// empty when, on the host's steady clock, the timed frame's fence wait in this draw() ended at least a quarter of
    /// the counter's wrap period (2^timestampValidBits ticks of timestampPeriod nanoseconds) after the earlier frame's
    /// submission. Empty otherwise too, as on the first timed frame.
    std::optional<double> gpu_idle_ms;
    /// The time between the displays of two frames presented one after the other: the difference between the display
    /// times (`actualPresentTime`) that `VK_GOOGLE_display_timing` reports for them. Display times arrive some time
    /// after their presents, so the frames were presented by earlier calls. Each draw() reads the display times that
    /// arrived since the previous one and sets the field from the latest frame among them whose predecessor has a known
    /// display time, from this read or an earlier one, that is no later than the frame's own. Both frames must have
    /// been presented to the current swapchain, and a display time of zero, which Vulkan does not define, counts as
    /// unknown. Vulkan defines a display time as when the frame was displayed, so in PresentMode::fifo the difference
    /// is normally the display's refresh interval, or a multiple of it after a frame missed a vertical blank. The times
    /// are the driver's, though: MoltenVK 1.4.1 takes them from Core Animation and, for a frame that Core Animation
    /// reports no display time for, as one it did not display, uses the time at which it learns that, so its intervals
    /// then follow the presents rather than the display. Set where VulkanRenderer::measures_present_interval() is true;
    /// empty otherwise, as when no display time arrived since the previous draw().
    std::optional<double> present_interval_ms;
};

/// Resource residency and draw counters.
///
/// Byte counts cover only the listed allocations, not staging, UI, swapchain or driver memory, so they are
/// not total device or process memory. Frame counters describe the latest preparation:
/// VulkanRenderer::set_scenes prepares without culling, and VulkanRenderer::draw with it.
struct ResourceStats {
    /// Live material samplers, those of released meshes included until no frame in flight can use them; images with
    /// identical sampling settings and mip level count share one.
    std::uint64_t resident_material_samplers{};
    /// Meshes uploaded since construction; cache hits are not counted.
    std::uint64_t mesh_uploads{};
    /// Vertex and index bytes uploaded since construction.
    std::uint64_t geometry_uploaded_bytes{};
    /// Device allocation bytes of cached vertex and index buffers.
    std::uint64_t resident_geometry_bytes{};
    /// Device allocation bytes of cached material images, custom materials' included.
    std::uint64_t resident_texture_bytes{};
    /// Device allocation bytes of cached placements (MeshPlacements), 48 bytes per placement before alignment.
    std::uint64_t resident_placement_bytes{};
    /// Custom materials in the GPU cache.
    std::uint64_t cached_custom_materials{};
    /// Whether the latest preparation draws a custom material that reads opaque depth or color, so that its frame
    /// copies both after the opaque draws.
    bool opaque_inputs{};
    /// Allocation bytes of the opaque depth and color copies, which exist from the first frame that copies them
    /// until the swapchain is recreated.
    std::uint64_t opaque_input_bytes{};
    /// Allocation bytes of the pose buffers, one for each frame in flight (RendererOptions::frames_in_flight), each of
    /// which holds every instance palette of the frames that it serves.
    std::uint64_t pose_buffer_bytes{};
    /// Palette bytes written by the latest preparation, including instances that only cast shadows.
    std::uint64_t pose_uploaded_bytes{};
    /// Main-view draw calls of the latest frame. A draw of an object with placements takes one call per run of
    /// adjacent visible placement clusters that draw the same level of detail and, for an opaque or masked material,
    /// through the same pipeline (#discarding_draw_calls).
    std::uint64_t draw_calls{};
    /// Copies drawn by main-view draw calls of the latest frame: one per call of an object without placements, and
    /// one per placement that a call draws.
    std::uint64_t drawn_copies{};
    /// Placement clusters (MeshPlacements::clusters()) of objects with a main-view draw that main-view frustum culling
    /// skipped in the latest frame, counted once per object whatever its materials; an object culled whole counts
    /// in #culled_instances instead.
    std::uint64_t culled_clusters{};
    /// Objects without placements, and placement clusters of objects with a main-view draw, that their visibility
    /// ranges (VisibilityRange) hid in the latest frame.
    std::uint64_t range_culled{};
    /// Main-view draw calls of the latest frame that drew a simplified level (DrawLevel) instead of the full draw.
    std::uint64_t lod_draws{};
    /// Main-view draw calls of the latest frame that drew a mesh other than an impostor with an opaque or masked
    /// Material through the pipeline whose fragment shader may discard: those of masked materials, of objects and
    /// placement clusters that may lie in a margin of their visibility range, and of single-sided materials whose
    /// facing the rasterizer cannot decide, as VulkanRenderer describes. The other draw calls of such meshes never
    /// discard, so a GPU that removes hidden surfaces before shading them, as Apple GPUs do, can treat them as opaque.
    std::uint64_t discarding_draw_calls{};
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
    /// Indices submitted by main-view draws of the latest frame, once per copy drawn.
    std::uint64_t submitted_indices{};
    /// Draw calls of every shadow pass in the latest frame, each cascade's and the detail region's, so a caster that
    /// several cascades hold counts in each.
    std::uint64_t shadow_draw_calls{};
    /// Indices submitted to every shadow pass in the latest frame, once per copy drawn.
    std::uint64_t shadow_submitted_indices{};
    /// Device allocation bytes of the shadow depth images: the cascades' layers and the detail region's.
    std::uint64_t shadow_bytes{};
    /// Device allocation bytes of the atmosphere's transmittance, multiple scattering and sky view tables, allocated
    /// with the renderer whether the atmosphere is enabled or not.
    std::uint64_t atmosphere_bytes{};
    /// Allocation bytes of the scene color and depth targets; excludes the swapchain and shadow images.
    std::uint64_t world_target_bytes{};
};

/// Renders selected scenes into one borrowed SDL window.
///
/// Each frame renders the sun's shadow maps, then into a linear `RGBA16F` target the opaque and masked meshes, the
/// optional sky, which shades only the pixels that they leave at the far plane, and the blended meshes; it converts
/// the target for display and composites UI last. The window must outlive the renderer, which never destroys it.
///
/// The sun's shadow maps are its cascades (ShadowCascades), which each frame fits to the current view as
/// fit_shadow_cascades() does and extends toward the sun over the casters that each one's square reaches, as layers of
/// one depth image, and the optional detail region (DirectionalShadow), each the first of `VK_FORMAT_D32_SFLOAT` and
/// `VK_FORMAT_D16_UNORM` that the device can attach and sample; with 16-bit depth, each cascade's bias also covers a
/// step of its depth. Each pass draws the opaque and masked casters that it holds, culled against itself.
///
/// The view's depth buffer is the first of `VK_FORMAT_D32_SFLOAT`, `VK_FORMAT_X8_D24_UNORM_PACK32` and
/// `VK_FORMAT_D16_UNORM` that the device can attach, sample and copy, as custom materials that read opaque depth
/// require; Vulkan guarantees all three uses for `VK_FORMAT_D16_UNORM`. Reversed depth keeps distant surfaces precise
/// only in the floating-point format, whose values crowd toward 0 as the depths of distant surfaces do; the fixed-point
/// formats space their values evenly, which leaves distant surfaces the precision of forward depth.
///
/// Applications customize shading, not the frame. A CustomMaterial supplies SPIR-V vertex and fragment shaders,
/// an optional depth-only variant, a parameter block, textures and a blend mode for the mesh material slots it
/// is assigned to (Scene::set_custom_material), and its shaders read the inputs that custom_material.hpp
/// documents. The renderer creates its pipelines and keeps the passes and their order, depth testing, sorting,
/// the vertex, descriptor and push constant layouts, the display conversion and every Vulkan object.
/// Applications cannot add render passes, render targets, post-processing or compute work, cannot reach Vulkan
/// objects, and cannot change the standard material's shading.
///
/// Use the renderer from the application's SDL video thread, with no concurrent calls. Scenes and settings
/// change only between draws, on that thread; a `const` Scene pointer does not synchronize access. Up to
/// RendererOptions::frames_in_flight frames are in flight. Each has its own command buffer, palettes, environment and
/// custom material frame blocks, UI vertices and descriptor sets, which draw() rewrites only once the frame that used
/// them last has finished. Frames share the shadow maps, the atmosphere's tables, the scene targets and the opaque
/// input copies, which their commands order on the GPU, and cached resources, UI images and replaced shadow maps are
/// destroyed only once every frame that may use them has finished. Transfers go through staging buffers, which are
/// freed only after their upload completes.
///
/// Uploads read texels through Mesh::texel_images() and CustomMaterial::texel_images(), and once an upload has
/// completed they call release_texels() on the Mesh or CustomMaterial, so that one compiled with
/// TexelRetention::until_upload lets its texels go: the GPU cache keeps only device images. Uploading such a Mesh or
/// material again, in another renderer or in one created after a RendererFatalError, succeeds only while something
/// else still holds its source images.
///
/// The GPU mesh cache is keyed by Mesh object: each cached Mesh owns one vertex buffer, one index buffer and
/// its own material images, even when another Mesh has identical content. Buffers and images are suballocated
/// from larger device memory blocks, except that a large resource, or one the driver asks to place alone, gets
/// its own allocation. The device's `maxMemoryAllocationCount`, which Vulkan allows to be as low as 4096,
/// therefore limits only resources placed alone, not how many Meshes can be cached. Once the frame that it waits for
/// has finished, draw() releases every cached Mesh that only the renderer still references, and destroys the Mesh's GPU
/// resources once no frame in flight can draw them; a draw() that returns early because the window is not drawable
/// releases nothing. Selection, culling and visibility never evict, and there is no size budget.
///
/// An object with placements (Scene::set_placements) draws each of its mesh's draws as instances of one indexed draw,
/// one call per run of adjacent placement clusters (MeshPlacements::clusters()) that the pass can see and that draw the
/// same level of detail: the main view culls clusters by their world bounds when frustum culling is on, and each
/// shadow pass culls them against itself, so copies outside the view still cast shadows. Each MeshPlacements uploads
/// its transforms once into a device buffer, cached per object and released as meshes are, and the vertex shaders
/// compose each placement with the object's world matrix and the mesh's rest pose.
///
/// An object with a visibility range (Scene::set_visibility_range) draws only at the distances the range allows from
/// the eye of the current view, in the shadow passes too, measured to the center of its mesh's rest bounds
/// (Mesh::rest_bounds()) as the object, or each copy, places it. Objects, and placement clusters, entirely outside it
/// are culled in every pass. In the range's margins the standard material discards the share of a copy's pixels that
/// a 4x4 ordered dither gives, keeping in a begin margin the pixels complementary to those an end margin keeps, so it
/// fades without blending or sorting, and a copy casts shadows while more than half of it draws; its vertex shaders
/// hide the copies outside the range in a cluster that is not culled. Custom materials fade and hide copies only
/// through animaVisibility() and animaDissolved(), as custom_material.hpp describes.
///
/// A draw with levels of detail (IndexedDraw::levels) draws, for each object or placement cluster, the coarsest level
/// whose error stays within set_lod_threshold() pixels on screen; its index buffer holds every level, uploaded with
/// the mesh, and choosing one costs no upload or allocation.
///
/// A Mesh compiled with Mesh::compile_impostor() draws as an impostor (impostor.hpp): each object or placed copy is one
/// quad, which faces the eye across the front of the sphere of its ImpostorFrames, covering the sphere's silhouette,
/// computed in the mesh's space, so that any affine world matrix or placement keeps it exact. Each pixel blends the
/// three frames whose directions surround the direction toward the eye, weighted by their barycentric position on the
/// grid: in each, where the pixel's ray crosses the frame's plane, the ray steps once to the height stored there, as
/// parallax mapping steps, and samples the frame there, from the mip level its footprint chooses, but none coarser than
/// four texels across a frame or than one whose texels each lie within a single frame, half a texel of the coarser
/// level it reads inside the frame's edges. The blended coverage is tested against the material's cutoff, the pixel's
/// depth is that of the blended surface point, or the quad's where the parallax step carries that point in front of the
/// quad, and it is lit as the standard material lights a surface, with the blended normal, base color times the
/// object's material factor, occlusion, roughness, metallic and emission, and shadows received without a receiver
/// plane. Into the shadow maps the impostor draws along the sun with the frames nearest the sun's direction. A copy
/// seen from inside its sphere draws nothing, and visibility ranges apply as to any copy. A custom material assigned to
/// its material slot draws it as a quad instead.
///
/// Materials render with glTF metallic-roughness shading: isotropic GGX, height-correlated Smith
/// visibility and Schlick Fresnel, perceptual roughness floored at `0.045` before squaring and `0.04`
/// reflectance for dielectrics. Base color is the base color texture, decoded from sRGB before filtering,
/// times the vertex color and material factor, clamped to [0, 1]. The metallic and roughness factors multiply
/// the blue and green channels of the metallic-roughness texture, occlusion (red) scales only ambient light,
/// and emission is added before fog. Textures use their Sampler filters and wrap modes, and a texture whose
/// magnification and minification filters are linear and that samples a mip chain also filters anisotropically, up
/// to max_anisotropy() samples; nearest and unmipmapped textures never do. ImageFormat::rgba8 images
/// get mip chains built on the CPU when mipmapped. ImageFormat::bc7 images upload the levels they store, or only
/// the base level for an unmipmapped texture, and a single stored level samples as an unmipmapped texture does: as
/// `VK_FORMAT_BC7_SRGB_BLOCK` or `VK_FORMAT_BC7_UNORM_BLOCK` by the texture's encoding where samples_bc7() is true,
/// and otherwise decoded to RGBA8 on the CPU as decode_image() decodes them, which takes four times the device
/// memory. Missing textures sample white, and a primitive's textures share one UV set.
/// Normal maps use the authored tangent frame, whose handedness survives skinning and mirrored transforms, or
/// else a screen-derivative frame; degenerate UVs keep the interpolated normal. Masked materials discard
/// fragments whose texture alpha times material alpha times vertex alpha is below the cutoff, in the color
/// and shadow passes alike. Unlit materials show their base color without lighting, shadows or emission, cast
/// no shadows and still receive fog and exposure. Skinned vertices blend up to four joint matrices on the GPU.
/// Normals transform as normal() transforms them: in the inverse transpose's direction, and for a matrix that
/// collapses an axis, as the flattened surface's normal.
/// Triangles wound counterclockwise on screen face the viewer, and a back face shades with its normal
/// reversed. A matrix with a negative determinant, such as a scale of (-1, 1, 1), reverses the winding of the
/// triangles it places, as glTF specifies for mirrored nodes, so a mirrored draw shades as the mirror image of
/// its original; in a skinned triangle, the first vertex's blended matrix decides. A single-sided material
/// (Material::double_sided false) draws no face turned away from the viewer, and shadows are cast from both sides.
/// The rasterizer culls those faces in a draw of an opaque material outside the margins of its visibility range whose
/// placing matrices each have a determinant with a magnitude above `1e-4` of the product of the lengths of its first
/// three columns: the back faces, or under an odd number of negative determinants the front faces. The placing matrices
/// are the node's world matrix for an object without placements, and for the copies of a placement cluster the
/// object's world matrix, the node's rest matrix and each copy's placement (MeshPlacements), where the placements'
/// determinants must also share one sign. Otherwise, as in every skinned draw, the fragment shader discards those
/// faces. Opaque and masked meshes other than impostors draw through pipelines whose fragment shader cannot discard,
/// so that a GPU that removes hidden surfaces before shading them, as Apple GPUs do, can treat their fragments as
/// opaque, except masked materials, objects and placement clusters that may lie in a margin of their visibility range,
/// and single-sided draws that the rasterizer does not cull (ResourceStats::discarding_draw_calls).
///
/// Blended materials (AlphaMode::blend) draw in the same pass, after every opaque and masked draw of every
/// selected scene. They test depth with `GREATER`, since the view's depth is reversed, and write none, so nearer opaque
/// and masked surfaces hide them and they hide nothing. Their alpha is the product that masking tests. The shader
/// shades the straight (unpremultiplied) albedo as it shades an opaque material, fog included, which applies at the
/// surface's own distance, then writes its color times alpha with that alpha, blended as `ONE, ONE_MINUS_SRC_ALPHA`:
/// the glTF "over" operator, applied to the whole shaded color, emission included, in linear light. Since the fog's
/// light depends only on a path's direction (EnvironmentSettings::fog_density), fogging each surface before blending
/// equals fogging the path from the eye through it to what lies behind.
/// Exposure scales the composite, so it commutes with blending; tone mapping, which is not linear, applies to the
/// composite. Texels stay straight alpha: mip chains of blended base-color maps weight color by alpha
/// (TextureMipOptions::alpha_weighted_color), but filtering within a level interpolates straight color, so the
/// color of transparent texels still reaches the edges between them and opaque ones. Blended materials cast no
/// shadows, since shadow maps hold depth only; lit ones receive shadows.
///
/// Each frame the blended draws of all selected scenes, including those of blended and additive custom materials,
/// sort together, back to front, by one key per draw of each
/// instance: the squared distance from the eye to the center of the draw's world bounds
/// (Scene::Instance::primitive_bounds) in a perspective view, or that center's distance along the view direction
/// in an orthographic one. Draws with equal keys keep selection, instance and draw order, so their order does not
/// flicker. Sorting whole draws has limits:
/// - Triangles within a draw blend in index order, so where visible triangles of one draw overlap on screen, as
///   the near and far sides of a closed double-sided mesh do, a farther triangle can cover a nearer one.
/// - Where two draws intersect, the order is correct on only one side of the intersection.
/// - Only bounds centers are compared, so a large draw can sort wrongly against a small one near it, such as a
///   water plane and an object floating on it.
/// - Mesh::compile_static can split one primitive into several draws, which sort separately.
/// - The copies of an object with placements sort as one draw, by the bounds of every copy, and blend in
///   MeshPlacements::transforms() order.
///
/// A custom material draws in the pass that its CustomBlend mode selects. An opaque one draws in the world pass
/// after every opaque and masked mesh and before the sky, testing depth with `GREATER` and writing it, so it hides
/// and is hidden as they are. Blended and additive ones are blended draws: they sort with blended materials and draw
/// in that order, testing depth with `GREATER` and writing none. When a frame draws a blended or additive custom
/// material whose fragment shader reads opaque depth or color (CustomMaterial::reads_opaque_depth,
/// CustomMaterial::reads_opaque_color), the world pass ends after the opaque draws, the renderer copies the depth
/// and color targets into images that those shaders sample, and a second pass loads the targets and draws the
/// blended draws; other frames copy nothing. The copies are allocated on the first frame that needs them and
/// released with the swapchain. A custom material casts shadows only through its depth-only variant, which draws
/// into each shadow pass, each cascade and the detail region, with that pass's view-projection; without one it casts
/// none. Custom shaders read no shadow maps, and fog is theirs to apply, through `animaFogged()` or from the
/// documented inputs, while exposure and tone mapping apply to the whole target at display conversion. A
/// CustomMaterial's shader modules, pipelines, parameter
/// buffer, textures and descriptors are created the first time a preparation draws it and are released as cached
/// meshes are, once only the renderer references the material; creating them fails as a mesh upload does.
///
/// Swapchains follow the window's pixel size and present in the mode that set_present_mode() requests where the surface
/// offers it, otherwise in PresentMode::fifo. Each swapchain is created with a `minImageCount` of one more than the
/// fewest images that the surface allows in every mode and, where the instance supports `VK_EXT_surface_maintenance1`,
/// in the swapchain's mode, but no more than the most that it allows in them; Vulkan lets the driver create more
/// images than that. Recreation after a resize, a change of present mode or an out-of-date or suboptimal result waits
/// for the device to go idle, so it can stall briefly. Where the device supports `VK_KHR_present_id` and
/// `VK_KHR_present_wait` (waits_for_presents()), presents in the FIFO modes carry ids, which wait_for_frame() and
/// draw() wait for. Display output is sRGB-encoded once: by an sRGB swapchain format when the surface offers one,
/// otherwise in the display shader for 8-bit UNORM formats. Presentation semaphores belong to swapchain images. Where
/// the instance and device support `VK_EXT_swapchain_maintenance1`, presentation fences are waited before swapchain
/// resources are destroyed; otherwise, or with RendererOptions::disable_present_fences, a device wait-idle is used,
/// which unextended Vulkan does not guarantee to cover presentation. Portability enumeration and
/// `VK_KHR_portability_subset` are enabled when advertised, as on MoltenVK. Diagnostics are printed to standard output.
///
/// After shutdown(), request_capture(), set_view(), set_frustum_culling(), set_lod_threshold(), set_present_mode(),
/// set_environment(), set_time(), set_scenes(), prepare_meshes(), prepare_mesh(), wait_for_frame() and draw() throw
/// `std::logic_error`; after a RendererFatalError they throw RendererFatalError.
class VulkanRenderer {
  public:
    /// Creates the Vulkan instance, surface and device for @p window, then selects RendererOptions::scenes.
    ///
    /// @p window must be live and created with `SDL_WINDOW_VULKAN`. Uses the first Vulkan 1.1 device that supports
    /// swapchains, has a queue family for both graphics and compute, and can present to the window; the first draw()
    /// with a drawable window creates the swapchain. Throws `std::invalid_argument` for a RendererOptions::fail_after
    /// stage that the option says construction rejects, before anything else, then for a
    /// RendererOptions::max_anisotropy that is not finite or is below 1 ("Maximum anisotropy must be finite and at
    /// least 1"), for a RendererOptions::lod_threshold that is not finite or is negative ("LOD threshold must be finite
    /// and nonnegative"), for a RendererOptions::frames_in_flight other than 1 or 2 ("Frames in flight must be 1 or
    /// 2"), for a RendererOptions::present_mode that is not a PresentMode enumerator ("Unknown present mode") and for a
    /// null @p window; RendererUnavailableError when no driver or device can present to the window; what set_scenes()
    /// throws for the initial selection; InjectedRendererFailure for RendererOptions::fail_after; and
    /// `std::runtime_error` for other failures, including failed Vulkan calls. Completed stages are released before the
    /// exception propagates.
    VulkanRenderer(SDL_Window *window, RendererOptions options);
    /// Performs shutdown() if it has not run.
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    /// Makes the next draw() recreate the swapchain for the window's current pixel size.
    void request_resize() noexcept;
    /// Writes the next frame that draw() submits to @p path as a binary PPM, 8-bit RGB, creating missing
    /// parent directories; replaces any pending request and discards an image that take_capture() has not
    /// returned. Throws `std::invalid_argument` for an empty path.
    ///
    /// The swapchain is recreated first if it cannot be copied from. A request that fails is consumed: one
    /// draw() throws `std::runtime_error` for it, RenderStats::captured stays false, and later draws neither
    /// retry nor report it. When the surface has no 8-bit BGRA or RGBA format usable as a copy source, that
    /// draw() keeps the current swapchain and presents nothing; when the file cannot be written, it has already
    /// submitted the frame and, unless presentation reported the swapchain out of date, presented and counted
    /// it.
    void request_capture(std::filesystem::path path);
    /// Reads the next frame that draw() submits back into memory for take_capture(); replaces any pending
    /// request, including one for a file, and discards an image that take_capture() has not returned.
    ///
    /// The swapchain is recreated first if it cannot be copied from. A request that fails is consumed as a file
    /// request is: when the surface has no 8-bit BGRA or RGBA format usable as a copy source, one draw() throws
    /// `std::runtime_error`, keeps the current swapchain and presents nothing, RenderStats::captured stays
    /// false, and later draws neither retry nor report it.
    void request_capture();
    /// Hands over the image that the latest request_capture() read back. Empty until a draw() completes that
    /// request, after a newer request, and once the image has been taken.
    [[nodiscard]] std::optional<CapturedImage> take_capture();
    /// Sets the view used for shading, fog, the sky and culling from a column-major Vulkan view-projection
    /// (clip Y down, reversed depth from 1 at the near plane to 0 at the far one), such as anima::view_matrix
    /// returns, or anima::perspective() or anima::orthographic() times anima::look_at(). A matrix with forward depth
    /// would draw farther surfaces over nearer ones.
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
    /// Sets the largest error, in pixels of the view's height, that a level of detail (DrawLevel) may add on screen,
    /// from the next draw(); it starts as RendererOptions::lod_threshold, and 0 always draws the full draws, as Godot's
    /// mesh LOD threshold pixels do.
    ///
    /// Each draw of an object, and each placement cluster's copies of it, uses the coarsest of the draw's levels
    /// whose error, scaled by the largest axis scale among the matrices that may place the draw (its node's, or its
    /// skin's joints', IndexedDraw::palette_count of them), covers at most @p pixels when projected
    /// at the distance from the eye to the nearest point of the draw's, or the cluster's, world bounds; from inside
    /// those bounds it uses the full draw. An orthographic view projects errors without distance. Shadow passes
    /// draw the levels the view chose. Throws `std::invalid_argument` unless @p pixels is finite and nonnegative
    /// ("LOD threshold must be finite and nonnegative"), keeping the previous threshold.
    void set_lod_threshold(float pixels);
    /// Requests @p mode for presentation; it starts as RendererOptions::present_mode. When @p mode differs from the
    /// current request, the next draw() recreates the swapchain, as after request_resize(), in @p mode where the
    /// surface offers it and otherwise in PresentMode::fifo, which Vulkan requires every surface to offer;
    /// present_mode() reports which. Requesting the current mode again changes nothing. Throws `std::invalid_argument`
    /// for a value that is not a PresentMode enumerator ("Unknown present mode"), keeping the previous request.
    void set_present_mode(PresentMode mode);
    /// The present mode of the latest swapchain that draw() created, or empty before the first. After
    /// set_present_mode() it changes with the next swapchain.
    [[nodiscard]] std::optional<PresentMode> present_mode() const noexcept;
    /// Whether wait_for_frame() and draw() wait for presents in PresentMode::fifo and PresentMode::fifo_relaxed, which
    /// the constructor decides once: the device offers `VK_KHR_present_id` and `VK_KHR_present_wait` with their
    /// `presentId` and `presentWait` features, which the renderer then enables. The other modes queue no presents
    /// behind one another, so frames in them never wait for presents. RenderStats::present_waits counts the waits.
    [[nodiscard]] bool waits_for_presents() const noexcept;
    /// Replaces the lighting environment from the next draw(); it starts as a default Environment.
    ///
    /// Validates @p environment with validate_environment() and the detail region, enabled or not, with
    /// detail_shadow_matrix(). While the shadow cascades are enabled, ShadowCascades::resolution must fit the
    /// device's 2D image and framebuffer limits and ShadowCascades::count its image array layers, and so must the
    /// detail region's DirectionalShadow::resolution while it is enabled; disabled maps keep 1x1 placeholders, so
    /// their resolutions meet that check only in a call that enables them ("Shadow resolution exceeds device
    /// capabilities"). Invalid input throws `std::invalid_argument` or anima::MathError and keeps the previous
    /// environment. Each draw() fits the cascades to the view as fit_shadow_cascades() does, extends their depth over
    /// their casters, allocates changed shadow maps and throws SceneResourceError if that fails.
    void set_environment(const Environment &environment);
    /// Sets the seconds that custom material shaders read as the frame block's `time`, from the next draw(); it
    /// starts at 0. The renderer never advances it, so the application chooses the clock, pauses and rate. Shaders
    /// receive it as a 32-bit float, whose resolution coarsens as it grows (to about 1 ms from 8,192 s), so wrap it at
    /// a period that the shaders tolerate. Throws `std::invalid_argument` unless @p seconds is finite, keeping the
    /// previous time.
    void set_time(float seconds);
    /// Selects the scenes to draw, or clears the selection with an empty list; the renderer keeps the pointers.
    /// It never follows SceneSet::active(); SceneSet::render_scenes() lists a set's scenes.
    ///
    /// Throws `std::invalid_argument` for a null or repeated scene before any work. Then waits for every frame in
    /// flight and uploads the meshes of every visible, active instance, whatever the view. If that fails, the previous
    /// selection stays and meshes uploaded so far stay cached. It throws `std::invalid_argument` for a mesh beyond
    /// device limits (more vertices than the indexed-draw range, or a texture larger than the 2D image limit),
    /// `std::logic_error` as Mesh::texel_images() and CustomMaterial::texel_images() do for texels that
    /// TexelRetention::until_upload let go, `std::length_error` when the palettes exceed the storage-buffer range,
    /// `std::runtime_error` for other failures, including failed Vulkan calls such as allocations,
    /// InjectedRendererFailure as SceneReplacementOptions::fail_after requests, and RendererFatalError for device loss
    /// or a fence timeout. The swapchain is untouched, so this works while the window is minimized.
    ///
    /// Selected scenes remain the caller's to change between draws; each draw() prepares their current content.
    /// A scene that its SceneSet unloads or replaces stays selected but is empty.
    void set_scenes(std::vector<std::shared_ptr<const Scene>> scenes, SceneReplacementOptions options = {});
    /// Uploads @p assets into the mesh cache without selecting or drawing them, for example while loading.
    /// Cached or repeated meshes are reused.
    ///
    /// Throws `std::invalid_argument` for a null pointer anywhere in @p assets before any upload; an empty span
    /// does nothing. Leaves the selection, view and poses unchanged. Does not wait for the frames in flight: a call
    /// whose meshes are all cached submits no GPU work and returns without waiting, and the resources that earlier
    /// calls released while frames that may use them were in flight stay until a later draw() or set_scenes() has
    /// waited for those frames (ResourceStats::resident_material_samplers counts the samplers of such meshes). Each
    /// mesh that it uploads waits for the fences of its own copies, which Vulkan signals only after every command
    /// submitted to the queue before them, the frames in flight included. Not atomic: after a failure, meshes
    /// uploaded earlier stay cached while they have other owners. Throws
    /// `std::invalid_argument` for a mesh beyond device limits, `std::logic_error` as Mesh::texel_images() does for
    /// texels that TexelRetention::until_upload let go, `std::runtime_error` for other upload failures,
    /// including failed Vulkan calls, InjectedRendererFailure as ResourcePreparationOptions::fail_after
    /// requests, and RendererFatalError for device loss or a fence timeout.
    void prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets, ResourcePreparationOptions options = {});
    /// Uploads MeshPreparation::asset() as prepare_meshes() does, using the preparation's mip chains and
    /// block-compressed images instead of reading texels from the Mesh, so it also uploads a Mesh whose
    /// TexelRetention::until_upload texels an earlier upload let go. Allocation and upload still run synchronously on
    /// this thread. @p preparation is read only during the call, and not at all if the mesh is already cached, in which
    /// case the call returns without waiting, as prepare_meshes() describes.
    void prepare_mesh(const MeshPreparation &preparation, ResourcePreparationOptions options = {});
    /// Current cache and allocation sizes with the latest frame counters; all zero without asset support.
    [[nodiscard]] ResourceStats resource_stats() const noexcept;
    /// Whether ImageFormat::bc7 images upload as BC7, which the constructor decides once: the device has the
    /// `textureCompressionBC` feature and samples `VK_FORMAT_BC7_SRGB_BLOCK` and `VK_FORMAT_BC7_UNORM_BLOCK` with
    /// linear filtering and as copy destinations, and RendererOptions::decode_bc7 is off. Otherwise uploads decode
    /// them to RGBA8 on the CPU. Desktop GPUs sample BC7; an application that would rather load other images than
    /// pay for decoding can ask here first.
    [[nodiscard]] bool samples_bc7() const noexcept;
    /// The anisotropy that textures filter with, which the constructor decides once: the smaller of
    /// RendererOptions::max_anisotropy and the device's `maxSamplerAnisotropy`, or 1 on a device without the
    /// `samplerAnisotropy` feature. Only textures whose magnification and minification filters are linear and that
    /// sample a mip chain use it, custom material textures included; changing it requires a new renderer, since
    /// every material's descriptors hold its samplers.
    [[nodiscard]] float max_anisotropy() const noexcept;
    /// Waits for the frame that the next draw() or UiContext::render() waits for before preparing its own: the one
    /// submitted RendererOptions::frames_in_flight submissions before the frame that the call will submit.
    ///
    /// Where waits_for_presents() is true, it then waits for the present of the frame submitted one submission before
    /// that one, if that frame was presented to the current swapchain in PresentMode::fifo or
    /// PresentMode::fifo_relaxed: until `vkWaitForPresentKHR` reports the present, for at most 100 ms. A present that
    /// is not reported by then, as to a window that the system does not show, is not an error but holds the call for
    /// the whole 100 ms; nor is a swapchain that the wait finds out of date or suboptimal, which the next draw()
    /// recreates. RenderStats::present_waits and RenderStats::present_wait_timeouts count the waits. Vulkan asks
    /// drivers to report a present as close as possible to its display. On a driver that does, the wait holds the call
    /// only while that frame and the RendererOptions::frames_in_flight frames presented after it all wait for display,
    /// so that at most RendererOptions::frames_in_flight presented frames wait for display ahead of the next, and no
    /// frame's work waits for the display of the frame just before it. Vulkan does not require it, though:
    /// MoltenVK 1.4.1 reports a present once the GPU has finished the commands that present it, so there the wait
    /// bounds no display queue, and its submission paces frames instead (FrameProfile::record_submit_ms).
    ///
    /// Call it at the top of each frame, before reading input and updating the scenes and the view. draw()'s waits
    /// then return at once, so they no longer fall between reading the input and drawing it, and
    /// FrameProfile::fence_wait_ms times only the window checks and any swapchain recreation. A driver that paces
    /// presentation by blocking image acquisition, submission or presentation still blocks inside draw(). It waits
    /// whatever the window's state, and returns at once before the first submitted frame and when no frame has been
    /// submitted since the previous wait. Throws `std::logic_error` after shutdown(), RendererFatalError after a fatal
    /// failure, and RendererFatalError, after which the renderer accepts only shutdown, for any other failure of either
    /// wait, such as device or surface loss, and for a frame that does not finish within 5 seconds.
    void wait_for_frame();
    /// Prepares, records, submits and presents one frame; does not advance simulation or animation.
    ///
    /// Returns false while the window is hidden, minimized or zero-sized, when no image is acquired within
    /// 100 ms, and when the swapchain is out of date, which the next call recreates; keep running the event
    /// loop and call again. Returns true when the frame was presented.
    ///
    /// Each call waits for the frame submitted RendererOptions::frames_in_flight submissions before the one that it
    /// submits and, as wait_for_frame() describes, for the present of the frame submitted one submission before that,
    /// waits that return at once when wait_for_frame() has already made them, releases unowned cache entries, culls,
    /// uploads meshes that became visible or cast shadows (prepare_meshes() can upload them earlier) and writes every
    /// prepared instance's palette. The palettes of one frame must fit the device's storage-buffer range. Throws
    /// SceneResourceError when that preparation fails recoverably; RendererFatalError for device or surface loss, a
    /// fence timeout, any other Vulkan failure, or any failure to build a new swapchain once the previous one is
    /// released; and `std::runtime_error` for other failures, such as a surface that offers no usable format, which
    /// leaves the current swapchain in place, or a capture request that fails, which only that call reports (see
    /// request_capture()).
    [[nodiscard]] bool draw();
    /// Timings of the latest draw(); see FrameProfile.
    [[nodiscard]] FrameProfile frame_profile() const noexcept;
    /// Whether FrameProfile::gpu_idle_ms can be set, which the constructor decides once: RendererOptions::profile is
    /// on, the graphics queue writes timestamps, and the device offers `VK_KHR_calibrated_timestamps` or
    /// `VK_EXT_calibrated_timestamps`, which the renderer then enables. Vulkan orders timestamps written by different
    /// submissions only with one of them enabled, and the idle time compares two frames' timestamps.
    [[nodiscard]] bool measures_gpu_idle() const noexcept;
    /// Whether FrameProfile::present_interval_ms can be set, which the constructor decides once:
    /// RendererOptions::profile is on and the device offers `VK_GOOGLE_display_timing`, which the renderer then
    /// enables.
    [[nodiscard]] bool measures_present_interval() const noexcept;
    /// The counters so far, which shutdown() returns once they are final; after it, those final counters.
    [[nodiscard]] RenderStats stats() const noexcept;
    /// Waits for the device and presentation to finish, destroys every Vulkan object and returns the final
    /// counters, including failures during this cleanup. Idempotent. The waits have no timeout, so a hung
    /// driver blocks here and in the destructor.
    [[nodiscard]] RenderStats shutdown();

  private:
    friend class UiContext;
    // Whether the surface offers the sRGB format that UI blending needs; UiContext checks it at construction.
    [[nodiscard]] bool srgb_presentation();
    [[nodiscard]] bool draw_ui(const detail::UiFrame &frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anima
