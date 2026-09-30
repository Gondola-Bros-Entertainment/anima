#include <anima/desktop/vulkan_renderer.hpp>
#ifdef ANIMA_HAS_ASSETS
#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/assets/render_visibility.hpp>
#include <anima/scene.hpp>
#include <map>
#include <tuple>
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
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace anima {
namespace {
constexpr std::uint64_t fence_timeout = 5'000'000'000ULL;
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
// Swapchain formats in order of preference, each in VK_COLOR_SPACE_SRGB_NONLINEAR_KHR.
constexpr std::array preferred_surface_formats{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                               VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
} // namespace

struct VulkanRenderer::Impl {
    SDL_Window *window;
    RendererOptions options;
    RenderStats stats{};
    FrameProfile profile{};
    // Timestamps of a profiled frame, in the order draw() writes them; FrameProfile's GPU fields are the
    // intervals between them.
    struct TimingQuery {
        enum : std::uint32_t { start, after_shadows, after_scene, after_resolve, end, count };
    };
    VkQueryPool timing_queries{};
    std::uint32_t timestamp_bits{};
    double timestamp_period{};
    bool timing_pending{};
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
    // Whether BC7 images upload as BC7: the device samples BC7 with linear filtering and RendererOptions::decode_bc7
    // is off. Otherwise they upload decoded to RGBA8.
    bool bc7_sampled{};
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    VkFence frame_fence{};
    VkSemaphore acquired{};
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
#include "environment_renderer.inc"
#include "resource_renderer.inc"
#include "shadow_renderer.inc"
#include "world_renderer.inc"
// Custom materials draw the resource renderer's instances, so their declarations follow it.
#include "custom_renderer.inc"
#endif
    std::array<float, 16> view_projection{};
    std::array<float, 4> view_origin{0, 0, -1, 0};

    VkSwapchainKHR swapchain{};
    VkExtent2D extent{};
    VkFormat format{};
    VkRenderPass render_pass{};
    VkPipeline pipeline{}, ui_pipeline{};
    VkImage depth_image{};
    VmaAllocation depth_allocation{};
    VkImageView depth_view{};
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
        default:
            throw std::invalid_argument(unknown_stage);
        }
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
                if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    graphics = i;
                if (supported)
                    present = i;
                if (supported && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
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
        info.pEnabledFeatures = &enabled;
        if (present_fences)
            info.pNext = &maintenance;
        check(vkCreateDevice(physical, &info, nullptr, &device), "Create device");
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
        std::cout << "BC7 textures: " << (bc7_sampled ? "sampled as BC7" : "decoded to RGBA8 on the CPU") << '\n';
    }
    void create_frame_resources() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = graphics_family;
        check(vkCreateCommandPool(device, &pool, nullptr, &command_pool), "Create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocation, &command), "Allocate command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fence, nullptr, &frame_fence), "Create frame fence");
        if (options.profile && timestamp_bits && timestamp_period > 0) {
            VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = TimingQuery::count;
            check(vkCreateQueryPool(device, &queries, nullptr, &timing_queries), "Create timing query pool");
        }
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphore, nullptr, &acquired), "Create acquire semaphore");
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
        create_shadow_pass();
        create_pipeline(PipelineKind::shadow_resource, shadow_resource_pipeline);
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
        check(vkDeviceWaitIdle(device), "Wait before swapchain recreation");
        wait_for_presentation();
        destroy_swapchain();
        // Nothing can be presented until the replacement is complete, so any failure from here on is fatal.
        try {
            create_swapchain(caps, selected, selected_extent, capture);
        } catch (const std::exception &error) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        resize = false;
        ++stats.swapchain_generations;
        std::cout << "Swapchain " << stats.swapchain_generations << ": " << extent.width << 'x' << extent.height
                  << " pixels, " << images.size() << " images\n";
        return true;
    }
    void create_swapchain(const VkSurfaceCapabilitiesKHR &caps, VkSurfaceFormatKHR selected, VkExtent2D selected_extent,
                          bool capture) {
        extent = selected_extent;
        format = selected.format;
        auto count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0)
            count = std::min(count, caps.maxImageCount);
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
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // Required by Vulkan; bounded CPU/GPU use.
        info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create swapchain");
        const auto handles = enumerate<VkImage>(
            [&](auto *n, auto *p) { return vkGetSwapchainImagesKHR(device, swapchain, n, p); }, "Get swapchain images");
        images.resize(handles.size());
        create_depth();
        create_render_pass();
#ifdef ANIMA_HAS_ASSETS
        create_world_targets();
        create_display_pipeline();
#endif
        create_pipeline(PipelineKind::diagnostic, pipeline);
#ifdef ANIMA_HAS_ASSETS
        create_pipeline(PipelineKind::resource, resource_pipeline);
        create_pipeline(PipelineKind::blended_resource, blended_resource_pipeline);
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
            framebuffer.renderPass = render_pass;
            const VkImageView attachments[]{image.view, depth_view};
            framebuffer.attachmentCount = 2;
#ifdef ANIMA_HAS_ASSETS
            framebuffer.renderPass = present_pass;
            framebuffer.attachmentCount = 1;
#endif
            framebuffer.pAttachments = attachments;
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
    // blended_resource draws meshes as resource does, but composites premultiplied color over the target and
    // writes no depth.
    enum class PipelineKind { diagnostic, ui, resource, blended_resource, sky, shadow_resource };
    void create_pipeline(PipelineKind mode, VkPipeline &output) {
        const bool shadow = mode == PipelineKind::shadow_resource;
        const bool blended = mode == PipelineKind::blended_resource;
#ifdef ANIMA_HAS_ASSETS
        const bool resource = mode == PipelineKind::resource || blended || shadow;
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
        // mesh.frag's constant 0 selects premultiplied output.
        const VkBool32 premultiplied = VK_TRUE;
        const VkSpecializationMapEntry premultiplied_entry{0, 0, sizeof(premultiplied)};
        const VkSpecializationInfo premultiplied_output{1, &premultiplied_entry, sizeof(premultiplied), &premultiplied};
        if (blended)
            stages[1].pSpecializationInfo = &premultiplied_output;
        if (sky) {
            stages[0].module = sky_vertex_shader;
            stages[1].module = sky_fragment_shader;
        }
        if (shadow) {
            stages[0].module = shadow_resource_vertex_shader;
            stages[1].module = shadow_fragment_shader;
        }
#endif
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
#ifdef ANIMA_HAS_ASSETS
        const VkVertexInputBindingDescription resource_binding{0, sizeof(SourceVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        if (resource) {
            vertex.vertexBindingDescriptionCount = 1;
            vertex.pVertexBindingDescriptions = &resource_binding;
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(resource_attributes.size());
            vertex.pVertexAttributeDescriptions = resource_attributes.data();
        }
        const VkVertexInputAttributeDescription shadow_resource_attributes[]{
            resource_attributes[0], resource_attributes[3], resource_attributes[4], resource_attributes[5],
            resource_attributes[7]};
        if (shadow) {
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(shadow_resource_attributes));
            vertex.pVertexAttributeDescriptions = shadow_resource_attributes;
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
        depth_state.depthTestEnable = !ui;
        // Blended surfaces are hidden by nearer opaque and masked ones, and hide nothing themselves.
        depth_state.depthWriteEnable = !ui && !sky && !blended;
        depth_state.depthCompareOp = sky ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
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
        info.stageCount = 2;
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
    void create_depth() {
        depth_format = VK_FORMAT_UNDEFINED;
        for (auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, candidate, &properties);
            VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if ((properties.optimalTilingFeatures & needed) == needed) {
                depth_format = candidate;
                break;
            }
        }
        if (depth_format == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("No depth attachment format");
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = depth_format;
        image.extent = {extent.width, extent.height, 1};
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
        [[maybe_unused]] const auto bytes = create_image(image, depth_image, depth_allocation, "Create depth image");
#ifdef ANIMA_HAS_ASSETS
        depth_allocation_bytes = bytes;
#endif
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = depth_image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = depth_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &depth_view), "Create depth view");
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
        info.maxAnisotropy = 1;
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
    void save_capture() {
        // Consume the request first, so a file that cannot be written is reported by one draw, not every draw.
        const auto path = std::exchange(options.capture, {});
        const bool to_memory = std::exchange(capture_to_memory, false);
        check(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, fence_timeout), "Wait for capture");
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
        check(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, fence_timeout), "Wait for frame");
        measure(profile.fence_wait_ms);
#ifdef ANIMA_HAS_ASSETS
        check(vkResetCommandBuffer(command, 0), "Reset retired resource commands");
        retire_resources();
        try {
            ensure_shadow_targets();
            update_environment();
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
        if (timing_pending) {
            // With VK_QUERY_RESULT_64_BIT and VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, each query writes its timestamp
            // and then a value that is nonzero once that timestamp is available.
            struct TimestampResult {
                std::uint64_t timestamp, available;
            };
            std::array<TimestampResult, TimingQuery::count> results{};
            const auto result = vkGetQueryPoolResults(device, timing_queries, 0, TimingQuery::count, sizeof(results),
                                                      results.data(), sizeof(TimestampResult),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_NOT_READY)
                check(result, "Read GPU timestamps");
            if (result == VK_SUCCESS &&
                std::all_of(results.begin(), results.end(), [](const auto &query) { return query.available != 0; })) {
                const auto mask = timestamp_bits >= 64 ? UINT64_MAX : (std::uint64_t{1} << timestamp_bits) - 1;
                const auto milliseconds = [&](std::uint32_t from, std::uint32_t to) {
                    // The device's timestamp period is in nanoseconds per tick.
                    const std::chrono::duration<double, std::nano> elapsed(
                        double((results[to].timestamp - results[from].timestamp) & mask) * timestamp_period);
                    return std::chrono::duration<double, std::milli>(elapsed).count();
                };
                profile.gpu_ms = milliseconds(TimingQuery::start, TimingQuery::end);
                profile.gpu_shadow_ms = milliseconds(TimingQuery::start, TimingQuery::after_shadows);
                profile.gpu_scene_ms = milliseconds(TimingQuery::after_shadows, TimingQuery::after_scene);
                profile.gpu_resolve_ms = milliseconds(TimingQuery::after_scene, TimingQuery::after_resolve);
                profile.gpu_transfer_ms = milliseconds(TimingQuery::after_resolve, TimingQuery::end);
                profile.gpu_available = true;
            }
            timing_pending = false;
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
        if (options.profile)
            marked = Clock::now();
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
#endif
        measure(profile.upload_ms);
        std::uint32_t index = 0;
        const auto acquire = vkAcquireNextImageKHR(device, swapchain, 100'000'000, acquired, VK_NULL_HANDLE, &index);
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
        check(vkResetCommandBuffer(command, 0), "Reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "Begin command buffer");
        if (timing_queries) {
            vkCmdResetQueryPool(command, timing_queries, 0, TimingQuery::count);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing_queries, TimingQuery::start);
        }
#ifdef ANIMA_HAS_ASSETS
        record_shadow();
#endif
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries,
                                TimingQuery::after_shadows);
        // The fixed background wherever neither the sky nor a mesh is drawn.
        constexpr VkClearColorValue clear_color{{0.018F, 0.027F, 0.041F, 1.0F}};
        std::array<VkClearValue, 2> clear{};
        clear[0].color = clear_color;
        clear[1].depthStencil = {1, 0};
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
        pass.renderArea.extent = extent;
        pass.clearValueCount = 2;
        pass.pClearValues = clear.data();
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        const float scale[]{std::min(1.0F, 1.0F / aspect), std::min(1.0F, aspect)};
#ifdef ANIMA_HAS_ASSETS
        if (environment.sky) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, environment_pipeline_layout, 1, 1,
                                    &environment_set, 0, nullptr);
            vkCmdDraw(command, 3, 1, 0, 0);
        }
        if (!resource_scenes.empty()) {
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
        } else
#endif
            if (options.diagnostic_triangle) {
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
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries,
                                TimingQuery::after_scene);
#ifdef ANIMA_HAS_ASSETS
        finish_world_writes();
        record_display(image.framebuffer);
#ifdef ANIMA_UI
        if (ui_frame)
            record_ui(*ui_frame);
#endif
        vkCmdEndRenderPass(command);
#endif
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries,
                                TimingQuery::after_resolve);
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
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries, TimingQuery::end);
        check(vkEndCommandBuffer(command), "End command buffer");
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &image.rendered;
        // Reset only when submission will happen; an out-of-date acquire must not strand an unsignaled fence.
        check(vkResetFences(device, 1, &frame_fence), "Reset frame fence");
        check(vkQueueSubmit(graphics_queue, 1, &submit, frame_fence), "Submit frame");
        timing_pending = timing_queries != VK_NULL_HANDLE;
        measure(profile.record_submit_ms);
        VkSwapchainPresentFenceInfoEXT fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        fence_info.swapchainCount = 1;
        fence_info.pFences = &image.presented;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        if (present_fences)
            present.pNext = &fence_info;
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
        if (depth_view)
            vkDestroyImageView(device, depth_view, nullptr);
        depth_view = VK_NULL_HANDLE;
        if (depth_image)
            vmaDestroyImage(allocator, depth_image, depth_allocation);
        depth_image = VK_NULL_HANDLE;
        depth_allocation = VK_NULL_HANDLE;
#ifdef ANIMA_HAS_ASSETS
        if (resource_pipeline)
            vkDestroyPipeline(device, resource_pipeline, nullptr);
        resource_pipeline = VK_NULL_HANDLE;
        if (blended_resource_pipeline)
            vkDestroyPipeline(device, blended_resource_pipeline, nullptr);
        blended_resource_pipeline = VK_NULL_HANDLE;
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
#ifdef ANIMA_HAS_ASSETS
            resource_instances.clear();
            resource_blended_draws.clear();
            resource_custom_draws.clear();
            resource_custom_shadow_draws.clear();
            resource_scenes.clear();
            resource_cache.clear();
            custom_cache.clear();
            custom_frame_buffer.reset();
            if (custom_frame_pool)
                vkDestroyDescriptorPool(device, custom_frame_pool, nullptr);
            if (custom_pipeline_layout)
                vkDestroyPipelineLayout(device, custom_pipeline_layout, nullptr);
            if (custom_frame_layout)
                vkDestroyDescriptorSetLayout(device, custom_frame_layout, nullptr);
            if (custom_material_layout)
                vkDestroyDescriptorSetLayout(device, custom_material_layout, nullptr);
            pose_buffer.reset();
            shadow_target.reset();
            detail_shadow_target.reset();
            if (shadow_resource_pipeline)
                vkDestroyPipeline(device, shadow_resource_pipeline, nullptr);
            if (shadow_pass)
                vkDestroyRenderPass(device, shadow_pass, nullptr);
            if (shadow_resource_vertex_shader)
                vkDestroyShaderModule(device, shadow_resource_vertex_shader, nullptr);
            if (shadow_fragment_shader)
                vkDestroyShaderModule(device, shadow_fragment_shader, nullptr);
            environment_buffer.reset();
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
            ui_buffer.reset();
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
            if (acquired)
                vkDestroySemaphore(device, acquired, nullptr);
            if (frame_fence)
                vkDestroyFence(device, frame_fence, nullptr);
            if (timing_queries)
                vkDestroyQueryPool(device, timing_queries, nullptr);
            if (command_pool)
                vkDestroyCommandPool(device, command_pool, nullptr);
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
void VulkanRenderer::set_view(const std::array<float, 16> &view_projection) {
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
#endif
    impl_->view_projection = view_projection;
}
void VulkanRenderer::set_frustum_culling(bool enabled) {
    impl_->running();
    impl_->options.frustum_culling = enabled;
}
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
    if (impl_->world_target)
        stats.world_target_bytes = impl_->world_target->color.allocation_bytes + impl_->depth_allocation_bytes;
    return stats;
#else
    return {};
#endif
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
    const auto shadow = directional_shadow_matrix(environment);
    const RenderFrustum frustum(shadow);
    const auto detail_shadow = directional_shadow_matrix(environment, true);
    const RenderFrustum detail_frustum(detail_shadow);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl_->physical, &properties);
    // A disabled region allocates only its 1x1 placeholder, so the device bounds enabled regions alone.
    for (const auto &region : {environment.shadow, environment.detail_shadow})
        if (region.enabled && (region.resolution > properties.limits.maxImageDimension2D ||
                               region.resolution > properties.limits.maxFramebufferWidth ||
                               region.resolution > properties.limits.maxFramebufferHeight))
            throw std::invalid_argument("Shadow resolution exceeds device capabilities");
    impl_->environment = environment;
    impl_->shadow_view = shadow;
    impl_->shadow_frustum = frustum;
    impl_->detail_shadow_view = detail_shadow;
    impl_->detail_shadow_frustum = detail_frustum;
#else
    throw std::logic_error("Environment rendering requires asset support");
#endif
}
RenderStats VulkanRenderer::shutdown() {
    impl_->cleanup();
    return impl_->stats;
}
FrameProfile VulkanRenderer::frame_profile() const noexcept { return impl_->profile; }
#ifdef ANIMA_UI
std::uint64_t VulkanRenderer::presented_frames() const noexcept { return impl_->stats.presented_frames; }
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
