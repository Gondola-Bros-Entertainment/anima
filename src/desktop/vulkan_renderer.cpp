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
} // namespace

struct VulkanRenderer::Impl {
    SDL_Window *window;
    RendererOptions options;
    RenderStats stats{};
    FrameProfile profile{};
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
    std::uint32_t graphics_family{}, present_family{};
    VkQueue graphics_queue{}, present_queue{};
    bool maintenance_instance{}, present_fences{}, resize = true, stopped = false, fatal = false;
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
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        std::shared_ptr<GpuSampler> shared_sampler;
        VkDeviceSize allocation_bytes{};
    };
#ifdef ANIMA_HAS_ASSETS
    struct ResourceBuffer {
        VkDevice device{};
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize bytes{}, allocation_bytes{};
        void *mapping{};
        ~ResourceBuffer() {
            if (mapping)
                vkUnmapMemory(device, memory);
            if (buffer)
                vkDestroyBuffer(device, buffer, nullptr);
            if (memory)
                vkFreeMemory(device, memory, nullptr);
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
        std::shared_ptr<const MeshSnapshot> source;
        VkDescriptorPool texture_pool{};
        VkBuffer material_buffer{};
        VkDeviceMemory material_memory{};
        std::vector<VkDescriptorSet> material_sets;
        std::vector<GpuTexture> textures;
        ~GpuMaterials() {
            if (texture_pool)
                vkDestroyDescriptorPool(device, texture_pool, nullptr);
            if (material_buffer)
                vkDestroyBuffer(device, material_buffer, nullptr);
            if (material_memory)
                vkFreeMemory(device, material_memory, nullptr);
            for (auto &texture : textures) {
                if (texture.sampler && !texture.shared_sampler)
                    vkDestroySampler(device, texture.sampler, nullptr);
                if (texture.view)
                    vkDestroyImageView(device, texture.view, nullptr);
                if (texture.image)
                    vkDestroyImage(device, texture.image, nullptr);
                if (texture.memory)
                    vkFreeMemory(device, texture.memory, nullptr);
            }
        }
        GpuMaterials() = default;
        GpuMaterials(const GpuMaterials &) = delete;
        GpuMaterials &operator=(const GpuMaterials &) = delete;
    };
#endif
    struct UploadBatch {
        VkDevice device{};
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        VkBuffer staging_buffer{};
        VkDeviceMemory staging_memory{};
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
                vkDestroyBuffer(device, staging_buffer, nullptr);
            if (staging_memory)
                vkFreeMemory(device, staging_memory, nullptr);
            staging_buffer = VK_NULL_HANDLE;
            staging_memory = VK_NULL_HANDLE;
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
#endif
    std::array<float, 16> view_projection{};
    std::array<float, 4> view_origin{0, 0, -1, 0};

    VkSwapchainKHR swapchain{};
    VkExtent2D extent{};
    VkFormat format{};
    VkRenderPass render_pass{};
    VkPipeline pipeline{}, ui_pipeline{};
    VkImage depth_image{};
    VkDeviceMemory depth_memory{};
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
    VkDeviceMemory capture_memory{};
    void *capture_mapping{};
    bool capture_coherent{};

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
        check(vkCreateInstance(&info, nullptr, &instance), "Create instance");
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
            throw std::runtime_error("No Vulkan 1.1 graphics/present device with swapchain support");
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
        info.pEnabledFeatures = &enabled;
        if (present_fences)
            info.pNext = &maintenance;
        check(vkCreateDevice(physical, &info, nullptr, &device), "Create device");
        vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, present_family, 0, &present_queue);
        std::cout << "Presentation retirement: "
                  << (present_fences ? "EXT_swapchain_maintenance1 fences"
                                     : "Vulkan 1.1 wait-idle fallback (see docs/engine.md)")
                  << '\n';
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
            queries.queryCount = 5;
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
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float)};
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
        check(vkDeviceWaitIdle(device), "Wait before swapchain recreation");
        wait_for_presentation();
        destroy_swapchain();
        extent = selected_extent;
        auto formats = enumerate<VkSurfaceFormatKHR>(
            [&](auto *n, auto *p) { return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, p); },
            "Enumerate surface formats");
        if (formats.empty())
            throw std::runtime_error("Surface has no formats");
        VkSurfaceFormatKHR selected = formats.front();
        if (formats.size() == 1 && selected.format == VK_FORMAT_UNDEFINED)
            selected = {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (auto desired :
             {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            const auto found = std::find_if(formats.begin(), formats.end(), [&](const auto &value) {
                return value.format == desired && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (found != formats.end()) {
                selected = *found;
                break;
            }
        }
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
        const bool capture = !options.capture.empty() && !stats.captured;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error("Surface cannot be a color attachment");
        if (capture && (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                        (format != VK_FORMAT_B8G8R8A8_SRGB && format != VK_FORMAT_R8G8B8A8_SRGB &&
                         format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM)))
            throw std::runtime_error("Capture requires a transferable BGRA/RGBA8 swapchain");
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
        resize = false;
        ++stats.swapchain_generations;
        std::cout << "Swapchain " << stats.swapchain_generations << ": " << extent.width << 'x' << extent.height
                  << " pixels, " << images.size() << " images\n";
        return true;
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
    }
    enum class PipelineKind { diagnostic, ui, resource, sky, shadow_resource };
    void create_pipeline(PipelineKind mode, VkPipeline &output) {
        const bool shadow = mode == PipelineKind::shadow_resource;
#ifdef ANIMA_HAS_ASSETS
        const bool resource = mode == PipelineKind::resource || mode == PipelineKind::shadow_resource;
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
        const VkVertexInputAttributeDescription resource_attributes[]{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, color)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SourceVertex, uv)},
            {4, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(SourceVertex, joints)},
            {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, weights)},
            {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, tangent)},
            {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(SourceVertex, alpha)}};
        if (resource) {
            vertex.vertexBindingDescriptionCount = 1;
            vertex.pVertexBindingDescriptions = &resource_binding;
            vertex.vertexAttributeDescriptionCount = 8;
            vertex.pVertexAttributeDescriptions = resource_attributes;
        }
        const VkVertexInputAttributeDescription shadow_resource_attributes[]{
            resource_attributes[0], resource_attributes[3], resource_attributes[4], resource_attributes[5],
            resource_attributes[7]};
        if (shadow) {
            vertex.vertexAttributeDescriptionCount = 5;
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
        depth_state.depthWriteEnable = !ui && !sky;
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
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = ui;
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
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & required) == required)
                return i;
        throw std::runtime_error("No compatible Vulkan memory type");
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
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device, &image, nullptr, &depth_image), "Create depth image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, depth_image, &requirements);
#ifdef ANIMA_HAS_ASSETS
        depth_allocation_bytes = requirements.size;
#endif
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &depth_memory), "Allocate depth memory");
        check(vkBindImageMemory(device, depth_image, depth_memory, 0), "Bind depth memory");
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
        // Preserve minification with nearest level-zero selection without mipmaps.
        info.maxLod = source.mipmapped ? static_cast<float>(levels - 1) : 0.25F;
        info.maxAnisotropy = 1;
        auto sampler = std::make_shared<GpuSampler>();
        sampler->device = device;
        check(vkCreateSampler(device, &info, nullptr, &sampler->handle), "Create material sampler");
        material_samplers[key] = sampler;
        return sampler;
    }
    void upload_textures(GpuMaterials &target, UploadBatch &upload, RendererFailureStage failure, bool initial,
                         const MeshPreparation *prepared = nullptr) {
        const auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (const auto image_format : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, image_format, &properties);
            if ((properties.optimalTilingFeatures & needed) != needed)
                throw std::runtime_error("GPU lacks filtered RGBA colour/data images");
        }
        const auto generated_plan = prepared
                                        ? MaterialTexturePlan{}
                                        : material_texture_plan(target.source->material_data, target.source->textures);
        const auto &plan = prepared ? prepared->plan() : generated_plan;
        target.textures.resize(plan.images.size());
        std::uint32_t total_mips = 0;
        for (std::size_t i = 0; i < target.textures.size(); ++i) {
            const Texture white{1, 1, {255, 255, 255, 255}, {}};
            const auto &planned = plan.images[i];
            const auto &source = planned.source < 0 ? white : target.source->textures[planned.source];
            const auto generated_mips = prepared ? std::vector<MipLevel>{}
                                        : source.sampler.mipmapped
                                            ? texture_mips(source, planned.mips)
                                            : std::vector<MipLevel>{{source.width, source.height, source.rgba}};
            const auto &mips = prepared ? prepared->images().at(i) : generated_mips;
            total_mips += static_cast<std::uint32_t>(mips.size());
            auto &texture = target.textures[i];
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format =
                source.encoding == TextureEncoding::srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            image.extent = {source.width, source.height, 1};
            image.mipLevels = static_cast<std::uint32_t>(mips.size());
            image.arrayLayers = 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(device, &image, nullptr, &texture.image), "Create texture image");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, texture.image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            texture.allocation_bytes = requirements.size;
            check(vkAllocateMemory(device, &allocation, nullptr, &texture.memory), "Allocate texture memory");
            check(vkBindImageMemory(device, texture.image, texture.memory, 0), "Bind texture memory");
            if (i == 0)
                inject_scene(failure, RendererFailureStage::texture, initial);
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = texture.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = image.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mipLevels, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create texture view");
            texture.shared_sampler = material_sampler(source.sampler, image.mipLevels);
            texture.sampler = texture.shared_sampler->handle;
            VkDeviceSize size = 0;
            for (const auto &mip : mips)
                size += mip.rgba.size();
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = size;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(device, &buffer, nullptr, &upload.staging_buffer), "Create texture staging buffer");
            vkGetBufferMemoryRequirements(device, upload.staging_buffer, &requirements);
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            check(vkAllocateMemory(device, &allocation, nullptr, &upload.staging_memory),
                  "Allocate texture staging memory");
            check(vkBindBufferMemory(device, upload.staging_buffer, upload.staging_memory, 0),
                  "Bind texture staging memory");
            void *mapped = nullptr;
            check(vkMapMemory(device, upload.staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map texture staging");
            std::size_t offset = 0;
            for (const auto &mip : mips) {
                std::memcpy(static_cast<char *>(mapped) + offset, mip.rgba.data(), mip.rgba.size());
                offset += mip.rgba.size();
            }
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = upload.staging_memory;
            range.size = VK_WHOLE_SIZE;
            const auto flushed = vkFlushMappedMemoryRanges(device, 1, &range);
            vkUnmapMemory(device, upload.staging_memory);
            check(flushed, "Flush texture staging");
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
            vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);
            std::vector<VkBufferImageCopy> copies;
            offset = 0;
            for (std::uint32_t level = 0; level < image.mipLevels; ++level) {
                VkBufferImageCopy copy{};
                copy.bufferOffset = offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                copy.imageExtent = {mips[level].width, mips[level].height, 1};
                copies.push_back(copy);
                offset += mips[level].rgba.size();
            }
            vkCmdCopyBufferToImage(upload.command, upload.staging_buffer, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()),
                                   copies.data());
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            check(vkEndCommandBuffer(upload.command), "End texture upload");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &upload.command;
            check(vkResetFences(device, 1, &upload.fence), "Reset upload fence");
            check(vkQueueSubmit(graphics_queue, 1, &submit, upload.fence), "Submit texture upload");
            upload.pending = true;
            if (i == 0) {
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
        check(vkCreateBuffer(device, &buffer_create, nullptr, &target.material_buffer), "Create material buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, target.material_buffer, &requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = requirements.size;
        memory.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        check(vkAllocateMemory(device, &memory, nullptr, &target.material_memory), "Allocate material memory");
        check(vkBindBufferMemory(device, target.material_buffer, target.material_memory, 0), "Bind material memory");
        std::vector<std::byte> uniforms(static_cast<std::size_t>(stride * count));
        for (std::uint32_t i = 0; i < count; ++i) {
            const Material fallback;
            const auto &m = i == 0 ? fallback : target.source->material_data[i - 1];
            const MaterialUniform uniform{{m.emissive.x, m.emissive.y, m.emissive.z, m.alpha},
                                          {m.normal_scale, m.alpha_mode == AlphaMode::mask ? m.alpha_cutoff : -1.F,
                                           m.occlusion_strength, m.unlit ? 1.F : 0.F},
                                          {m.normal_texture >= 0 ? 1.F : 0.F, 0, 0, 0}};
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
        void *mapped{};
        check(vkMapMemory(device, target.material_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map material memory");
        std::memcpy(mapped, uniforms.data(), uniforms.size());
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = target.material_memory;
        range.size = VK_WHOLE_SIZE;
        const auto flushed = vkFlushMappedMemoryRanges(device, 1, &range);
        vkUnmapMemory(device, target.material_memory);
        check(flushed, "Flush material memory");
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
        check(vkCreateBuffer(device, &buffer, nullptr, &capture_buffer), "Create readback buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, capture_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const auto flags = memory.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1U << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i;
                capture_coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if (capture_coherent)
                    break;
            }
        }
        if (type == UINT32_MAX)
            throw std::runtime_error("No host-visible memory for capture");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &allocation, nullptr, &capture_memory), "Allocate readback memory");
        check(vkBindBufferMemory(device, capture_buffer, capture_memory, 0), "Bind readback memory");
        check(vkMapMemory(device, capture_memory, 0, VK_WHOLE_SIZE, 0, &capture_mapping), "Map readback memory");
    }
    void save_capture() {
        check(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, fence_timeout), "Wait for capture");
        if (!capture_coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = capture_memory;
            range.size = VK_WHOLE_SIZE;
            check(vkInvalidateMappedMemoryRanges(device, 1, &range), "Invalidate readback memory");
        }
        if (!options.capture.parent_path().empty())
            std::filesystem::create_directories(options.capture.parent_path());
        std::ofstream output(options.capture, std::ios::binary);
        if (!output)
            throw std::runtime_error("Cannot open capture output: " + options.capture.string());
        output << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        const auto *bytes = static_cast<const unsigned char *>(capture_mapping);
        const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        const auto count = static_cast<std::size_t>(extent.width) * extent.height;
        std::vector<unsigned char> rgb(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            rgb[i * 3] = bytes[i * 4 + (bgra ? 2 : 0)];
            rgb[i * 3 + 1] = bytes[i * 4 + 1];
            rgb[i * 3 + 2] = bytes[i * 4 + (bgra ? 0 : 2)];
        }
        output.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        output.close();
        if (!output)
            throw std::runtime_error("Failed writing capture");
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
            std::array<std::uint64_t, 10> values{};
            const auto result = vkGetQueryPoolResults(device, timing_queries, 0, 5, sizeof(values), values.data(),
                                                      2 * sizeof(std::uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_NOT_READY)
                check(result, "Read GPU timestamps");
            if (result == VK_SUCCESS && values[1] && values[3] && values[5] && values[7] && values[9]) {
                const auto mask = timestamp_bits >= 64 ? UINT64_MAX : (std::uint64_t{1} << timestamp_bits) - 1;
                const auto milliseconds = [&](unsigned a, unsigned b) {
                    return double((values[b * 2] - values[a * 2]) & mask) * timestamp_period / 1e6;
                };
                profile.gpu_ms = milliseconds(0, 4);
                profile.gpu_shadow_ms = milliseconds(0, 1);
                profile.gpu_scene_ms = milliseconds(1, 2);
                profile.gpu_resolve_ms = milliseconds(2, 3);
                profile.gpu_transfer_ms = milliseconds(3, 4);
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
            vkCmdResetQueryPool(command, timing_queries, 0, 5);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing_queries, 0);
        }
#ifdef ANIMA_HAS_ASSETS
        record_shadow();
#endif
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries, 1);
        std::array<VkClearValue, 2> clear{};
        clear[0].color = {{0.018F, 0.027F, 0.041F, 1.0F}};
        clear[1].depthStencil = {1, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = render_pass;
        pass.framebuffer = image.framebuffer;
#ifdef ANIMA_HAS_ASSETS
        pass.framebuffer = world_target->framebuffer;
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
        } else
#endif
            if (options.diagnostic_triangle) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scale), scale);
            vkCmdDraw(command, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(command);
        if (timing_queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries, 2);
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
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries, 3);
        const bool capture = capture_buffer && !stats.captured;
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
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_queries, 4);
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
        if (capture)
            save_capture();
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return false;
        ++stats.presented_frames;
        return true;
    }
    void destroy_swapchain() noexcept {
        if (ui_pipeline)
            vkDestroyPipeline(device, ui_pipeline, nullptr);
        ui_pipeline = VK_NULL_HANDLE;
        if (capture_mapping)
            vkUnmapMemory(device, capture_memory);
        capture_mapping = nullptr;
        if (capture_buffer)
            vkDestroyBuffer(device, capture_buffer, nullptr);
        capture_buffer = VK_NULL_HANDLE;
        if (capture_memory)
            vkFreeMemory(device, capture_memory, nullptr);
        capture_memory = VK_NULL_HANDLE;
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
            vkDestroyImage(device, depth_image, nullptr);
        depth_image = VK_NULL_HANDLE;
        if (depth_memory)
            vkFreeMemory(device, depth_memory, nullptr);
        depth_memory = VK_NULL_HANDLE;
#ifdef ANIMA_HAS_ASSETS
        if (resource_pipeline)
            vkDestroyPipeline(device, resource_pipeline, nullptr);
        resource_pipeline = VK_NULL_HANDLE;
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
            // The smoke runner's process watchdog handles a hung driver during teardown.
            try {
                wait_for_presentation(UINT64_MAX);
            } catch (const std::exception &error) {
                ++errors;
                std::fprintf(stderr, "%s\n", error.what());
            }
            destroy_swapchain();
#ifdef ANIMA_HAS_ASSETS
            resource_instances.clear();
            resource_scenes.clear();
            resource_cache.clear();
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
    impl_->initialize();
}
VulkanRenderer::~VulkanRenderer() = default;
void VulkanRenderer::request_resize() noexcept { impl_->resize = true; }
void VulkanRenderer::request_capture(std::filesystem::path path) {
    if (path.empty())
        throw std::invalid_argument("Capture path is empty");
    impl_->options.capture = std::move(path);
    impl_->stats.captured = false;
    if (!impl_->capture_buffer)
        impl_->resize = true;
}
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
    for (const auto &region : {environment.shadow, environment.detail_shadow})
        if (region.resolution > properties.limits.maxImageDimension2D ||
            region.resolution > properties.limits.maxFramebufferWidth ||
            region.resolution > properties.limits.maxFramebufferHeight)
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
bool VulkanRenderer::draw_ui(const detail::UiFrame &frame) {
    try {
        return impl_->draw(&frame);
    } catch (const VulkanFailure &error) {
        impl_->fatal = true;
        throw RendererFatalError(error.what());
    } catch (...) {
        if (impl_->fatal)
            throw RendererFatalError("Device lost while retiring UI upload");
        throw;
    }
}
#endif
} // namespace anima
