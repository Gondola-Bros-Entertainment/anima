#include <anima/desktop/vulkan_renderer.hpp>
#ifdef ANIMA_HAS_ASSETS
#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/assets/render_visibility.hpp>
#include <anima/scene.hpp>
#include <map>
#endif
#ifdef ANIMA_UI
#include "ui_draw.hpp"
#include <map>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace anima {
namespace {
constexpr std::uint64_t fence_timeout = 5'000'000'000ULL;
// Longest that a frame waits for the present of an earlier one, in nanoseconds. That wait only paces frames, so a
// present that never completes, as to a window the system does not show, holds a frame back no longer than this.
constexpr std::uint64_t present_wait_timeout = 100'000'000ULL;
// Display times that one call reads from VK_GOOGLE_display_timing before asking for more.
constexpr std::uint32_t display_time_batch = 16;
// Most frames that RendererOptions::frames_in_flight allows.
constexpr std::uint32_t max_frames_in_flight = 2;
// Throws `std::invalid_argument` unless @p scale is finite and from VulkanRenderer::min_render_scale to
// VulkanRenderer::max_render_scale.
void validate_render_scale(float scale) {
    if (!std::isfinite(scale) || scale < VulkanRenderer::min_render_scale || scale > VulkanRenderer::max_render_scale)
        throw std::invalid_argument("Render scale must be finite and from 0.25 to 2");
}
// What construction and VulkanRenderer::set_render_scale() report for a render scale without asset support.
[[maybe_unused]] constexpr auto render_scale_without_assets = "Render scale requires asset support";
constexpr std::uint32_t vertex_code[] =
#include "triangle.vert.inc"
    ;
constexpr std::uint32_t fragment_code[] =
#include "triangle.frag.inc"
    ;
#ifdef ANIMA_UI
constexpr std::uint32_t ui_vertex_code[] =
#include "ui.vert.inc"
    ;
constexpr std::uint32_t ui_fragment_code[] =
#include "ui.frag.inc"
    ;
#endif
#ifdef ANIMA_HAS_ASSETS
constexpr std::uint32_t shadow_resource_vertex_code[] =
#include "shadow-resource.vert.inc"
    ;
constexpr std::uint32_t shadow_fragment_code[] =
#include "shadow.frag.inc"
    ;
constexpr std::uint32_t sky_vertex_code[] =
#include "sky.vert.inc"
    ;
constexpr std::uint32_t sky_fragment_code[] =
#include "sky.frag.inc"
    ;
constexpr std::uint32_t resolve_fragment_code[] =
#include "resolve.frag.inc"
    ;
constexpr std::uint32_t mesh_fragment_code[] =
#include "mesh.frag.inc"
    ;
constexpr std::uint32_t resource_vertex_code[] =
#include "resource.vert.inc"
    ;
constexpr std::uint32_t impostor_vertex_code[] =
#include "impostor.vert.inc"
    ;
constexpr std::uint32_t impostor_fragment_code[] =
#include "impostor.frag.inc"
    ;
constexpr std::uint32_t impostor_shadow_fragment_code[] =
#include "impostor-shadow.frag.inc"
    ;
constexpr std::uint32_t atmosphere_transmittance_code[] =
#include "atmosphere-transmittance.comp.inc"
    ;
constexpr std::uint32_t atmosphere_scattering_code[] =
#include "atmosphere-scattering.comp.inc"
    ;
constexpr std::uint32_t atmosphere_sky_code[] =
#include "atmosphere-sky.comp.inc"
    ;
#endif
struct VulkanFailure : std::runtime_error {
    VkResult result;
    VulkanFailure(VkResult value, const char *operation)
        : std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(value) + ")"),
          result(value) {}
};
void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw VulkanFailure(result, operation);
}
template <class T, class F> std::vector<T> enumerate(F query, const char *operation) {
    std::vector<T> values;
    for (;;) {
        std::uint32_t count = 0;
        check(query(&count, static_cast<T *>(nullptr)), operation);
        values.resize(count);
        if (count == 0)
            return values;
        const auto result = query(&count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        check(result, operation);
        values.resize(count);
        return values;
    }
}
bool has_extension(const std::vector<VkExtensionProperties> &properties, const char *name) {
    return std::any_of(properties.begin(), properties.end(),
                       [name](const auto &p) { return std::strcmp(p.extensionName, name) == 0; });
}
// Converts to the type of any field, so that brace initialization of an aggregate from one of these per field compiles
// and from one more does not. Declared only, for unevaluated operands.
struct AnyField {
    template <class T> operator T() const;
};
// Whether T can be brace-initialized from one AnyField for each of @p I.
template <class T, std::size_t... I> constexpr bool initializable_from_any(std::index_sequence<I...>) {
    return requires { T{(static_cast<void>(I), AnyField{})...}; };
}
// Whether the aggregate T has exactly @p count fields. A field of class type, such as a Vec3, counts once, and an array
// field once per element.
template <class T, std::size_t count>
constexpr bool has_fields = initializable_from_any<T>(std::make_index_sequence<count>{}) &&
                            !initializable_from_any<T>(std::make_index_sequence<count + 1>{});
// Swapchain formats in order of preference, each in VK_COLOR_SPACE_SRGB_NONLINEAR_KHR.
constexpr std::array preferred_surface_formats{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                               VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
// The Vulkan present mode of @p mode; throws std::invalid_argument for a value that is not a PresentMode enumerator.
VkPresentModeKHR vulkan_present_mode(PresentMode mode) {
    switch (mode) {
    case PresentMode::fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case PresentMode::immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::fifo_relaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    throw std::invalid_argument("Unknown present mode");
}
const char *present_mode_name(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::fifo:
        return "fifo";
    case PresentMode::immediate:
        return "immediate";
    case PresentMode::mailbox:
        return "mailbox";
    case PresentMode::fifo_relaxed:
        return "fifo_relaxed";
    }
    return "unknown";
}
} // namespace

struct VulkanRenderer::Impl {
    SDL_Window *window;
    RendererOptions options;
    RenderStats stats{};
    FrameProfile profile{};
    // Timestamps of a profiled frame, in the order draw() writes them; FrameProfile's GPU fields are the
    // intervals between them.
    struct TimingQuery {
        enum : std::uint32_t { start, after_atmosphere, after_shadows, after_scene, after_resolve, end, count };
    };
    // TimingQuery::count queries for each frame slot (FrameSlot::first_query).
    VkQueryPool timing_queries{};
    std::uint32_t timestamp_bits{};
    double timestamp_period{};
    // Whether a calibrated timestamps extension is enabled, so that timestamps from different submissions compare.
    bool calibrated_timestamps{};
    // A frame whose timestamps were read: its end timestamp, and when it was submitted on the host's clock.
    struct TimedFrame {
        std::uint64_t end{};
        std::chrono::steady_clock::time_point submitted;
    };
    // The latest frame whose timestamps were read; empty when the latest frame read could not be. Each draw() reads the
    // timestamps of the frame that its slot submitted last, and the slots submit in turn, so frames are read in the
    // order of their submission.
    std::optional<TimedFrame> previous_frame;
    std::atomic<std::uint32_t> warnings{}, errors{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    // Suballocates every buffer and image, so allocations stay far below maxMemoryAllocationCount.
    VmaAllocator allocator{};
    std::uint32_t graphics_family{}, present_family{};
    VkQueue graphics_queue{}, present_queue{};
    bool maintenance_instance{}, present_fences{}, resize = true, stopped = false, fatal = false;
    // With VK_EXT_surface_maintenance1, the query of a surface's capabilities for one present mode.
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surface_capabilities2{};
    // Whether VK_KHR_present_id and VK_KHR_present_wait are enabled with their features, so that frames presented in
    // the FIFO modes carry ids and later frames wait for their presents; see awaits_presents().
    bool present_waits{};
    PFN_vkWaitForPresentKHR wait_for_present{};
    // Whether VK_GOOGLE_display_timing is enabled, which RendererOptions::profile asks for, so that presents carry
    // ids whose display times set FrameProfile::present_interval_ms.
    bool display_timing{};
    PFN_vkGetPastPresentationTimingGOOGLE past_presentation_timing{};
    // Presents so far, each of which takes the next value as its id.
    std::uint64_t presents{};
    // The mode of the latest swapchain created; empty before the first.
    std::optional<PresentMode> presenting;
    // The latest frame of the current swapchain that VK_GOOGLE_display_timing reported displayed: its id and display
    // time in nanoseconds. Display times arrive in batches, gathered in past_timings.
    struct DisplayedFrame {
        std::uint32_t id{};
        std::uint64_t time{};
    };
    std::optional<DisplayedFrame> latest_display;
    std::vector<VkPastPresentationTimingGOOGLE> past_timings;
    // Whether BC7 images upload as BC7: the device samples BC7 with linear filtering and RendererOptions::decode_bc7
    // is off. Otherwise they upload decoded to RGBA8.
    bool bc7_sampled{};
    // Anisotropy of linear, mipmapped material samplers: RendererOptions::max_anisotropy within the device's limit, or
    // 1 without the samplerAnisotropy feature.
    float anisotropy = 1;
    VkShaderModule vertex_shader{}, fragment_shader{}, mesh_fragment_shader{};
    VkPipelineLayout pipeline_layout{}, environment_pipeline_layout{};
    VkShaderModule ui_vertex_shader{}, ui_fragment_shader{};
    VkPipelineLayout ui_pipeline_layout{};
    VkDescriptorSetLayout ui_texture_layout{};
    VkDescriptorSetLayout texture_layout{};
    struct GpuSampler {
        VkDevice device{};
        VkSampler handle{};
        ~GpuSampler() {
            if (handle)
                vkDestroySampler(device, handle, nullptr);
        }
    };
    struct GpuTexture {
        VkImage image{};
        VmaAllocation allocation{};
        VkImageView view{};
        VkSampler sampler{};
        std::shared_ptr<GpuSampler> shared_sampler;
        VkDeviceSize allocation_bytes{};
    };
#ifdef ANIMA_HAS_ASSETS
    struct ResourceBuffer {
        VmaAllocator allocator{};
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VkDeviceSize bytes{}, allocation_bytes{};
        // Host-visible buffers stay mapped until they are destroyed.
        void *mapping{};
        ~ResourceBuffer() {
            if (buffer)
                vmaDestroyBuffer(allocator, buffer, allocation);
        }
    };
    using SamplerKey = std::tuple<Filter, Filter, Filter, Wrap, Wrap, bool, std::uint32_t>;
    // Weak entries never extend GPU lifetime beyond the scenes using a sampler.
    std::map<SamplerKey, std::weak_ptr<GpuSampler>> material_samplers;
    // Every candidate owns its allocations immediately. Destruction is legal only
    // after its graphics/upload work has completed; see UploadBatch and replacement.
    struct MaterialUniform {
        std::array<float, 4> emissive_alpha, surface, maps;
    };
    struct GpuMaterials {
        VkDevice device{};
        VmaAllocator allocator{};
        std::shared_ptr<const MeshSnapshot> source;
        VkDescriptorPool texture_pool{};
        VkBuffer material_buffer{};
        VmaAllocation material_allocation{};
        std::vector<VkDescriptorSet> material_sets;
        std::vector<GpuTexture> textures;
        ~GpuMaterials() {
            if (texture_pool)
                vkDestroyDescriptorPool(device, texture_pool, nullptr);
            if (material_buffer)
                vmaDestroyBuffer(allocator, material_buffer, material_allocation);
            for (auto &texture : textures) {
                if (texture.sampler && !texture.shared_sampler)
                    vkDestroySampler(device, texture.sampler, nullptr);
                if (texture.view)
                    vkDestroyImageView(device, texture.view, nullptr);
                if (texture.image)
                    vmaDestroyImage(allocator, texture.image, texture.allocation);
            }
        }
        GpuMaterials() = default;
        GpuMaterials(const GpuMaterials &) = delete;
        GpuMaterials &operator=(const GpuMaterials &) = delete;
    };
#endif
    struct UploadBatch {
        VkDevice device{};
        VmaAllocator allocator{};
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        VkBuffer staging_buffer{};
        VmaAllocation staging_allocation{};
        bool pending{};
        bool simulate_device_loss{}; // Validation hook, evaluated after real work retires.
        bool *fatal{};
        std::atomic<std::uint32_t> *errors{};
        void create(std::uint32_t family) {
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = family;
            check(vkCreateCommandPool(device, &info, nullptr, &pool), "Create upload command pool");
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device, &allocation, &command), "Allocate upload command buffer");
            VkFenceCreateInfo info_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            check(vkCreateFence(device, &info_fence, nullptr, &fence), "Create upload fence");
        }
        void release_staging() noexcept {
            if (staging_buffer)
                vmaDestroyBuffer(allocator, staging_buffer, staging_allocation);
            staging_buffer = VK_NULL_HANDLE;
            staging_allocation = VK_NULL_HANDLE;
        }
        void wait() {
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout), "Wait for texture upload");
            pending = false;
        }
        ~UploadBatch() {
            // A timed-out or deliberately interrupted submit still owns resources.
            // Retire it before the candidate unwinds, even if that needs the process
            // watchdog. Device loss also ends pending use; it is never recoverable.
            if (pending) {
                const auto result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
                if ((result == VK_ERROR_DEVICE_LOST || simulate_device_loss) && fatal)
                    *fatal = true;
                if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
                    if (errors)
                        ++*errors;
                    std::fprintf(stderr, "Upload retirement failed: %d\n", result);
                    // Completion is unknown. Never free memory that the queue
                    // might still use, even if a driver cannot retire the work.
                    std::terminate();
                }
            }
            release_staging();
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
        }
        UploadBatch() = default;
        UploadBatch(const UploadBatch &) = delete;
        UploadBatch &operator=(const UploadBatch &) = delete;
    };
#ifdef ANIMA_HAS_ASSETS
#include "atmosphere_renderer.inc"
#include "environment_renderer.inc"
#include "resource_renderer.inc"
#include "shadow_renderer.inc"
#include "world_renderer.inc"
// Custom materials draw the resource renderer's instances, so their declarations follow it.
#include "custom_renderer.inc"
#endif
    Mat4 view_projection{};
    std::array<float, 4> view_origin{0, 0, -1, 0};

    VkSwapchainKHR swapchain{};
    // The swapchain's size, which display conversion, UI and captures use, and the size of the current scene targets,
    // which the view renders at: the swapchain's times the render scale with asset support (scene_size()), and the
    // swapchain's without. With asset support each swapchain creation leaves scene_extent zero until draw() creates the
    // scene targets, and a failure to create them keeps it zero.
    VkExtent2D extent{}, scene_extent{};
    VkFormat format{};
    VkRenderPass render_pass{};
    VkPipeline pipeline{}, ui_pipeline{};
    // The view's depth attachment, of scene_extent.
    struct DepthTarget {
        VkDevice device{};
        VmaAllocator allocator{};
        VkImage image{};
        VmaAllocation allocation{};
        VkImageView view{};
        VkDeviceSize allocation_bytes{};
        ~DepthTarget() {
            if (view)
                vkDestroyImageView(device, view, nullptr);
            if (image)
                vmaDestroyImage(allocator, image, allocation);
        }
        DepthTarget() = default;
        DepthTarget(const DepthTarget &) = delete;
        DepthTarget &operator=(const DepthTarget &) = delete;
    };
    std::unique_ptr<DepthTarget> depth_target;
    VkFormat depth_format{};
    struct Image {
        VkImage image{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        // Presentation waits belong to acquired images, not to CPU frames.
        VkSemaphore rendered{};
        VkFence presented{};
        bool present_pending{};
    };
    std::vector<Image> images;
    VkBuffer capture_buffer{};
    VmaAllocation capture_allocation{};
    void *capture_mapping{};
    // Whether the pending request reads the frame into memory instead of options.capture, and the image read
    // back, held until take_capture().
    bool capture_to_memory{};
    std::optional<CapturedImage> captured_image;

    Impl(SDL_Window *borrowed_window, RendererOptions settings)
        : window(borrowed_window), options(std::move(settings)) {}
    ~Impl() { cleanup(); }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT,
                                                         const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                         void *user) noexcept {
        auto &self = *static_cast<Impl *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++self.errors;
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            ++self.warnings;
        std::fprintf(stderr, "[Vulkan validation] %s\n", data->pMessage);
        return VK_FALSE;
    }
    void fail_after(RendererFailureStage stage) {
        if (options.fail_after == stage)
            throw InjectedRendererFailure(stage, true);
    }
    void initialize() {
        // Reject stages that construction and draw() never fire, before any work, as set_scenes() does.
        constexpr auto unknown_stage = "Unknown initialization failure stage";
        switch (options.fail_after) {
        case RendererFailureStage::none:
        case RendererFailureStage::instance:
        case RendererFailureStage::surface:
        case RendererFailureStage::device:
        case RendererFailureStage::resources:
        case RendererFailureStage::swapchain:
            break;
        case RendererFailureStage::texture:
        case RendererFailureStage::texture_upload:
#ifdef ANIMA_HAS_ASSETS
            // Only the initial selection's mesh uploads fire these.
            if (!options.scenes.empty())
                break;
#endif
            throw std::invalid_argument(unknown_stage);
        case RendererFailureStage::scene_targets:
#ifdef ANIMA_HAS_ASSETS
            // draw() fires it while the render scale is above 1.
            break;
#else
            // Without asset support the render scale is always 1.
            throw std::invalid_argument(unknown_stage);
#endif
        default:
            throw std::invalid_argument(unknown_stage);
        }
        if (!std::isfinite(options.max_anisotropy) || options.max_anisotropy < 1)
            throw std::invalid_argument("Maximum anisotropy must be finite and at least 1");
        if (!std::isfinite(options.lod_threshold) || options.lod_threshold < 0)
            throw std::invalid_argument("LOD threshold must be finite and nonnegative");
        if (options.frames_in_flight < 1 || options.frames_in_flight > max_frames_in_flight)
            throw std::invalid_argument("Frames in flight must be 1 or 2");
        (void)vulkan_present_mode(options.present_mode);
        validate_render_scale(options.render_scale);
#ifndef ANIMA_HAS_ASSETS
        // Without the scene targets the view renders straight into the swapchain images.
        if (options.render_scale != 1)
            throw std::invalid_argument(render_scale_without_assets);
#endif
        if (!window)
            throw std::invalid_argument("Renderer requires an SDL window");
        create_instance();
        fail_after(RendererFailureStage::instance);
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        fail_after(RendererFailureStage::surface);
        create_device();
        fail_after(RendererFailureStage::device);
        create_frame_resources();
#ifdef ANIMA_HAS_ASSETS
        SceneReplacementOptions initial;
        if (options.fail_after == RendererFailureStage::texture ||
            options.fail_after == RendererFailureStage::texture_upload)
            initial.fail_after = options.fail_after;
        set_scenes(std::move(options.scenes), initial, true);
#else
        if (!options.scenes.empty())
            throw std::invalid_argument("Resource rendering requires the asset library");
        ++stats.scene_generations;
#endif
        fail_after(RendererFailureStage::resources);
        // Swapchain creation is deferred if the window starts minimized/zero-sized.
    }
    void create_instance() {
        const auto extensions = enumerate<VkExtensionProperties>(
            [](auto *n, auto *p) { return vkEnumerateInstanceExtensionProperties(nullptr, n, p); },
            "Enumerate instance extensions");
        std::uint32_t count = 0;
        const auto *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!sdl_extensions)
            throw std::runtime_error(SDL_GetError());
        std::vector<const char *> enabled(sdl_extensions, sdl_extensions + count);
        // The window system's surface extensions come from the installed drivers.
        for (const auto *name : enabled)
            if (!has_extension(extensions, name))
                throw RendererUnavailableError(std::string("No installed Vulkan driver provides ") + name);
        VkInstanceCreateFlags flags = 0;
        if (has_extension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        maintenance_instance = has_extension(extensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
                               has_extension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        if (maintenance_instance) {
            enabled.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            enabled.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        }
        const char *validation_layer = "VK_LAYER_KHRONOS_validation";
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = debug_callback;
        debug.pUserData = this;
        if (options.validation) {
            const auto layers = enumerate<VkLayerProperties>(
                [](auto *n, auto *p) { return vkEnumerateInstanceLayerProperties(n, p); }, "Enumerate instance layers");
            if (!std::any_of(layers.begin(), layers.end(),
                             [&](const auto &layer) { return std::strcmp(layer.layerName, validation_layer) == 0; }) ||
                !has_extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                throw std::runtime_error("--validation requires VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils");
            enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Anima";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "Anima";
        app.engineVersion = app.applicationVersion;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.flags = flags;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
        info.ppEnabledExtensionNames = enabled.data();
        if (options.validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &validation_layer;
            info.pNext = &debug;
        }
        const auto created = vkCreateInstance(&info, nullptr, &instance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER)
            throw RendererUnavailableError("No installed Vulkan driver supports Vulkan 1.1 (VkResult " +
                                           std::to_string(created) + ")");
        check(created, "Create instance");
        if (maintenance_instance) {
            surface_capabilities2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
            if (!surface_capabilities2)
                throw std::runtime_error("Surface capabilities entry point unavailable");
        }
        if (options.validation) {
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create)
                throw std::runtime_error("Debug messenger entry point unavailable");
            check(create(instance, &debug, nullptr, &messenger), "Create debug messenger");
        }
    }
    void create_device() {
        const auto devices = enumerate<VkPhysicalDevice>(
            [&](auto *n, auto *p) { return vkEnumeratePhysicalDevices(instance, n, p); }, "Enumerate physical devices");
        std::vector<VkExtensionProperties> selected_extensions;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1)
                continue;
            const auto extensions = enumerate<VkExtensionProperties>(
                [&](auto *n, auto *p) { return vkEnumerateDeviceExtensionProperties(candidate, nullptr, n, p); },
                "Enumerate device extensions");
            if (!has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            auto graphics = UINT32_MAX, present = UINT32_MAX;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (families[i].queueCount == 0)
                    continue;
                VkBool32 supported = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported), "Query present support");
                // The atmosphere's tables are built by compute dispatches on the graphics queue; a device with a
                // graphics family has one that also computes.
                constexpr VkQueueFlags world = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if ((families[i].queueFlags & world) == world)
                    graphics = i;
                if (supported)
                    present = i;
                if (supported && (families[i].queueFlags & world) == world)
                    break;
            }
            if (graphics == UINT32_MAX || present == UINT32_MAX)
                continue;
            std::uint32_t formats = 0, modes = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formats, nullptr), "Query surface formats");
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &modes, nullptr),
                  "Query present modes");
            if (formats == 0 || modes == 0)
                continue;
            physical = candidate;
            graphics_family = graphics;
            timestamp_bits = families[graphics].timestampValidBits;
            timestamp_period = properties.limits.timestampPeriod;
#ifdef ANIMA_HAS_ASSETS
            // The scene targets are 2D images attached to framebuffers.
            const auto &limits = properties.limits;
            max_scene_extent = {std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth),
                                std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight)};
#endif
            present_family = present;
            selected_extensions = extensions;
            std::cout << "GPU: " << properties.deviceName << "; Vulkan " << VK_VERSION_MAJOR(properties.apiVersion)
                      << '.' << VK_VERSION_MINOR(properties.apiVersion) << '.'
                      << VK_VERSION_PATCH(properties.apiVersion) << '\n';
            break;
        }
        if (!physical)
            throw RendererUnavailableError("No Vulkan 1.1 graphics/present device with swapchain support");
        std::vector<const char *> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        // Required when advertised by portability implementations, including MoltenVK.
        if (has_extension(selected_extensions, "VK_KHR_portability_subset"))
            extensions.push_back("VK_KHR_portability_subset");
        // Vulkan orders the timestamps of different submissions only with calibrated timestamps enabled, which the
        // profile's idle time between frames needs. The names are spelled out because the KHR extension is newer than
        // the oldest headers the build accepts.
        if (options.profile && timestamp_bits && timestamp_period > 0)
            for (const char *calibrated : {"VK_KHR_calibrated_timestamps", "VK_EXT_calibrated_timestamps"})
                if (has_extension(selected_extensions, calibrated)) {
                    extensions.push_back(calibrated);
                    calibrated_timestamps = true;
                    break;
                }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        if (maintenance_instance && !options.disable_present_fences &&
            has_extension(selected_extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &maintenance;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            present_fences = maintenance.swapchainMaintenance1;
            if (present_fences)
                extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }
        // Waiting for a present needs both its id and the wait, each an extension with a feature of its own.
        VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        VkPhysicalDevicePresentWaitFeaturesKHR present_wait{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
        if (has_extension(selected_extensions, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            has_extension(selected_extensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            present_id.pNext = &present_wait;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &present_id;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            present_waits = present_id.presentId && present_wait.presentWait;
            if (present_waits) {
                extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            }
        }
        display_timing = options.profile && has_extension(selected_extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        if (display_timing)
            extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        const float priority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (auto family : {graphics_family, present_family}) {
            if (!queues.empty() && family == queues.front().queueFamilyIndex)
                continue;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
        info.pQueueCreateInfos = queues.data();
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        VkPhysicalDeviceFeatures available{}, enabled{};
        vkGetPhysicalDeviceFeatures(physical, &available);
        enabled.fullDrawIndexUint32 = available.fullDrawIndexUint32;
        // BC formats need the feature, and BC7 images also need sampling with linear filtering and copies into them.
        enabled.textureCompressionBC = available.textureCompressionBC;
        const auto bc7_features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        bc7_sampled = available.textureCompressionBC && !options.decode_bc7;
        for (const auto bc7_format : {VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, bc7_format, &properties);
            bc7_sampled = bc7_sampled && (properties.optimalTilingFeatures & bc7_features) == bc7_features;
        }
        // Anisotropic filtering is an optional feature, and its degree is limited by the device.
        if (available.samplerAnisotropy && options.max_anisotropy > 1) {
            enabled.samplerAnisotropy = VK_TRUE;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            anisotropy = std::min(options.max_anisotropy, properties.limits.maxSamplerAnisotropy);
        }
        info.pEnabledFeatures = &enabled;
        // Each enabled extension's feature structure, as the queries above filled it, joins the chain.
        void *feature_chain = nullptr;
        const auto enable = [&](auto &feature) {
            feature.pNext = feature_chain;
            feature_chain = &feature;
        };
        if (present_fences)
            enable(maintenance);
        if (present_waits) {
            enable(present_id);
            enable(present_wait);
        }
        info.pNext = feature_chain;
        check(vkCreateDevice(physical, &info, nullptr, &device), "Create device");
        // The loader need not export extension commands, so they are looked up through the device.
        if (present_waits) {
            wait_for_present =
                reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device, "vkWaitForPresentKHR"));
            if (!wait_for_present)
                throw std::runtime_error("Present wait entry point unavailable");
        }
        if (display_timing) {
            past_presentation_timing = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                vkGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));
            if (!past_presentation_timing)
                throw std::runtime_error("Display timing entry point unavailable");
        }
        VmaAllocatorCreateInfo allocator_info{};
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_1;
        allocator_info.physicalDevice = physical;
        allocator_info.device = device;
        allocator_info.instance = instance;
        check(vmaCreateAllocator(&allocator_info, &allocator), "Create memory allocator");
        vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, present_family, 0, &present_queue);
        std::cout << "Presentation retirement: "
                  << (present_fences ? "EXT_swapchain_maintenance1 fences" : "Vulkan 1.1 wait-idle fallback") << '\n';
        std::cout << "Present waits: " << (present_waits ? "KHR_present_wait in the FIFO modes" : "none") << '\n';
        std::cout << "BC7 textures: " << (bc7_sampled ? "sampled as BC7" : "decoded to RGBA8 on the CPU") << '\n';
        std::cout << "Texture anisotropy: " << anisotropy << '\n';
    }
    void create_frame_resources() {
        frames.resize(options.frames_in_flight);
        for (std::uint32_t i = 0; i < frames.size(); ++i) {
            auto &slot = frames[i];
            // Each draw() resets the slot's whole pool once the slot's frame has finished.
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = graphics_family;
            check(vkCreateCommandPool(device, &pool, nullptr, &slot.pool), "Create command pool");
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = slot.pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device, &allocation, &slot.command), "Allocate command buffer");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device, &fence, nullptr, &slot.fence), "Create frame fence");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device, &semaphore, nullptr, &slot.acquired), "Create acquire semaphore");
            slot.first_query = i * TimingQuery::count;
        }
        if (options.profile && timestamp_bits && timestamp_period > 0) {
            VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = TimingQuery::count * static_cast<std::uint32_t>(frames.size());
            check(vkCreateQueryPool(device, &queries, nullptr, &timing_queries), "Create timing query pool");
        }
        const auto make_shaders = [&](const auto &vert, const auto &frag, VkShaderModule &vs, VkShaderModule &fs) {
            VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shader.codeSize = sizeof(vert);
            shader.pCode = vert;
            check(vkCreateShaderModule(device, &shader, nullptr, &vs), "Create vertex shader");
            shader.codeSize = sizeof(frag);
            shader.pCode = frag;
            check(vkCreateShaderModule(device, &shader, nullptr, &fs), "Create fragment shader");
        };
        make_shaders(vertex_code, fragment_code, vertex_shader, fragment_shader);
        // The diagnostic triangle's push range, in bytes; its shader reads only the leading vec2 scale.
        constexpr std::uint32_t diagnostic_push_bytes = 16 * sizeof(float);
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, diagnostic_push_bytes};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout), "Create triangle pipeline layout");
#ifdef ANIMA_UI
        make_shaders(ui_vertex_code, ui_fragment_code, ui_vertex_shader, ui_fragment_shader);
        create_ui_layout();
#endif
#ifdef ANIMA_HAS_ASSETS
        create_environment_layout();
        create_atmosphere(atmosphere_transmittance_code, atmosphere_scattering_code, atmosphere_sky_code);
        make_shaders(sky_vertex_code, sky_fragment_code, sky_vertex_shader, sky_fragment_shader);
        const auto make_fragment = [&](const auto &code, VkShaderModule &shader) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = sizeof(code);
            info.pCode = code;
            check(vkCreateShaderModule(device, &info, nullptr, &shader), "Create world output shader");
        };
        make_fragment(mesh_fragment_code, mesh_fragment_shader);
        make_fragment(resolve_fragment_code, resolve_fragment_shader);
        create_world_layout();
        std::array<VkDescriptorSetLayoutBinding, material_texture_count + 1> bindings{};
        for (unsigned i = 0; i < bindings.size(); ++i)
            bindings[i] = {i,
                           i == material_texture_count ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptor.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &texture_layout), "Create texture layout");
        const VkDescriptorSetLayout environment_layouts[]{texture_layout, environment_layout};
        layout.setLayoutCount = 2;
        layout.pSetLayouts = environment_layouts;
        layout.pushConstantRangeCount = 0;
        layout.pPushConstantRanges = nullptr;
        check(vkCreatePipelineLayout(device, &layout, nullptr, &environment_pipeline_layout),
              "Create mesh pipeline layout");
        create_resource_layout();
        create_custom_layout();
        make_shaders(shadow_resource_vertex_code, shadow_fragment_code, shadow_resource_vertex_shader,
                     shadow_fragment_shader);
        make_shaders(impostor_vertex_code, impostor_fragment_code, impostor_vertex_shader, impostor_fragment_shader);
        make_fragment(impostor_shadow_fragment_code, impostor_shadow_fragment_shader);
        create_shadow_pass();
        create_pipeline(PipelineKind::shadow_opaque, shadow_opaque_pipeline);
        create_pipeline(PipelineKind::shadow_masked, shadow_masked_pipeline);
        create_pipeline(PipelineKind::shadow_impostor, shadow_impostor_pipeline);
#endif
    }
    void running() const {
        if (stopped)
            throw std::logic_error("Renderer is shut down");
        if (fatal)
            throw RendererFatalError("Renderer has a fatal failure; only shutdown is legal");
    }
    static void inject_scene(RendererFailureStage failure, RendererFailureStage stage, bool initial) {
        if (failure == stage)
            throw InjectedRendererFailure(stage, initial);
    }
    bool drawable(VkExtent2D &pixels) const {
        if (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
            return false;
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height))
            throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels: ") + SDL_GetError());
        if (width <= 0 || height <= 0)
            return false;
        pixels = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        return true;
    }
    void wait_for_presentation(std::uint64_t timeout = fence_timeout) {
        for (auto &image : images) {
            if (image.present_pending) {
                check(vkWaitForFences(device, 1, &image.presented, VK_TRUE, timeout), "Wait for presentation fence");
                image.present_pending = false;
            }
        }
    }
    VkSurfaceCapabilitiesKHR surface_capabilities() const {
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "Query surface capabilities");
        return caps;
    }
    // The format recreate() gives the swapchain: the first preferred format that the surface offers, otherwise
    // its first format. A lone VK_FORMAT_UNDEFINED means that the surface accepts any format.
    VkSurfaceFormatKHR surface_format() const {
        const auto formats = enumerate<VkSurfaceFormatKHR>(
            [&](auto *n, auto *p) { return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, p); },
            "Enumerate surface formats");
        if (formats.empty())
            throw std::runtime_error("Surface has no formats");
        if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED)
            return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (const auto desired : preferred_surface_formats) {
            const auto found = std::find_if(formats.begin(), formats.end(), [&](const auto &value) {
                return value.format == desired && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (found != formats.end())
                return *found;
        }
        return formats.front();
    }
    // The mode that recreate() gives the swapchain: the requested one where the surface offers it, otherwise FIFO,
    // which Vulkan requires every surface to offer.
    PresentMode supported_present_mode() const {
        const auto modes = enumerate<VkPresentModeKHR>(
            [&](auto *n, auto *p) { return vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, n, p); },
            "Enumerate present modes");
        if (std::find(modes.begin(), modes.end(), vulkan_present_mode(options.present_mode)) != modes.end())
            return options.present_mode;
        std::cout << "Present mode " << present_mode_name(options.present_mode) << " is unavailable; presenting in "
                  << present_mode_name(PresentMode::fifo) << '\n';
        return PresentMode::fifo;
    }
    // The swapchain's image count for @p mode, which the surface offers: one more than its least, within its most.
    // Vulkan bounds the count by @p caps, the capabilities of every mode; with VK_EXT_surface_maintenance1 the least
    // and most for @p mode alone, which may be more or fewer, bound it too.
    std::uint32_t image_count(const VkSurfaceCapabilitiesKHR &caps, PresentMode mode) const {
        auto least = caps.minImageCount;
        // Zero means no most.
        auto most = caps.maxImageCount;
        if (surface_capabilities2) {
            VkSurfacePresentModeEXT present{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};
            present.presentMode = vulkan_present_mode(mode);
            VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
            info.pNext = &present;
            info.surface = surface;
            VkSurfaceCapabilities2KHR capabilities{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
            check(surface_capabilities2(physical, &info, &capabilities), "Query present mode capabilities");
            const auto &mode_caps = capabilities.surfaceCapabilities;
            least = std::max(least, mode_caps.minImageCount);
            if (mode_caps.maxImageCount != 0 && (most == 0 || mode_caps.maxImageCount < most))
                most = mode_caps.maxImageCount;
        }
        const auto count = least + 1;
        return most != 0 ? std::min(count, most) : count;
    }
    static VkExtent2D surface_extent(VkExtent2D pixels, const VkSurfaceCapabilitiesKHR &caps) {
        return caps.currentExtent.width != UINT32_MAX
                   ? caps.currentExtent
                   : VkExtent2D{std::clamp(pixels.width, caps.minImageExtent.width, caps.maxImageExtent.width),
                                std::clamp(pixels.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    bool recreate(VkExtent2D pixels) {
        const auto caps = surface_capabilities();
        const auto selected_extent = surface_extent(pixels, caps);
        if (selected_extent.width == 0 || selected_extent.height == 0)
            return false;
        // Check the surface while the current swapchain still exists, so these failures leave it intact.
        const auto selected = surface_format();
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error("Surface cannot be a color attachment");
        // A pending request, to a file or into memory; completing or failing it clears both.
        const bool capture = capture_to_memory || !options.capture.empty();
        if (capture && (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                        (selected.format != VK_FORMAT_B8G8R8A8_SRGB && selected.format != VK_FORMAT_R8G8B8A8_SRGB &&
                         selected.format != VK_FORMAT_B8G8R8A8_UNORM && selected.format != VK_FORMAT_R8G8B8A8_UNORM))) {
            // The request fails, not the renderer; later draws go on without it.
            options.capture.clear();
            capture_to_memory = false;
            throw std::runtime_error("Capture requires a transferable BGRA/RGBA8 swapchain");
        }
        const auto mode = supported_present_mode();
        const auto count = image_count(caps, mode);
        check(vkDeviceWaitIdle(device), "Wait before swapchain recreation");
        wait_for_presentation();
        destroy_swapchain();
        // Nothing can be presented until the replacement is complete, so any failure from here on is fatal.
        try {
            create_swapchain(caps, selected, selected_extent, capture, mode, count);
        } catch (const std::exception &error) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        resize = false;
        ++stats.swapchain_generations;
        std::cout << "Swapchain " << stats.swapchain_generations << ": " << extent.width << 'x' << extent.height
                  << " pixels, " << images.size() << " images, " << present_mode_name(mode) << " presentation\n";
        return true;
    }
    void create_swapchain(const VkSurfaceCapabilitiesKHR &caps, VkSurfaceFormatKHR selected, VkExtent2D selected_extent,
                          bool capture, PresentMode mode, std::uint32_t count) {
        extent = selected_extent;
        format = selected.format;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto option : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & option) {
                alpha = option;
                break;
            }
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = count;
        info.imageFormat = format;
        info.imageColorSpace = selected.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        const std::uint32_t families[]{graphics_family, present_family};
        info.imageSharingMode =
            graphics_family == present_family ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT) {
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        }
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = vulkan_present_mode(mode);
        info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create swapchain");
        presenting = mode;
        const auto handles = enumerate<VkImage>(
            [&](auto *n, auto *p) { return vkGetSwapchainImagesKHR(device, swapchain, n, p); }, "Get swapchain images");
        images.resize(handles.size());
        choose_depth_format();
#ifndef ANIMA_HAS_ASSETS
        // The view renders into the swapchain images, with a depth buffer of their size.
        scene_extent = extent;
        depth_target = create_depth(scene_extent);
#endif
        create_render_pass();
#ifdef ANIMA_HAS_ASSETS
        // The scene targets are left to draw()'s ensure_scene_targets(), where a failure to create them is recoverable.
        present_pass = create_display_pass();
        create_display_pipeline();
#endif
        create_pipeline(PipelineKind::diagnostic, pipeline);
#ifdef ANIMA_HAS_ASSETS
        for (const bool height_fog : {false, true}) {
            for (std::size_t mesh = 0; mesh < mesh_pipeline_count; ++mesh)
                create_pipeline(MeshPipeline(mesh) == MeshPipeline::discarding ? PipelineKind::resource
                                                                               : PipelineKind::opaque_resource,
                                resource_pipelines[mesh][height_fog], height_fog, mesh_pipeline_culling[mesh]);
            create_pipeline(PipelineKind::blended_resource, blended_resource_pipelines[height_fog], height_fog);
            create_pipeline(PipelineKind::impostor, impostor_pipelines[height_fog], height_fog);
        }
        create_pipeline(PipelineKind::sky, sky_pipeline);
#endif
        for (std::size_t i = 0; i < images.size(); ++i) {
            auto &image = images[i];
            image.image = handles[i];
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &image.view), "Create image view");
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
#ifdef ANIMA_HAS_ASSETS
            // The display pass writes the swapchain image alone; the view's depth belongs to the scene targets.
            framebuffer.renderPass = present_pass;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &image.view;
#else
            framebuffer.renderPass = render_pass;
            const VkImageView attachments[]{image.view, depth_target->view};
            framebuffer.attachmentCount = 2;
            framebuffer.pAttachments = attachments;
#endif
            framebuffer.width = extent.width;
            framebuffer.height = extent.height;
            framebuffer.layers = 1;
            check(vkCreateFramebuffer(device, &framebuffer, nullptr, &image.framebuffer), "Create framebuffer");
            // Exercise cleanup with the first image complete and the remaining images uninitialized.
            if (i == 0)
                fail_after(RendererFailureStage::swapchain);
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device, &semaphore, nullptr, &image.rendered), "Create present semaphore");
            if (present_fences) {
                VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                check(vkCreateFence(device, &fence, nullptr, &image.presented), "Create present fence");
            }
        }
        if (capture)
            create_capture();
    }
    void create_render_pass() {
        VkAttachmentDescription attachment{};
        attachment.format = format;
#ifdef ANIMA_HAS_ASSETS
        attachment.format = world_color_format;
#endif
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#ifdef ANIMA_HAS_ASSETS
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#endif
        VkAttachmentDescription depth{};
        depth.format = depth_format;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentDescription attachments[]{attachment, depth};
        VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        subpass.pDepthStencilAttachment = &depth_reference;
        // The pass's layout transitions and clears follow earlier attachment work, whose depth writes become
        // available. The first scope also holds the stages logically earlier than these, so where the pass draws the
        // world target, they follow the fragment shaders that sampled its color in earlier display passes, a write
        // after a read that needs only this execution dependency.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        std::array<VkSubpassDependency, 2> dependencies{dependency, {}};
        auto &outgoing = dependencies[1];
        outgoing.srcSubpass = 0;
        outgoing.dstSubpass = VK_SUBPASS_EXTERNAL;
        outgoing.srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        outgoing.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        outgoing.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        outgoing.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 2;
        pass.pDependencies = dependencies.data();
        check(vkCreateRenderPass(device, &pass, nullptr, &render_pass), "Create render pass");
#ifdef ANIMA_HAS_ASSETS
        create_opaque_input_passes(pass);
#endif
    }
    // resource draws the view's opaque and masked meshes with mesh.frag's discards, and opaque_resource draws opaque
    // ones without them (MeshPipeline). blended_resource draws meshes as resource does, but composites premultiplied
    // color over the target and writes no depth. shadow_opaque and shadow_masked draw meshes' opaque and masked casters
    // into the shadow maps, shadow_opaque without a fragment shader. impostor and shadow_impostor draw impostor meshes
    // (Mesh::impostor()) in the view and the shadow maps.
    enum class PipelineKind {
        diagnostic,
        ui,
        resource,
        opaque_resource,
        blended_resource,
        sky,
        shadow_opaque,
        shadow_masked,
        impostor,
        shadow_impostor
    };
    // Creates the pipeline of @p mode in @p output, culling the faces that @p cull_mode names; one that draws the view
    // with mesh.frag or impostor.frag compiles the height fog's code only with @p height_fog.
    void create_pipeline(PipelineKind mode, VkPipeline &output, [[maybe_unused]] bool height_fog = true,
                         VkCullModeFlags cull_mode = VK_CULL_MODE_NONE) {
        const bool impostor = mode == PipelineKind::impostor || mode == PipelineKind::shadow_impostor;
        const bool shadow = mode == PipelineKind::shadow_opaque || mode == PipelineKind::shadow_masked ||
                            mode == PipelineKind::shadow_impostor;
        const bool blended = mode == PipelineKind::blended_resource;
        // The view's mesh pipelines, which draw with mesh.frag.
        [[maybe_unused]] const bool view_mesh =
            mode == PipelineKind::resource || mode == PipelineKind::opaque_resource || blended;
#ifdef ANIMA_HAS_ASSETS
        const bool resource = view_mesh || shadow || impostor;
#endif
        const bool ui = mode == PipelineKind::ui, sky = mode == PipelineKind::sky;
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (auto &stage : stages) {
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = ui ? ui_vertex_shader : vertex_shader;
#ifdef ANIMA_HAS_ASSETS
        if (resource)
            stages[0].module = resource_vertex_shader;
#endif
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ui ? ui_fragment_shader : fragment_shader;
#ifdef ANIMA_HAS_ASSETS
        if (resource)
            stages[1].module = mesh_fragment_shader;
        // Sets impostor.vert's constant 0, which selects the shadow passes' view.
        const VkBool32 enabled = VK_TRUE;
        const VkSpecializationMapEntry constant_entry{0, 0, sizeof(enabled)};
        const VkSpecializationInfo constant_enabled{1, &constant_entry, sizeof(enabled), &enabled};
        // The view's fragment constants: mesh.frag's 0 selects premultiplied output, 1 (environment.glsl) the height
        // fog's code, in mesh.frag and impostor.frag, and mesh.frag's 2 keeps its discards.
        const std::array<VkBool32, 3> fragment_constants{blended ? VK_TRUE : VK_FALSE, height_fog ? VK_TRUE : VK_FALSE,
                                                         mode == PipelineKind::opaque_resource ? VK_FALSE : VK_TRUE};
        const std::array<VkSpecializationMapEntry, 3> fragment_entries{{{0, 0, sizeof(VkBool32)},
                                                                        {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                                        {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}}};
        const VkSpecializationInfo fragment_specialization{static_cast<std::uint32_t>(fragment_entries.size()),
                                                           fragment_entries.data(), sizeof(fragment_constants),
                                                           fragment_constants.data()};
        if (view_mesh || mode == PipelineKind::impostor)
            stages[1].pSpecializationInfo = &fragment_specialization;
        if (sky) {
            stages[0].module = sky_vertex_shader;
            stages[1].module = sky_fragment_shader;
        }
        if (shadow) {
            stages[0].module = shadow_resource_vertex_shader;
            stages[1].module = shadow_fragment_shader;
        }
        if (impostor) {
            stages[0].module = impostor_vertex_shader;
            stages[1].module = shadow ? impostor_shadow_fragment_shader : impostor_fragment_shader;
            if (shadow)
                stages[0].pSpecializationInfo = &constant_enabled;
        }
#endif
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
#ifdef ANIMA_HAS_ASSETS
        // Mesh vertices from binding 0 and placement rows from binding 1.
        std::array<VkVertexInputAttributeDescription, resource_attributes.size() + placement_attributes.size()>
            placed_attributes{};
        std::copy(resource_attributes.begin(), resource_attributes.end(), placed_attributes.begin());
        std::copy(placement_attributes.begin(), placement_attributes.end(),
                  placed_attributes.begin() + resource_attributes.size());
        if (resource) {
            vertex.vertexBindingDescriptionCount = static_cast<std::uint32_t>(resource_bindings.size());
            vertex.pVertexBindingDescriptions = resource_bindings.data();
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(placed_attributes.size());
            vertex.pVertexAttributeDescriptions = placed_attributes.data();
        }
        const VkVertexInputAttributeDescription shadow_resource_attributes[]{
            resource_attributes[0], resource_attributes[3],  resource_attributes[4],  resource_attributes[5],
            resource_attributes[7], placement_attributes[0], placement_attributes[1], placement_attributes[2]};
        if (shadow) {
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(shadow_resource_attributes));
            vertex.pVertexAttributeDescriptions = shadow_resource_attributes;
        }
        // An impostor's corner is its texture coordinate alone.
        const VkVertexInputAttributeDescription impostor_attributes[]{resource_attributes[3], placement_attributes[0],
                                                                      placement_attributes[1], placement_attributes[2]};
        if (impostor) {
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(impostor_attributes));
            vertex.pVertexAttributeDescriptions = impostor_attributes;
        }
#endif
#ifdef ANIMA_UI
        const VkVertexInputBindingDescription ui_binding{0, sizeof(detail::UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription ui_attributes[]{
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(detail::UiVertex, x)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(detail::UiVertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(detail::UiVertex, u)}};
        if (ui) {
            vertex.vertexBindingDescriptionCount = 1;
            vertex.pVertexBindingDescriptions = &ui_binding;
            vertex.vertexAttributeDescriptionCount = 3;
            vertex.pVertexAttributeDescriptions = ui_attributes;
        }
#endif
        VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // The diagnostic triangle is a screen overlay, not part of the view.
        depth_state.depthTestEnable = !ui && mode != PipelineKind::diagnostic;
        // Blended surfaces are hidden by nearer opaque and masked ones, and hide nothing themselves.
        depth_state.depthWriteEnable = !ui && !sky && !blended;
        // The view uses reversed depth, where nearer surfaces have greater depth and the sky lies at the far
        // plane, depth 0. Shadow passes are orthographic, where depth is linear in distance, and keep forward depth.
        depth_state.depthCompareOp = shadow ? VK_COMPARE_OP_LESS
                                     : sky  ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                            : VK_COMPARE_OP_GREATER;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = cull_mode;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // UI and blended meshes composite premultiplied linear color with the "over" operator.
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = ui || blended;
        blend_attachment.srcColorBlendFactor = blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor = blend_attachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = shadow ? 0 : 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        // Without a fragment shader, rasterization still writes each covered texel's depth.
        info.stageCount = mode == PipelineKind::shadow_opaque ? 1 : 2;
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDepthStencilState = &depth_state;
        info.pDynamicState = &dynamic;
        info.layout = ui ? ui_pipeline_layout : pipeline_layout;
#ifdef ANIMA_HAS_ASSETS
        if (resource)
            info.layout = resource_pipeline_layout;
        if (sky)
            info.layout = environment_pipeline_layout;
#endif
        info.renderPass = render_pass;
#ifdef ANIMA_HAS_ASSETS
        if (shadow)
            info.renderPass = shadow_pass;
        if (ui)
            info.renderPass = present_pass;
#endif
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &output),
              "Create graphics pipeline");
    }
    // Creates @p info's image in device-local memory from the allocator, bound, and returns the size of its
    // allocation. Throws `std::runtime_error` naming @p action when creation, allocation or binding fails.
    VkDeviceSize create_image(const VkImageCreateInfo &info, VkImage &image, VmaAllocation &allocation,
                              const char *action) const {
        VmaAllocationCreateInfo placement{};
        placement.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VmaAllocationInfo allocated{};
        check(vmaCreateImage(allocator, &info, &placement, &image, &allocation, &allocated), action);
        return allocated.size;
    }
    // Creates @p info's buffer in memory with the @p required properties, preferring @p preferred ones, bound,
    // and returns its allocation's size and, for host-visible memory, its mapping, which lasts until the buffer
    // is destroyed. Throws `std::runtime_error` naming @p action when creation, allocation or binding fails.
    VmaAllocationInfo create_buffer(const VkBufferCreateInfo &info, VkMemoryPropertyFlags required, VkBuffer &buffer,
                                    VmaAllocation &allocation, const char *action,
                                    VkMemoryPropertyFlags preferred = 0) const {
        VmaAllocationCreateInfo placement{};
        placement.requiredFlags = required;
        placement.preferredFlags = preferred;
        if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            placement.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocated{};
        check(vmaCreateBuffer(allocator, &info, &placement, &buffer, &allocation, &allocated), action);
        return allocated;
    }
    void choose_depth_format() {
        depth_format = VK_FORMAT_UNDEFINED;
        // Reversed depth keeps its precision only in the floating-point format, and X8_D24 keeps more than D16. Each
        // holds depth alone, so the depth aspect and the clear to 0 suit all three. Custom materials that read opaque
        // depth sample a copy of the depth image, so the format must also be sampled and copied. Vulkan requires
        // sampling, and with it copying, of D16 and the floating-point format but not of X8_D24, and attachment of D16
        // and of at least one of the others, so D16 always qualifies.
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        for (auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, candidate, &properties);
            if ((properties.optimalTilingFeatures & needed) == needed) {
                depth_format = candidate;
                break;
            }
        }
        if (depth_format == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("No depth attachment format");
    }
    // Creates a depth attachment of @p size in depth_format. Throws `std::runtime_error` when a Vulkan call fails,
    // releasing what it created.
    std::unique_ptr<DepthTarget> create_depth(VkExtent2D size) const {
        auto target = std::make_unique<DepthTarget>();
        target->device = device;
        target->allocator = allocator;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = depth_format;
        image.extent = {size.width, size.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
#ifdef ANIMA_HAS_ASSETS
        // Custom materials that read opaque depth sample a copy of this image.
        if (opaque_copies_supported())
            image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        target->allocation_bytes = create_image(image, target->image, target->allocation, "Create depth image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = target->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = depth_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &target->view), "Create depth view");
        return target;
    }
#ifdef ANIMA_HAS_ASSETS
    std::shared_ptr<GpuSampler> material_sampler(const Sampler &source, std::uint32_t levels) {
        const SamplerKey key{source.mag, source.min, source.mip, source.u, source.v, source.mipmapped, levels};
        for (auto it = material_samplers.begin(); it != material_samplers.end();) {
            if (it->second.expired())
                it = material_samplers.erase(it);
            else
                ++it;
        }
        if (const auto found = material_samplers.find(key); found != material_samplers.end())
            if (auto sampler = found->second.lock())
                return sampler;
        const auto wrap = [](Wrap mode) {
            if (mode == Wrap::clamp)
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (mode == Wrap::mirror)
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = source.mag == Filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter = source.min == Filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.mipmapMode = !source.mipmapped || source.mip == Filter::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                                             : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = wrap(source.u);
        info.addressModeV = wrap(source.v);
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // Without mipmaps, keep glTF's minification filter while sampling level 0 only. The Vulkan specification's
        // VkSamplerCreateInfo note emulates OpenGL's GL_NEAREST and GL_LINEAR minification this way: with
        // VK_SAMPLER_MIPMAP_MODE_NEAREST, minLod 0 and maxLod 0.25, the level of detail can still be positive, so
        // minFilter applies, while mip selection always rounds down to the base level. A maximum of zero would
        // always magnify, applying magFilter instead.
        constexpr float unmipmapped_max_lod = .25F;
        info.maxLod = source.mipmapped ? static_cast<float>(levels - 1) : unmipmapped_max_lod;
        // Anisotropic filtering samples a finer mip level several times along the footprint's long axis. Vulkan
        // leaves the scheme to the implementation, including how it combines with nearest filters, so only linear,
        // mipmapped samplers use it and nearest and unmipmapped ones keep glTF's exact filters.
        const bool anisotropic =
            anisotropy > 1 && source.mag == Filter::linear && source.min == Filter::linear && source.mipmapped;
        info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = anisotropic ? anisotropy : 1;
        auto sampler = std::make_shared<GpuSampler>();
        sampler->device = device;
        check(vkCreateSampler(device, &info, nullptr, &sampler->handle), "Create material sampler");
        material_samplers[key] = sampler;
        return sampler;
    }
    // Throws `std::runtime_error` unless the device samples 8-bit RGBA color and data images with linear filtering.
    void require_texture_formats() const {
        const auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (const auto image_format : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, image_format, &properties);
            if ((properties.optimalTilingFeatures & needed) != needed)
                throw std::runtime_error("GPU lacks filtered RGBA colour/data images");
        }
    }
    // One level of texels to copy into an image: its extent and its bytes in the image's format.
    struct TexelLevel {
        std::uint32_t width{}, height{};
        std::span<const std::uint8_t> bytes;
    };
    // Creates @p texture's image in @p image_format with one mip level per entry of @p levels, the first at the image's
    // extent, its view and the shared sampler for @p sampling, then copies @p levels through @p upload and waits for
    // the copy. The first texture of an upload, @p first, fires the texture and upload stages of @p failure.
    void upload_texture(GpuTexture &texture, const Sampler &sampling, VkFormat image_format,
                        std::span<const TexelLevel> levels, UploadBatch &upload, RendererFailureStage failure,
                        bool initial, bool first) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = image_format;
        image.extent = {levels.front().width, levels.front().height, 1};
        image.mipLevels = static_cast<std::uint32_t>(levels.size());
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        texture.allocation_bytes = create_image(image, texture.image, texture.allocation, "Create texture image");
        if (first)
            inject_scene(failure, RendererFailureStage::texture, initial);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mipLevels, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create texture view");
        texture.shared_sampler = material_sampler(sampling, image.mipLevels);
        texture.sampler = texture.shared_sampler->handle;
        VkDeviceSize size = 0;
        for (const auto &level : levels)
            size += level.bytes.size();
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto *mapped =
            static_cast<char *>(create_buffer(buffer, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, upload.staging_buffer,
                                              upload.staging_allocation, "Create texture staging buffer")
                                    .pMappedData);
        // Levels are packed back to back. Each size is a multiple of the format's texel block, 4 bytes for RGBA8 and
        // 16 for BC7, so every copy starts where Vulkan requires.
        std::vector<VkBufferImageCopy> copies;
        std::size_t offset = 0;
        for (std::uint32_t level = 0; level < image.mipLevels; ++level) {
            const auto &source = levels[level];
            std::memcpy(mapped + offset, source.bytes.data(), source.bytes.size());
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            copy.imageExtent = {source.width, source.height, 1};
            copies.push_back(copy);
            offset += source.bytes.size();
        }
        check(vmaFlushAllocation(allocator, upload.staging_allocation, 0, VK_WHOLE_SIZE), "Flush texture staging");
        check(vkResetCommandBuffer(upload.command, 0), "Reset texture upload command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(upload.command, &begin), "Begin texture upload");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = view.subresourceRange;
        vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkCmdCopyBufferToImage(upload.command, upload.staging_buffer, texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()),
                               copies.data());
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(upload.command), "End texture upload");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &upload.command;
        check(vkResetFences(device, 1, &upload.fence), "Reset upload fence");
        check(vkQueueSubmit(graphics_queue, 1, &submit, upload.fence), "Submit texture upload");
        upload.pending = true;
        if (first) {
            inject_scene(failure, RendererFailureStage::texture_upload, initial);
            if (failure == RendererFailureStage::upload_timeout)
                throw VulkanFailure(VK_TIMEOUT, "Injected upload timeout");
            if (failure == RendererFailureStage::device_lost) {
                upload.simulate_device_loss = true;
                throw std::runtime_error("Injected failure before device loss at upload retirement");
            }
        }
        upload.wait();
        upload.release_staging();
    }
    // Uploads @p source's image into @p texture, sampled as @p source says, and returns its mip level count.
    //
    // An RGBA8 image uploads @p prepared when given, or else its mip chain built with @p mip_options, or its base level
    // alone when @p source is not mipmapped. A BC7 image uploads the levels it stores, or its base level alone when
    // @p source is not mipmapped: as BC7 where the device samples it, and otherwise decoded to RGBA8. One stored level
    // samples as an unmipmapped texture does. The image must have texels unless @p prepared supplies them.
    std::uint32_t upload_image(GpuTexture &texture, const Texture &source, const std::vector<MipLevel> *prepared,
                               const TextureMipOptions &mip_options, UploadBatch &upload, RendererFailureStage failure,
                               bool initial, bool first) {
        const auto &pixels = *source.image;
        const bool srgb = source.encoding == TextureEncoding::srgb;
        auto sampling = source.sampler;
        auto image_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        std::vector<TexelLevel> levels;
        // RGBA8 levels built here, which levels refers to.
        std::vector<MipLevel> built;
        if (pixels.format == ImageFormat::bc7) {
            const auto count = sampling.mipmapped ? pixels.levels : 1U;
            if (count == 1)
                sampling.mipmapped = false;
            if (bc7_sampled) {
                image_format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
                std::size_t offset = 0;
                for (std::uint32_t level = 0; level < count; ++level) {
                    const auto width = std::max(pixels.width >> level, 1U),
                               height = std::max(pixels.height >> level, 1U);
                    // Image::blocks: ceil(w / 4) * ceil(h / 4) blocks of 16 bytes per level.
                    const auto bytes = (std::size_t{width} + 3) / 4 * ((std::size_t{height} + 3) / 4) * 16;
                    levels.push_back({width, height, std::span(pixels.blocks).subspan(offset, bytes)});
                    offset += bytes;
                }
            } else
                built = decode_image(pixels, count);
        } else if (prepared)
            for (const auto &mip : *prepared)
                levels.push_back({mip.width, mip.height, mip.rgba});
        else if (sampling.mipmapped)
            built = texture_mips(source, mip_options);
        else
            levels.push_back({pixels.width, pixels.height, pixels.rgba});
        for (const auto &mip : built)
            levels.push_back({mip.width, mip.height, mip.rgba});
        upload_texture(texture, sampling, image_format, levels, upload, failure, initial, first);
        return static_cast<std::uint32_t>(levels.size());
    }
    // Uploads the images of @p target's material plan, or of @p prepared's, and writes its material descriptors.
    // Without @p prepared, @p texels holds the image of each source texture with its texels (Mesh::texel_images).
    void upload_textures(GpuMaterials &target, UploadBatch &upload, RendererFailureStage failure, bool initial,
                         std::span<const std::shared_ptr<const anima::Image>> texels,
                         const MeshPreparation *prepared = nullptr) {
        require_texture_formats();
        const auto generated_plan = prepared
                                        ? MaterialTexturePlan{}
                                        : material_texture_plan(target.source->material_data, target.source->textures);
        const auto &plan = prepared ? prepared->plan() : generated_plan;
        target.textures.resize(plan.images.size());
        std::uint32_t total_mips = 0;
        const Texture white{std::make_shared<anima::Image>(anima::Image{1, 1, {255, 255, 255, 255}}), {}};
        for (std::size_t i = 0; i < target.textures.size(); ++i) {
            const auto &planned = plan.images[i];
            // The texture's sampler and encoding, with the image that holds its texels.
            auto source = white;
            const std::vector<MipLevel> *mips = nullptr;
            if (planned.source >= 0)
                source = target.source->textures[planned.source];
            if (prepared) {
                // A preparation filtered RGBA8 images already and holds the block-compressed ones.
                if (const auto &image = prepared->compressed_images().at(i))
                    source.image = image;
                else
                    mips = &prepared->images().at(i);
            } else if (planned.source >= 0)
                source.image = texels[planned.source];
            total_mips +=
                upload_image(target.textures[i], source, mips, planned.mips, upload, failure, initial, i == 0);
        }
        target.material_sets.resize(target.source->material_data.size() + 1);
        const auto count = static_cast<std::uint32_t>(target.material_sets.size());
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * material_texture_count},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = count;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pool, nullptr, &target.texture_pool), "Create material descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(count, texture_layout);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = target.texture_pool;
        allocate.descriptorSetCount = count;
        allocate.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(device, &allocate, target.material_sets.data()),
              "Allocate material descriptors");
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(physical, &device_properties);
        const auto align = device_properties.limits.minUniformBufferOffsetAlignment;
        const auto stride = (sizeof(MaterialUniform) + align - 1) / align * align;
        VkBufferCreateInfo buffer_create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_create.size = stride * count;
        buffer_create.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        buffer_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        void *const material_mapping =
            create_buffer(buffer_create, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, target.material_buffer,
                          target.material_allocation, "Create material buffer")
                .pMappedData;
        std::vector<std::byte> uniforms(static_cast<std::size_t>(stride * count));
        for (std::uint32_t i = 0; i < count; ++i) {
            const Material fallback;
            const auto &m = i == 0 ? fallback : target.source->material_data[i - 1];
            const MaterialUniform uniform{{m.emissive.x, m.emissive.y, m.emissive.z, m.alpha},
                                          {m.normal_scale, m.alpha_mode == AlphaMode::mask ? m.alpha_cutoff : -1.F,
                                           m.occlusion_strength, m.unlit ? 1.F : 0.F},
                                          {m.normal_texture >= 0 ? 1.F : 0.F, m.double_sided ? 1.F : 0.F, 0, 0}};
            std::memcpy(uniforms.data() + stride * i, &uniform, sizeof(uniform));
            std::array<VkDescriptorImageInfo, material_texture_count> image_infos{};
            std::array<VkWriteDescriptorSet, material_texture_count + 1> writes{};
            for (unsigned j = 0; j < material_texture_count; ++j) {
                const auto &texture = target.textures.at(plan.bindings[i][j]);
                image_infos[j] = {texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[j].dstSet = target.material_sets[i];
                writes[j].dstBinding = j;
                writes[j].descriptorCount = 1;
                writes[j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[j].pImageInfo = &image_infos[j];
            }
            const VkDescriptorBufferInfo buffer_info{target.material_buffer, stride * i, sizeof(MaterialUniform)};
            writes[material_texture_count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[material_texture_count].dstSet = target.material_sets[i];
            writes[material_texture_count].dstBinding = material_texture_count;
            writes[material_texture_count].descriptorCount = 1;
            writes[material_texture_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[material_texture_count].pBufferInfo = &buffer_info;
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        std::memcpy(material_mapping, uniforms.data(), uniforms.size());
        check(vmaFlushAllocation(allocator, target.material_allocation, 0, VK_WHOLE_SIZE), "Flush material memory");
        inject_scene(failure, RendererFailureStage::descriptors, initial);
        std::cout << "GPU textures=" << target.textures.size() << ", mip_levels=" << total_mips
                  << ", material_descriptors=" << target.material_sets.size() << '\n';
    }
#endif
    void create_capture() {
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Coherent memory is preferred; save_capture invalidates any other kind before reading it.
        capture_mapping = create_buffer(buffer, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, capture_buffer, capture_allocation,
                                        "Create readback buffer", VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                              .pMappedData;
    }
    // Reads back the latest submitted frame, which copied its image into the capture buffer, once it has finished.
    void save_capture() {
        // Consume the request first, so a file that cannot be written is reported by one draw, not every draw.
        const auto path = std::exchange(options.capture, {});
        const bool to_memory = std::exchange(capture_to_memory, false);
        check(vkWaitForFences(device, 1, &frames[latest_frame].fence, VK_TRUE, fence_timeout), "Wait for capture");
        check(vmaInvalidateAllocation(allocator, capture_allocation, 0, VK_WHOLE_SIZE), "Invalidate readback memory");
        const auto *bytes = static_cast<const std::uint8_t *>(capture_mapping);
        const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        const auto count = static_cast<std::size_t>(extent.width) * extent.height;
        std::vector<std::uint8_t> rgb(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            rgb[i * 3] = bytes[i * 4 + (bgra ? 2 : 0)];
            rgb[i * 3 + 1] = bytes[i * 4 + 1];
            rgb[i * 3 + 2] = bytes[i * 4 + (bgra ? 0 : 2)];
        }
        if (to_memory)
            captured_image = CapturedImage{extent.width, extent.height, std::move(rgb)};
        else {
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            if (!output)
                throw std::runtime_error("Cannot open capture output: " + path.string());
            output << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
            output.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
            output.close();
            if (!output)
                throw std::runtime_error("Failed writing capture");
        }
        stats.captured = true;
        ++stats.capture_count;
    }
#ifdef ANIMA_UI
#include "ui_renderer.inc"
#endif
    // The resources of one frame in flight. draw() uses the RendererOptions::frames_in_flight slots in turn and waits
    // for a slot's fence before it rewrites any of them, since until then the frame that the slot submitted last can
    // still read them. Everything else that frames use is shared: images that frames write, such as the shadow maps,
    // the scene targets and the atmosphere's tables, are ordered between frames by the barriers of each frame's
    // commands, and shared objects that are replaced or released wait in a release list (release_after_frames()).
    struct FrameSlot {
        VkCommandPool pool{};
        VkCommandBuffer command{};
        // Signaled when the frame that the slot submitted last has finished, and at creation.
        VkFence fence{};
        // Signaled by the image acquisition that the slot's frame waits for.
        VkSemaphore acquired{};
        // Whether the slot submitted a frame whose fence has not been waited for since.
        bool in_flight{};
        // Objects retired while the slot's frame was the latest submitted, destroyed once its fence has been waited
        // for.
        std::vector<std::shared_ptr<void>> released;
        // The slot's first timing query, whether its frame wrote timestamps that have not been read, and when that
        // frame was submitted on the host's clock, before which none of its timestamps can be written.
        std::uint32_t first_query{};
        bool timing_pending{};
        std::chrono::steady_clock::time_point timing_submitted;
#ifdef ANIMA_HAS_ASSETS
        // The palettes of the slot's frame, which pose_set points at.
        std::unique_ptr<ResourceBuffer> pose_buffer;
        VkDescriptorSet pose_set{};
        // The environment block, which environment_set points at with the shadow maps of shadow_generation and the
        // atmosphere's tables.
        std::unique_ptr<ResourceBuffer> environment_buffer;
        VkDescriptorSet environment_set{};
        std::uint64_t shadow_generation{};
        // The custom materials' frame block, which custom_frame_set points at with the opaque input copies of
        // opaque_generation.
        std::unique_ptr<ResourceBuffer> custom_frame_buffer;
        VkDescriptorSet custom_frame_set{};
        std::uint64_t opaque_generation{};
#endif
#ifdef ANIMA_UI
        // The UI vertices of the slot's frame.
        std::unique_ptr<UiBuffer> ui_buffer;
#endif
    };
    std::vector<FrameSlot> frames;
    // The slot that draw() prepares and records next, and the slot of the latest submitted frame.
    std::size_t frame_index{}, latest_frame{};
    FrameSlot &frame() noexcept { return frames[frame_index]; }
    // The present ids of the latest RendererOptions::frames_in_flight + 1 submissions, which draw() uses in turn, as it
    // uses the frame slots: each holds the id of its submission's present while a later frame is to wait for that
    // present, otherwise 0. The submission that the next draw() makes reuses the entry of the submission one before
    // the one whose slot it reuses, and waits for that present first; see wait_for_queued_present().
    std::array<std::uint64_t, max_frames_in_flight + 1> present_ids{};
    // The entry of present_ids that the next submission uses.
    std::size_t present_index{};
    // Destroys @p retired, a pointer to an object that submitted frames may still use, once none can. Waiting for the
    // fence of the latest submitted frame covers every earlier submission too, since a fence signal operation includes
    // every command submitted to the queue before it, so @p retired waits in that frame's release list until draw() or
    // set_scenes() waits for its fence, or is destroyed now when that wait has already happened.
    template <class Retired> void release_after_frames(Retired retired) {
        if (auto &latest = frames[latest_frame]; retired && latest.in_flight)
            latest.released.emplace_back(std::move(retired));
    }
    // Waits for the frame that @p slot submitted last; a failure names @p operation.
    void wait_for_slot(FrameSlot &slot, const char *operation) {
        check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, fence_timeout), operation);
        slot.in_flight = false;
    }
    // Resets the commands of @p slot, whose frame has finished, and destroys what was retired while that frame was the
    // latest submitted.
    void recycle_slot(FrameSlot &slot) {
        check(vkResetCommandPool(device, slot.pool, 0), "Reset frame commands");
        slot.released.clear();
    }
    // Waits for every frame in flight and recycles every slot.
    void wait_for_frames(const char *operation) {
        for (auto &slot : frames) {
            wait_for_slot(slot, operation);
            recycle_slot(slot);
        }
    }
    // Whether frames wait for the presents of earlier ones: VK_KHR_present_wait is enabled and the swapchain's mode
    // queues every present for a vertical blank, so that a frame's inputs can otherwise be read while several frames
    // wait for display ahead of it. In the other modes no present waits behind another: immediate shows each at once
    // and mailbox replaces the one that waits, so the wait would bound no queue, and in mailbox it would hold frames
    // to the display's rate.
    [[nodiscard]] bool awaits_presents() const noexcept {
        return present_waits && (presenting == PresentMode::fifo || presenting == PresentMode::fifo_relaxed);
    }
    // Waits, for at most present_wait_timeout, until vkWaitForPresentKHR reports the present whose id the next
    // submission's entry of present_ids holds: that of the frame submitted frames_in_flight + 1 submissions earlier,
    // when draw() gave its present an id for this wait. Vulkan asks that a present be reported as close as possible to
    // its display; MoltenVK 1.4.1 reports it when the command buffer that presents it completes. Waiting for the
    // frame's present rather than for that of the frame whose slot the next submission reuses lets a frame start while
    // the frame before it waits for a vertical blank. The id is cleared, so a second call returns at once. A wait that
    // times out is not an error, since it only paces frames; an out-of-date or suboptimal swapchain is recreated by the
    // next draw().
    void wait_for_queued_present() {
        const auto id = std::exchange(present_ids[present_index], 0);
        if (id == 0)
            return;
        ++stats.present_waits;
        const auto result = wait_for_present(device, swapchain, id, present_wait_timeout);
        if (result == VK_TIMEOUT)
            ++stats.present_wait_timeouts;
        else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            resize = true;
        else
            check(result, "Wait for present");
    }
    // Waits for the frame that the next draw() waits for: the one that the slot it records next submitted last, and
    // then for the present that wait_for_queued_present() names. The slot's fence stays signaled until draw() resets it
    // just before its submission, and the present id is cleared, so draw()'s own waits then return at once.
    void wait_for_frame() {
        running();
        wait_for_slot(frame(), "Wait for frame");
        wait_for_queued_present();
    }
    // Reads the display times that VK_GOOGLE_display_timing reported since the previous read, and sets
    // FrameProfile::present_interval_ms from the latest frame that was displayed after the frame presented just before
    // it.
    void read_display_times() {
        past_timings.clear();
        for (;;) {
            const auto offset = past_timings.size();
            past_timings.resize(offset + display_time_batch);
            auto count = display_time_batch;
            const auto result = past_presentation_timing(device, swapchain, &count, past_timings.data() + offset);
            past_timings.resize(offset + (result == VK_SUCCESS || result == VK_INCOMPLETE ? count : 0));
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                resize = true;
                break;
            }
            if (result != VK_INCOMPLETE) {
                check(result, "Read display times");
                break;
            }
        }
        // Ids increase by one per present and wrap at 2^32, so in a span shorter than half of that the signed
        // difference orders them.
        const auto earlier = [](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; };
        std::sort(past_timings.begin(), past_timings.end(),
                  [&](const auto &a, const auto &b) { return earlier(a.presentID, b.presentID); });
        for (const auto &timing : past_timings) {
            // Vulkan does not define a display time of zero, so one is skipped rather than paired with a real time.
            if (timing.actualPresentTime == 0)
                continue;
            if (latest_display && timing.presentID - latest_display->id == 1 &&
                timing.actualPresentTime >= latest_display->time) {
                const std::chrono::duration<double, std::nano> interval(
                    double(timing.actualPresentTime - latest_display->time));
                profile.present_interval_ms = std::chrono::duration<double, std::milli>(interval).count();
            }
            if (!latest_display || earlier(latest_display->id, timing.presentID))
                latest_display = DisplayedFrame{timing.presentID, timing.actualPresentTime};
        }
    }
    bool draw(const detail::UiFrame *ui_frame = nullptr) {
        running();
        using Clock = std::chrono::steady_clock;
        profile = {};
        auto marked = options.profile ? Clock::now() : Clock::time_point{};
        const auto measure = [&](double &duration) {
            if (options.profile) {
                const auto now = Clock::now();
                duration = std::chrono::duration<double, std::milli>(now - marked).count();
                marked = now;
            }
        };
        VkExtent2D pixels{};
        if (!drawable(pixels))
            return false;
        // UI layout can change before a resize notification reaches the renderer.
        // Its extent guard below skips acquisition, so Vulkan's out-of-date result
        // cannot recover a stale swapchain. Reconcile the actual surface here.
        // During an asynchronous transition SDL can lag behind the surface: only
        // recreate if the surface changed, not on every mismatched UI frame.
        if (!resize && swapchain && (pixels.width != extent.width || pixels.height != extent.height)) {
            const auto current = surface_extent(pixels, surface_capabilities());
            resize = current.width != extent.width || current.height != extent.height;
        }
        if (resize || !swapchain) {
            if (!recreate(pixels))
                return false;
        }
        auto &slot = frame();
        wait_for_slot(slot, "Wait for frame");
        wait_for_queued_present();
        measure(profile.fence_wait_ms);
        recycle_slot(slot);
        if (display_timing)
            read_display_times();
#ifdef ANIMA_HAS_ASSETS
        retire_resources();
        try {
            // Before the custom materials' frame block, which holds the scene targets' size.
            ensure_scene_targets();
            ensure_shadow_targets();
            fit_shadow_cascades();
            update_custom_frame();
        } catch (const VulkanFailure &error) {
            if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST)
                throw;
            throw SceneResourceError(error.what());
        } catch (const std::exception &error) {
            if (fatal)
                throw RendererFatalError(error.what());
            throw SceneResourceError(error.what());
        }
#endif
        if (slot.timing_pending) {
            // With VK_QUERY_RESULT_64_BIT and VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, each query writes its timestamp
            // and then a value that is nonzero once that timestamp is available.
            struct TimestampResult {
                std::uint64_t timestamp, available;
            };
            std::array<TimestampResult, TimingQuery::count> results{};
            const auto result = vkGetQueryPoolResults(device, timing_queries, slot.first_query, TimingQuery::count,
                                                      sizeof(results), results.data(), sizeof(TimestampResult),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_NOT_READY)
                check(result, "Read GPU timestamps");
            if (result == VK_SUCCESS &&
                std::all_of(results.begin(), results.end(), [](const auto &query) { return query.available != 0; })) {
                const auto mask = timestamp_bits >= 64 ? UINT64_MAX : (std::uint64_t{1} << timestamp_bits) - 1;
                const auto milliseconds = [&](std::uint64_t from, std::uint64_t to) {
                    // The device's timestamp period is in nanoseconds per tick.
                    const std::chrono::duration<double, std::nano> elapsed(double((to - from) & mask) *
                                                                           timestamp_period);
                    return std::chrono::duration<double, std::milli>(elapsed).count();
                };
                const auto between = [&](std::uint32_t from, std::uint32_t to) {
                    return milliseconds(results[from].timestamp, results[to].timestamp);
                };
                profile.gpu_ms = between(TimingQuery::start, TimingQuery::end);
                profile.gpu_atmosphere_ms = between(TimingQuery::start, TimingQuery::after_atmosphere);
                profile.gpu_shadow_ms = between(TimingQuery::after_atmosphere, TimingQuery::after_shadows);
                profile.gpu_scene_ms = between(TimingQuery::after_shadows, TimingQuery::after_scene);
                profile.gpu_resolve_ms = between(TimingQuery::after_scene, TimingQuery::after_resolve);
                profile.gpu_transfer_ms = between(TimingQuery::after_resolve, TimingQuery::end);
                profile.gpu_available = true;
                // The timed frame's first timestamp follows every command submitted before it. With one frame in
                // flight, the timed frame was also submitted after the fence of the frame before it, so its first
                // timestamp happens-after that frame's last, which calibrated timestamps keep from being lower. With
                // two, Vulkan defines no order between the two writes, but calibrated timestamps put both in the
                // device's one time domain, and a first timestamp below the earlier frame's last means that the queue
                // held the timed frame before it finished the earlier one, so it went without frame work for no time.
                //
                // A counter of fewer than 64 bits wraps to zero, after which the masked difference is the true one
                // modulo the counter's period of 2^timestamp_bits ticks, and negative in the upper half of that period.
                // Both timestamps lie between the earlier frame's submission and the end of this draw()'s fence wait,
                // the time in marked, so the difference is kept only while those host times are less than a quarter
                // of that period apart: half of the period tells the signs apart, and the rest covers a GPU clock up
                // to twice as fast as its stated period.
                if (calibrated_timestamps && previous_frame) {
                    constexpr double wrap_fraction = .25;
                    const std::chrono::duration<double, std::nano> wrap(
                        std::ldexp(timestamp_period, static_cast<int>(timestamp_bits)));
                    const auto first = results[TimingQuery::start].timestamp;
                    if (marked - previous_frame->submitted < wrap * wrap_fraction)
                        profile.gpu_idle_ms = ((first - previous_frame->end) & mask) > mask / 2
                                                  ? 0
                                                  : milliseconds(previous_frame->end, first);
                }
                previous_frame = TimedFrame{results[TimingQuery::end].timestamp, slot.timing_submitted};
            } else
                previous_frame.reset();
            slot.timing_pending = false;
        }
#ifdef ANIMA_UI
        retire_ui_unused();
        if (ui_frame) {
            if (ui_frame->width != static_cast<int>(extent.width) ||
                ui_frame->height != static_cast<int>(extent.height))
                return false; // Layout is refreshed by the next UI update.
            prepare_ui(*ui_frame);
        }
#else
        (void)ui_frame;
#endif
        measure(profile.prepare_ms);
#ifdef ANIMA_HAS_ASSETS
        if (!resource_scenes.empty()) {
            try {
                prepare_resources(resource_scenes);
                if (resource_opaque_inputs)
                    ensure_opaque_inputs();
            } catch (const VulkanFailure &error) {
                if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST)
                    throw;
                throw SceneResourceError(error.what());
            } catch (const std::exception &error) {
                if (fatal)
                    throw RendererFatalError(error.what());
                throw SceneResourceError(error.what());
            }
            profile.uploaded_bytes = resources.pose_uploaded_bytes;
        }
        try {
            // Preparation extended each cascade's depth toward its casters.
            finish_shadow_cascades();
            update_environment();
        } catch (const VulkanFailure &error) {
            if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST)
                throw;
            throw SceneResourceError(error.what());
        } catch (const std::exception &error) {
            if (fatal)
                throw RendererFatalError(error.what());
            throw SceneResourceError(error.what());
        }
#endif
        measure(profile.upload_ms);
        std::uint32_t index = 0;
        // The slot's fence ordered the last wait on its semaphore, so the semaphore has no pending operation.
        const auto acquire =
            vkAcquireNextImageKHR(device, swapchain, 100'000'000, slot.acquired, VK_NULL_HANDLE, &index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resize = true;
            return false;
        }
        if (acquire == VK_TIMEOUT || acquire == VK_NOT_READY)
            return false;
        if (acquire != VK_SUBOPTIMAL_KHR)
            check(acquire, "Acquire image");
        measure(profile.acquire_ms);
        auto &image = images.at(index);
        if (image.present_pending) {
            check(vkWaitForFences(device, 1, &image.presented, VK_TRUE, fence_timeout),
                  "Wait before reusing present fence");
            image.present_pending = false;
            check(vkResetFences(device, 1, &image.presented), "Reset present fence");
        }
        // recycle_slot() reset the slot's pool after its frame had finished.
        const auto command = slot.command;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "Begin command buffer");
        // Writes timestamp @p query of the slot's range. A timestamp's first synchronization scope holds every command
        // submitted before it, limited to its stage, and BOTTOM_OF_PIPE there stands for every stage, so each write
        // follows all earlier work, that of earlier submissions included. The first therefore waits for the frame
        // before, and with two frames in flight the GPU fields leave out the time spent finishing that frame.
        const auto timestamp = [&](std::uint32_t query) {
            if (timing_queries)
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries,
                                    slot.first_query + query);
        };
        if (timing_queries)
            vkCmdResetQueryPool(command, timing_queries, slot.first_query, TimingQuery::count);
        timestamp(TimingQuery::start);
#ifdef ANIMA_HAS_ASSETS
        record_atmosphere();
#endif
        timestamp(TimingQuery::after_atmosphere);
#ifdef ANIMA_HAS_ASSETS
        record_shadow();
#endif
        timestamp(TimingQuery::after_shadows);
        // The fixed background wherever neither the sky nor a mesh is drawn.
        constexpr VkClearColorValue clear_color{{0.018F, 0.027F, 0.041F, 1.0F}};
        std::array<VkClearValue, 2> clear{};
        clear[0].color = clear_color;
        // The far plane in reversed depth.
        clear[1].depthStencil = {0, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = render_pass;
        pass.framebuffer = image.framebuffer;
#ifdef ANIMA_HAS_ASSETS
        pass.framebuffer = world_target->framebuffer;
        // A frame whose custom materials read opaque depth or color splits the world pass around copies of them.
        const bool split = !resource_scenes.empty() && resource_opaque_inputs;
        if (split)
            pass.renderPass = opaque_pass;
#endif
        // The view covers the scene targets, which record_display() then filters to the swapchain's size.
        pass.renderArea.extent = scene_extent;
        pass.clearValueCount = 2;
        pass.pClearValues = clear.data();
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0, 0, static_cast<float>(scene_extent.width), static_cast<float>(scene_extent.height),
                                  0, 1};
        const VkRect2D scissor{{0, 0}, scene_extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        // The window's aspect: display conversion maps the whole scene target onto the whole window, so proportions on
        // screen follow the window's, whatever rounding or the device's limits make of the targets' aspect.
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        const float scale[]{std::min(1.0F, 1.0F / aspect), std::min(1.0F, aspect)};
        bool triangle = options.diagnostic_triangle;
#ifdef ANIMA_HAS_ASSETS
        // With scenes selected, record_resources() draws the sky after their opaque surfaces; without them it lies
        // behind the diagnostic triangle, which tests no depth.
        if (resource_scenes.empty())
            record_sky();
        else {
            triangle = false;
            record_resources();
            if (split) {
                vkCmdEndRenderPass(command);
                copy_opaque_inputs();
                pass.renderPass = composite_pass;
                pass.clearValueCount = 0;
                pass.pClearValues = nullptr;
                vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(command, 0, 1, &viewport);
                vkCmdSetScissor(command, 0, 1, &scissor);
                record_blended();
            }
        }
#endif
        if (triangle) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scale), scale);
            vkCmdDraw(command, 3, 1, 0, 0);
        }
#if defined(ANIMA_UI) && !defined(ANIMA_HAS_ASSETS)
        // Without the linear world target this pass renders the swapchain image, and the UI pipeline
        // was created for it, so the UI composites here rather than in the display pass below.
        if (ui_frame)
            record_ui(*ui_frame);
#endif
        vkCmdEndRenderPass(command);
        timestamp(TimingQuery::after_scene);
#ifdef ANIMA_HAS_ASSETS
        finish_world_writes();
        record_display(image.framebuffer);
#ifdef ANIMA_UI
        if (ui_frame)
            record_ui(*ui_frame);
#endif
        vkCmdEndRenderPass(command);
#endif
        timestamp(TimingQuery::after_resolve);
        const bool capture = capture_buffer && (capture_to_memory || !options.capture.empty());
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = capture ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = capture ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             capture ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        if (capture) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer, 1,
                                   &copy);
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = capture_buffer;
            host.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &host, 0, nullptr);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
        }
        timestamp(TimingQuery::end);
        check(vkEndCommandBuffer(command), "End command buffer");
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &slot.acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &image.rendered;
        // Reset only when submission will happen; an out-of-date acquire must not strand an unsignaled fence.
        check(vkResetFences(device, 1, &slot.fence), "Reset frame fence");
        // The GPU cannot write the frame's timestamps before it is submitted.
        if (calibrated_timestamps)
            slot.timing_submitted = Clock::now();
        check(vkQueueSubmit(graphics_queue, 1, &submit, slot.fence), "Submit frame");
        slot.in_flight = true;
        slot.timing_pending = timing_queries != VK_NULL_HANDLE;
        latest_frame = frame_index;
        frame_index = (frame_index + 1) % frames.size();
        // The frame submitted frames_in_flight + 1 submissions later waits for this one's present.
        auto &queued_present = present_ids[present_index];
        present_index = (present_index + 1) % (frames.size() + 1);
        measure(profile.record_submit_ms);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        const auto attach = [&](auto &extension) {
            extension.pNext = present.pNext;
            present.pNext = &extension;
        };
        VkSwapchainPresentFenceInfoEXT fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        fence_info.swapchainCount = 1;
        fence_info.pFences = &image.presented;
        if (present_fences)
            attach(fence_info);
        const std::uint64_t present_id = ++presents;
        VkPresentIdKHR id_info{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        id_info.swapchainCount = 1;
        id_info.pPresentIds = &present_id;
        const bool awaited = awaits_presents();
        if (awaited)
            attach(id_info);
        // VK_GOOGLE_display_timing's ids are 32 bits wide, so they wrap; read_display_times() allows for that.
        const VkPresentTimeGOOGLE present_time{static_cast<std::uint32_t>(present_id), 0};
        VkPresentTimesInfoGOOGLE times_info{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        times_info.swapchainCount = 1;
        times_info.pTimes = &present_time;
        if (display_timing)
            attach(times_info);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &image.rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const auto result = vkQueuePresentKHR(present_queue, &present);
        measure(profile.present_ms);
        // Out-of-date/surface-lost still enqueue presentation's semaphore waits.
        image.present_pending =
            present_fences && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
                               result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR);
        if (awaited && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
            queued_present = present_id;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR)
            resize = true;
        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)
            check(result, "Present frame");
        // Count the frame before writing its capture, since a failed write still leaves it presented.
        const bool presented = result != VK_ERROR_OUT_OF_DATE_KHR;
        if (presented)
            ++stats.presented_frames;
        if (capture)
            save_capture();
        return presented;
    }
    void destroy_swapchain() noexcept {
        if (ui_pipeline)
            vkDestroyPipeline(device, ui_pipeline, nullptr);
        ui_pipeline = VK_NULL_HANDLE;
        if (capture_buffer)
            vmaDestroyBuffer(allocator, capture_buffer, capture_allocation);
        capture_buffer = VK_NULL_HANDLE;
        capture_allocation = VK_NULL_HANDLE;
        capture_mapping = nullptr;
        for (auto &image : images) {
            if (image.framebuffer)
                vkDestroyFramebuffer(device, image.framebuffer, nullptr);
            if (image.view)
                vkDestroyImageView(device, image.view, nullptr);
            if (image.rendered)
                vkDestroySemaphore(device, image.rendered, nullptr);
            if (image.presented)
                vkDestroyFence(device, image.presented, nullptr);
        }
        images.clear();
#ifdef ANIMA_HAS_ASSETS
        destroy_world_targets();
#endif
        depth_target.reset();
        scene_extent = {};
#ifdef ANIMA_HAS_ASSETS
        // Each pair holds a pipeline without and with the height fog's code.
        const auto destroy_pair = [&](std::array<VkPipeline, 2> &pipelines) noexcept {
            for (auto &value : pipelines) {
                if (value)
                    vkDestroyPipeline(device, value, nullptr);
                value = VK_NULL_HANDLE;
            }
        };
        for (auto &pipelines : resource_pipelines)
            destroy_pair(pipelines);
        destroy_pair(blended_resource_pipelines);
        destroy_pair(impostor_pipelines);
        if (sky_pipeline)
            vkDestroyPipeline(device, sky_pipeline, nullptr);
        sky_pipeline = VK_NULL_HANDLE;
#endif
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        if (render_pass)
            vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        // Present ids and display times belong to the swapchain.
        present_ids.fill(0);
        latest_display.reset();
    }
    void cleanup() noexcept {
        if (stopped)
            return;
        stopped = true;
        if (device) {
            const auto idle = vkDeviceWaitIdle(device);
            if (idle != VK_SUCCESS) {
                ++errors;
                std::fprintf(stderr, "Device idle failed during cleanup: %d\n", idle);
            }
            // Never destroy presentation resources merely because a timed wait expired.
            // A hung driver during teardown needs an external process timeout.
            try {
                wait_for_presentation(UINT64_MAX);
            } catch (const std::exception &error) {
                ++errors;
                std::fprintf(stderr, "%s\n", error.what());
            }
            destroy_swapchain();
            // Retired objects first, since some, such as shadow maps, were made for objects destroyed below.
            for (auto &slot : frames)
                slot.released.clear();
#ifdef ANIMA_HAS_ASSETS
            resource_instances.clear();
            resource_blended_draws.clear();
            resource_custom_draws.clear();
            resource_custom_shadow_draws.clear();
            resource_scenes.clear();
            resource_cache.clear();
            custom_cache.clear();
            placement_cache.clear();
            identity_placement.reset();
            if (custom_frame_pool)
                vkDestroyDescriptorPool(device, custom_frame_pool, nullptr);
            if (custom_pipeline_layout)
                vkDestroyPipelineLayout(device, custom_pipeline_layout, nullptr);
            if (custom_frame_layout)
                vkDestroyDescriptorSetLayout(device, custom_frame_layout, nullptr);
            if (custom_material_layout)
                vkDestroyDescriptorSetLayout(device, custom_material_layout, nullptr);
            cascade_target.reset();
            detail_shadow_target.reset();
            if (shadow_opaque_pipeline)
                vkDestroyPipeline(device, shadow_opaque_pipeline, nullptr);
            if (shadow_masked_pipeline)
                vkDestroyPipeline(device, shadow_masked_pipeline, nullptr);
            if (shadow_impostor_pipeline)
                vkDestroyPipeline(device, shadow_impostor_pipeline, nullptr);
            if (impostor_vertex_shader)
                vkDestroyShaderModule(device, impostor_vertex_shader, nullptr);
            if (impostor_fragment_shader)
                vkDestroyShaderModule(device, impostor_fragment_shader, nullptr);
            if (impostor_shadow_fragment_shader)
                vkDestroyShaderModule(device, impostor_shadow_fragment_shader, nullptr);
            if (shadow_pass)
                vkDestroyRenderPass(device, shadow_pass, nullptr);
            if (shadow_resource_vertex_shader)
                vkDestroyShaderModule(device, shadow_resource_vertex_shader, nullptr);
            if (shadow_fragment_shader)
                vkDestroyShaderModule(device, shadow_fragment_shader, nullptr);
            destroy_atmosphere();
            if (post_pipeline_layout)
                vkDestroyPipelineLayout(device, post_pipeline_layout, nullptr);
            if (scene_input_layout)
                vkDestroyDescriptorSetLayout(device, scene_input_layout, nullptr);
            if (resolve_fragment_shader)
                vkDestroyShaderModule(device, resolve_fragment_shader, nullptr);
            if (environment_pool)
                vkDestroyDescriptorPool(device, environment_pool, nullptr);
            if (environment_layout)
                vkDestroyDescriptorSetLayout(device, environment_layout, nullptr);
            if (pose_pool)
                vkDestroyDescriptorPool(device, pose_pool, nullptr);
            if (resource_pipeline_layout)
                vkDestroyPipelineLayout(device, resource_pipeline_layout, nullptr);
            if (pose_layout)
                vkDestroyDescriptorSetLayout(device, pose_layout, nullptr);
            if (resource_vertex_shader)
                vkDestroyShaderModule(device, resource_vertex_shader, nullptr);
            if (sky_vertex_shader)
                vkDestroyShaderModule(device, sky_vertex_shader, nullptr);
            if (sky_fragment_shader)
                vkDestroyShaderModule(device, sky_fragment_shader, nullptr);
#endif
#ifdef ANIMA_UI
            ui_images.clear();
#endif
            if (ui_pipeline_layout)
                vkDestroyPipelineLayout(device, ui_pipeline_layout, nullptr);
            if (ui_texture_layout)
                vkDestroyDescriptorSetLayout(device, ui_texture_layout, nullptr);
            if (ui_vertex_shader)
                vkDestroyShaderModule(device, ui_vertex_shader, nullptr);
            if (ui_fragment_shader)
                vkDestroyShaderModule(device, ui_fragment_shader, nullptr);
            if (environment_pipeline_layout)
                vkDestroyPipelineLayout(device, environment_pipeline_layout, nullptr);
            if (mesh_fragment_shader)
                vkDestroyShaderModule(device, mesh_fragment_shader, nullptr);
            if (pipeline_layout)
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (texture_layout)
                vkDestroyDescriptorSetLayout(device, texture_layout, nullptr);
            if (vertex_shader)
                vkDestroyShaderModule(device, vertex_shader, nullptr);
            if (fragment_shader)
                vkDestroyShaderModule(device, fragment_shader, nullptr);
            for (const auto &slot : frames) {
                if (slot.acquired)
                    vkDestroySemaphore(device, slot.acquired, nullptr);
                if (slot.fence)
                    vkDestroyFence(device, slot.fence, nullptr);
                if (slot.pool)
                    vkDestroyCommandPool(device, slot.pool, nullptr);
            }
            // Their buffers, which need the allocator; the descriptor pools above freed their sets.
            frames.clear();
            if (timing_queries)
                vkDestroyQueryPool(device, timing_queries, nullptr);
            // Every buffer and image, and so every allocation, is destroyed by now.
            if (allocator)
                vmaDestroyAllocator(allocator);
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
            SDL_Vulkan_DestroySurface(instance, surface, nullptr);
        if (messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy)
                destroy(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        stats.validation_warnings = warnings.load();
        stats.validation_errors = errors.load();
        std::cout << "Renderer cleanup: validation_warnings=" << stats.validation_warnings
                  << " validation_errors=" << stats.validation_errors << '\n';
    }
};

VulkanRenderer::VulkanRenderer(SDL_Window *window, RendererOptions options)
    : impl_(std::make_unique<Impl>(window, std::move(options))) {
    // If initialize throws, the already-constructed Impl owns and frees every completed stage.
    try {
        impl_->initialize();
    } catch (const VulkanFailure &error) {
        throw std::runtime_error(error.what()); // VulkanFailure is private.
    }
}
VulkanRenderer::~VulkanRenderer() = default;
void VulkanRenderer::request_resize() noexcept { impl_->resize = true; }
void VulkanRenderer::request_capture(std::filesystem::path path) {
    impl_->running();
    if (path.empty())
        throw std::invalid_argument("Capture path is empty");
    impl_->options.capture = std::move(path);
    impl_->capture_to_memory = false;
    impl_->captured_image.reset();
    impl_->stats.captured = false;
    if (!impl_->capture_buffer)
        impl_->resize = true;
}
void VulkanRenderer::request_capture() {
    impl_->running();
    impl_->options.capture.clear();
    impl_->capture_to_memory = true;
    impl_->captured_image.reset();
    impl_->stats.captured = false;
    if (!impl_->capture_buffer)
        impl_->resize = true;
}
std::optional<CapturedImage> VulkanRenderer::take_capture() { return std::exchange(impl_->captured_image, {}); }
bool VulkanRenderer::samples_bc7() const noexcept { return impl_->bc7_sampled; }
float VulkanRenderer::max_anisotropy() const noexcept { return impl_->anisotropy; }
void VulkanRenderer::set_view(const Mat4 &view_projection) {
    impl_->running();
    for (float value : view_projection)
        if (!std::isfinite(value))
            throw MathError(MathErrorCode::nonfinite_projection);
#ifdef ANIMA_HAS_ASSETS
    const auto origin = anima::view_origin(view_projection);
    const auto inverted = anima::inverse(view_projection);
    const RenderFrustum frustum(view_projection);
    impl_->view_origin = origin;
    impl_->resource_frustum = frustum;
    impl_->inverse_view = inverted;
    impl_->view_rays = Impl::rays_of(view_projection, origin);
    impl_->view_set = true;
#endif
    impl_->view_projection = view_projection;
}
void VulkanRenderer::set_frustum_culling(bool enabled) {
    impl_->running();
    impl_->options.frustum_culling = enabled;
}
void VulkanRenderer::set_present_mode(PresentMode mode) {
    impl_->running();
    (void)vulkan_present_mode(mode);
    if (mode == impl_->options.present_mode)
        return;
    impl_->options.present_mode = mode;
    impl_->resize = true;
}
std::optional<PresentMode> VulkanRenderer::present_mode() const noexcept { return impl_->presenting; }
bool VulkanRenderer::waits_for_presents() const noexcept { return impl_->present_waits; }
void VulkanRenderer::set_lod_threshold(float pixels) {
    impl_->running();
    if (!std::isfinite(pixels) || pixels < 0)
        throw std::invalid_argument("LOD threshold must be finite and nonnegative");
    impl_->options.lod_threshold = pixels;
}
void VulkanRenderer::set_render_scale(float scale) {
    impl_->running();
    validate_render_scale(scale);
#ifdef ANIMA_HAS_ASSETS
    // The next draw() resizes the scene targets once it has waited for its frame.
    impl_->options.render_scale = scale;
#else
    throw std::logic_error(render_scale_without_assets);
#endif
}
float VulkanRenderer::render_scale() const noexcept { return impl_->options.render_scale; }
void VulkanRenderer::set_scenes(std::vector<std::shared_ptr<const Scene>> sources, SceneReplacementOptions options) {
#ifdef ANIMA_HAS_ASSETS
    impl_->set_scenes(std::move(sources), std::move(options));
#else
    (void)sources;
    (void)options;
    throw std::logic_error("Resource rendering requires the asset library");
#endif
}
void VulkanRenderer::prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets,
                                    ResourcePreparationOptions options) {
#ifdef ANIMA_HAS_ASSETS
    impl_->prepare_meshes(assets, options);
#else
    (void)assets;
    (void)options;
    throw std::logic_error("Resource rendering requires the asset library");
#endif
}
void VulkanRenderer::prepare_mesh(const MeshPreparation &preparation, ResourcePreparationOptions options) {
#ifdef ANIMA_HAS_ASSETS
    impl_->prepare_meshes(std::span(&preparation.asset(), 1), options, &preparation);
#else
    (void)preparation;
    (void)options;
    throw std::logic_error("Resource rendering requires the asset library");
#endif
}
ResourceStats VulkanRenderer::resource_stats() const noexcept {
#ifdef ANIMA_HAS_ASSETS
    auto stats = impl_->resource_stats();
    // The depth target exists whenever the world target does.
    if (impl_->world_target)
        stats.world_target_bytes = impl_->world_target->color.allocation_bytes + impl_->depth_target->allocation_bytes;
    return stats;
#else
    return {};
#endif
}
void VulkanRenderer::wait_for_frame() {
    try {
        impl_->wait_for_frame();
    } catch (const VulkanFailure &error) {
        impl_->fatal = true;
        throw RendererFatalError(error.what());
    }
}
bool VulkanRenderer::draw() {
    try {
        return impl_->draw();
    } catch (const VulkanFailure &error) {
        impl_->fatal = true;
        throw RendererFatalError(error.what());
    }
}
void VulkanRenderer::set_time(float seconds) {
    impl_->running();
    if (!std::isfinite(seconds))
        throw std::invalid_argument("Shader time must be finite");
#ifdef ANIMA_HAS_ASSETS
    impl_->shader_time = seconds;
#else
    throw std::logic_error("Custom materials require asset support");
#endif
}
void VulkanRenderer::set_environment(const Environment &environment) {
    impl_->running();
    validate_environment(environment);
#ifdef ANIMA_HAS_ASSETS
    // Validated above, so neither the region's projection nor the sunlight validates it again.
    const auto detail_view = detail::detail_shadow_matrix_of(environment);
    const RenderFrustum detail_frustum(detail_view);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl_->physical, &properties);
    const auto &limits = properties.limits;
    const auto fits = [&](std::uint32_t resolution) {
        return resolution <= limits.maxImageDimension2D && resolution <= limits.maxFramebufferWidth &&
               resolution <= limits.maxFramebufferHeight;
    };
    // Disabled maps allocate only their 1x1 placeholders, so the device bounds enabled ones alone.
    const auto &cascades = environment.shadow_cascades;
    if ((cascades.enabled && (!fits(cascades.resolution) || cascades.count > limits.maxImageArrayLayers)) ||
        (environment.detail_shadow.enabled && !fits(environment.detail_shadow.resolution)))
        throw std::invalid_argument("Shadow resolution exceeds device capabilities");
    const auto sunlight = impl_->ground_sunlight(environment);
    impl_->environment = environment;
    impl_->sunlight = sunlight;
    impl_->detail_pass = {detail_view, detail_frustum};
#else
    throw std::logic_error("Environment rendering requires asset support");
#endif
}
RenderStats VulkanRenderer::stats() const noexcept {
    auto current = impl_->stats;
    // The validation counts stay in their atomic counters until cleanup() copies them, once they are final.
    if (!impl_->stopped) {
        current.validation_warnings = impl_->warnings.load();
        current.validation_errors = impl_->errors.load();
    }
    return current;
}
RenderStats VulkanRenderer::shutdown() {
    impl_->cleanup();
    return impl_->stats;
}
FrameProfile VulkanRenderer::frame_profile() const noexcept { return impl_->profile; }
bool VulkanRenderer::measures_gpu_idle() const noexcept { return impl_->calibrated_timestamps; }
bool VulkanRenderer::measures_present_interval() const noexcept { return impl_->display_timing; }
#ifdef ANIMA_UI
bool VulkanRenderer::srgb_presentation() {
    impl_->running();
    try {
        return impl_->srgb_presentation();
    } catch (const VulkanFailure &error) {
        impl_->fatal = true;
        throw RendererFatalError(error.what());
    }
}
bool VulkanRenderer::draw_ui(const detail::UiFrame &frame) {
    // A renderer that failed earlier stays fatal after shutdown, so only a failure of this call is replaced below.
    const bool fatal_before = impl_->fatal;
    try {
        return impl_->draw(&frame);
    } catch (const VulkanFailure &error) {
        impl_->fatal = true;
        throw RendererFatalError(error.what());
    } catch (const RendererFatalError &) {
        throw; // Already reports the failure that made the renderer fatal.
    } catch (...) {
        // Retiring a pending UI texture upload while another exception unwinds it can find the device lost and
        // make the renderer fatal; report the loss rather than that exception. Only a VulkanFailure can unwind
        // a pending UI upload today, and the first handler reports it.
        if (!fatal_before && impl_->fatal)
            throw RendererFatalError("Device lost while retiring UI upload");
        throw;
    }
}
#endif
} // namespace anima
