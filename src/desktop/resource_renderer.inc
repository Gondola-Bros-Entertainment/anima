// Included inside VulkanRenderer::Impl. No backend types enter the public scene API.
struct GpuAsset {
    std::shared_ptr<const Mesh> asset;
    std::unique_ptr<ResourceBuffer> vertices, indices;
    std::unique_ptr<GpuMaterials> materials;
};
struct ResourceInstance {
    std::shared_ptr<GpuAsset> gpu;
    const Scene::Instance *source{};
    std::uint32_t palette_offset{};
    std::size_t first_draw{}, draw_count{}, first_shadow{}, shadow_count{};
};
struct ResourcePush {
    Mat4 view;
    std::array<float, 4> origin, surface;
    std::array<std::uint32_t, 4> indices;
    std::array<float, 4> factor;
};
static_assert(sizeof(ResourcePush) == 128 && offsetof(ResourcePush, indices) == 96 &&
              offsetof(ResourcePush, factor) == 112 && sizeof(Mat4) == 64);
static_assert(sizeof(SourceVertex) == 96 && offsetof(SourceVertex, joints) == 44 &&
              offsetof(SourceVertex, weights) == 60);
VkShaderModule resource_vertex_shader{};
VkPipelineLayout resource_pipeline_layout{};
VkDescriptorSetLayout pose_layout{};
VkPipeline resource_pipeline{};
VkDescriptorPool pose_pool{};
VkDescriptorSet pose_set{};
std::vector<std::shared_ptr<const Scene>> resource_scenes;
std::map<const Mesh *, std::shared_ptr<GpuAsset>> resource_cache;
std::unique_ptr<ResourceBuffer> pose_buffer;
std::vector<ResourceInstance> resource_instances;
std::vector<std::size_t> resource_draws, resource_shadow_draws;
RenderFrustum resource_frustum;
ResourceStats resources{};

std::unique_ptr<ResourceBuffer> resource_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags properties) {
    auto value = std::make_unique<ResourceBuffer>();
    value->device = device;
    value->bytes = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &info, nullptr, &value->buffer), "Create resource buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, value->buffer, &requirements);
    value->allocation_bytes = requirements.size;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, properties);
    check(vkAllocateMemory(device, &allocation, nullptr, &value->memory), "Allocate resource buffer");
    check(vkBindBufferMemory(device, value->buffer, value->memory, 0), "Bind resource buffer");
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        check(vkMapMemory(device, value->memory, 0, VK_WHOLE_SIZE, 0, &value->mapping), "Map resource buffer");
    return value;
}
void flush_resource(ResourceBuffer &buffer) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer.memory;
    range.size = VK_WHOLE_SIZE;
    check(vkFlushMappedMemoryRanges(device, 1, &range), "Flush resource buffer");
}
void upload_resource_buffer(ResourceBuffer &destination, const void *data, VkAccessFlags access) {
    auto staging =
        resource_buffer(destination.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    std::memcpy(staging->mapping, data, static_cast<std::size_t>(destination.bytes));
    flush_resource(*staging);
    // UploadBatch retires submitted work before staging or the candidate can unwind.
    UploadBatch upload;
    upload.device = device;
    upload.fatal = &fatal;
    upload.errors = &errors;
    upload.create(graphics_family);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(upload.command, &begin), "Begin geometry upload");
    const VkBufferCopy copy{0, 0, destination.bytes};
    vkCmdCopyBuffer(upload.command, staging->buffer, destination.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = access;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    check(vkEndCommandBuffer(upload.command), "End geometry upload");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload.command;
    check(vkQueueSubmit(graphics_queue, 1, &submit, upload.fence), "Submit geometry upload");
    upload.pending = true;
    upload.wait();
}
void create_resource_layout() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.limits.maxPushConstantsSize < sizeof(ResourcePush) ||
        properties.limits.maxPerStageDescriptorStorageBuffers < 1 || properties.limits.maxBoundDescriptorSets < 3 ||
        properties.limits.maxVertexInputAttributes < 8 ||
        properties.limits.maxVertexInputBindingStride < sizeof(SourceVertex))
        throw std::runtime_error("Device lacks resource renderer interface limits");
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = sizeof(resource_vertex_code);
    shader.pCode = resource_vertex_code;
    check(vkCreateShaderModule(device, &shader, nullptr, &resource_vertex_shader), "Create resource vertex shader");
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                               nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount = 1;
    descriptor.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &pose_layout), "Create pose layout");
    const VkDescriptorSetLayout layouts[]{texture_layout, environment_layout, pose_layout};
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResourcePush)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 3;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &info, nullptr, &resource_pipeline_layout), "Create resource pipeline layout");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &pose_pool), "Create pose descriptor pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pose_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &pose_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &pose_set), "Allocate pose descriptor");
}
std::shared_ptr<GpuAsset> gpu_asset(const std::shared_ptr<const Mesh> &asset, RendererFailureStage failure,
                                    const MeshPreparation *prepared = nullptr) {
    if (auto found = resource_cache.find(asset.get()); found != resource_cache.end())
        return found->second;
    auto value = std::make_shared<GpuAsset>();
    value->asset = asset;
    const auto vertices = asset->vertices().size_bytes(), indices = asset->indices().size_bytes();
    value->materials = std::make_unique<GpuMaterials>();
    value->materials->device = device;
    value->materials->source = asset->materials();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (!asset->vertices().empty() && asset->vertices().size() - 1 > properties.limits.maxDrawIndexedIndexValue)
        throw std::invalid_argument("Resource index exceeds device indexed-draw range");
    for (const auto &image : asset->materials()->textures)
        if (image.width > properties.limits.maxImageDimension2D || image.height > properties.limits.maxImageDimension2D)
            throw std::invalid_argument("Resource texture exceeds device image dimensions");
    if (vertices && indices) {
        value->vertices =
            resource_buffer(vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        upload_resource_buffer(*value->vertices, asset->vertices().data(), VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        inject_scene(failure, RendererFailureStage::vertex, false);
        value->indices = resource_buffer(indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        upload_resource_buffer(*value->indices, asset->indices().data(), VK_ACCESS_INDEX_READ_BIT);
        inject_scene(failure, RendererFailureStage::index, false);
        UploadBatch upload;
        upload.device = device;
        upload.fatal = &fatal;
        upload.errors = &errors;
        upload.create(graphics_family);
        upload_textures(*value->materials, upload, failure, false, prepared);
    }
    resource_cache.emplace(asset.get(), value);
    ++resources.mesh_uploads;
    resources.geometry_uploaded_bytes += vertices + indices;
    return value;
}
void prepare_resources(std::span<const std::shared_ptr<const Scene>> sources, RendererFailureStage failure = {},
                       bool use_frustum = true) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::vector<Mat4> palettes;
    std::vector<ResourceInstance> instances;
    std::vector<std::size_t> draws, shadows;
    std::uint64_t candidate_instances = 0, culled_instances = 0, candidate_draws = 0, culled_draws = 0;
    for (const auto &source : sources)
        for (auto id : source->instances()) {
            const auto &render_instance = source->instance(id);
            if (!render_instance.visible || !render_instance.active || render_instance.asset->draws().empty())
                continue;
            const auto first_draw = draws.size(), first_shadow = shadows.size();
            bool candidate = false;
            const auto source_draws = render_instance.asset->draws();
            const bool cull = use_frustum && options.frustum_culling;
            const bool outside = cull && !resource_frustum.intersects(render_instance.bounds);
            for (std::size_t i = 0; i < source_draws.size(); ++i) {
                if (!render_instance.primitive_visible[i] || !source_draws[i].index_count)
                    continue;
                candidate = true;
                ++candidate_draws;
                if (outside || (cull && !resource_frustum.intersects(render_instance.primitive_bounds[i])))
                    ++culled_draws;
                else
                    draws.push_back(i);
                const auto material_index = source_draws[i].material;
                const bool unlit =
                    material_index >= 0 && render_instance.asset->materials()->material_data[material_index].unlit;
                if (!unlit &&
                    ((environment.shadow.enabled && shadow_frustum.intersects(render_instance.primitive_bounds[i])) ||
                     (environment.detail_shadow.enabled &&
                      detail_shadow_frustum.intersects(render_instance.primitive_bounds[i]))))
                    shadows.push_back(i);
            }
            if (!candidate)
                continue;
            ++candidate_instances;
            if (draws.size() == first_draw)
                ++culled_instances;
            if (draws.size() == first_draw && shadows.size() == first_shadow)
                continue;
            const auto max_matrices = properties.limits.maxStorageBufferRange / sizeof(Mat4);
            if (palettes.size() > max_matrices || render_instance.palette.size() > max_matrices - palettes.size())
                throw std::length_error("Instance palettes exceed device storage-buffer range");
            instances.push_back({gpu_asset(render_instance.asset, failure), &render_instance,
                                 static_cast<std::uint32_t>(palettes.size()), first_draw, draws.size() - first_draw,
                                 first_shadow, shadows.size() - first_shadow});
            palettes.insert(palettes.end(), render_instance.palette.begin(), render_instance.palette.end());
        }
    const VkDeviceSize bytes = palettes.size() * sizeof(Mat4);
    if (bytes && (!pose_buffer || pose_buffer->bytes < bytes)) {
        // Grow only the small pose buffer; mesh resources and instance IDs stay put.
        VkDeviceSize capacity = pose_buffer ? pose_buffer->bytes : sizeof(Mat4);
        while (capacity < bytes)
            capacity = std::min<VkDeviceSize>(capacity * 2, properties.limits.maxStorageBufferRange);
        auto candidate =
            resource_buffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        inject_scene(failure, RendererFailureStage::palette, false);
        const VkDescriptorBufferInfo buffer{candidate->buffer, 0, capacity};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = pose_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        pose_buffer.swap(candidate); // Existing graphics fence has retired all users.
    }
    if (bytes) {
        std::memcpy(pose_buffer->mapping, palettes.data(), bytes);
        flush_resource(*pose_buffer);
    }
    inject_scene(failure, RendererFailureStage::ready, false);
    resource_instances.swap(instances);
    resource_draws.swap(draws);
    resource_shadow_draws.swap(shadows);
    resources.pose_uploaded_bytes = bytes;
    resources.instances = candidate_instances - culled_instances;
    resources.candidate_instances = candidate_instances;
    resources.culled_instances = culled_instances;
    resources.candidate_draws = candidate_draws;
    resources.culled_draws = culled_draws;
}
void set_scenes(std::vector<std::shared_ptr<const Scene>> sources, SceneReplacementOptions settings,
                bool initial = false) {
    running();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i])
            throw std::invalid_argument("Renderer selection contains a null scene");
        if (std::find(sources.begin(), sources.begin() + i, sources[i]) != sources.begin() + i)
            throw std::invalid_argument("Renderer selection contains a duplicate scene");
    }
    const auto &failure = settings.fail_after;
    if (failure != RendererFailureStage::none && failure != RendererFailureStage::vertex &&
        failure != RendererFailureStage::index && failure != RendererFailureStage::texture &&
        failure != RendererFailureStage::texture_upload && failure != RendererFailureStage::descriptors &&
        failure != RendererFailureStage::palette && failure != RendererFailureStage::ready &&
        failure != RendererFailureStage::upload_timeout && failure != RendererFailureStage::device_lost)
        throw std::invalid_argument("Unknown resource failure stage");
    try {
        check(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, fence_timeout), "Wait before resource scene selection");
        check(vkResetCommandBuffer(command, 0), "Reset commands before resource scene selection");
        // Selection preflights resources independently of the previous camera.
        // Visibility is evaluated again with the current camera at each draw.
        if (!sources.empty())
            prepare_resources(sources, failure, false);
        else {
            inject_scene(failure, RendererFailureStage::ready, initial);
            resource_instances.clear();
            resource_draws.clear();
            resource_shadow_draws.clear();
            resources.pose_uploaded_bytes = resources.instances = 0;
            resources.candidate_instances = resources.culled_instances = 0;
            resources.candidate_draws = resources.culled_draws = 0;
            resources.draw_calls = resources.submitted_indices = 0;
            resources.shadow_draw_calls = resources.shadow_submitted_indices = 0;
        }
    } catch (const VulkanFailure &error) {
        if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        throw std::runtime_error(error.what()); // VulkanFailure is private.
    } catch (const InjectedRendererFailure &error) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource upload");
        throw InjectedRendererFailure(error.stage(), initial);
    } catch (...) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource upload");
        throw;
    }
    resource_scenes = std::move(sources);
    ++stats.scene_generations;
}
void prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets, const ResourcePreparationOptions &settings,
                    const MeshPreparation *prepared = nullptr) {
    running();
    const auto &failure = settings.fail_after;
    if (failure != RendererFailureStage::none && failure != RendererFailureStage::vertex &&
        failure != RendererFailureStage::index && failure != RendererFailureStage::texture &&
        failure != RendererFailureStage::texture_upload && failure != RendererFailureStage::descriptors &&
        failure != RendererFailureStage::ready && failure != RendererFailureStage::upload_timeout &&
        failure != RendererFailureStage::device_lost)
        throw std::invalid_argument("Unknown resource preparation failure stage");
    for (const auto &asset : assets)
        if (!asset)
            throw std::invalid_argument("Cannot prepare a null render asset");
    if (assets.empty())
        return;
    try {
        check(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, fence_timeout), "Wait before resource preparation");
        for (const auto &asset : assets)
            (void)gpu_asset(asset, failure, prepared);
        inject_scene(failure, RendererFailureStage::ready, false);
    } catch (const VulkanFailure &error) {
        if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        throw std::runtime_error(error.what()); // VulkanFailure is private.
    } catch (...) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource preparation");
        throw;
    }
}
void record_resources(bool shadow = false, bool detail = false) {
    if (!shadow) {
        resources.draw_calls = 0;
        resources.submitted_indices = 0;
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? shadow_resource_pipeline : resource_pipeline);
    for (const auto &render_instance : resource_instances) {
        const auto &gpu = *render_instance.gpu;
        const auto &source = *render_instance.source;
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &gpu.vertices->buffer, &offset);
        vkCmdBindIndexBuffer(command, gpu.indices->buffer, 0, VK_INDEX_TYPE_UINT32);
        const auto first = shadow ? render_instance.first_shadow : render_instance.first_draw;
        const auto count = shadow ? render_instance.shadow_count : render_instance.draw_count;
        for (std::size_t i = first; i < first + count; ++i) {
            const auto draw_index = (shadow ? resource_shadow_draws : resource_draws)[i];
            if (shadow &&
                !(detail ? detail_shadow_frustum : shadow_frustum).intersects(source.primitive_bounds[draw_index]))
                continue;
            const auto &draw = source.asset->draws()[draw_index];
            const Material fallback;
            const auto &material =
                draw.material < 0 ? fallback : source.asset->materials()->material_data.at(draw.material);
            const auto factor = draw.material < 0 ? Vec3{1, 1, 1} : source.factors.at(draw.material);
            const ResourcePush push{
                shadow ? (detail ? detail_shadow_view : shadow_view) : view_projection,
                view_origin,
                {material.metallic, material.roughness, 0, 0},
                {render_instance.palette_offset + draw.palette_offset, draw.skinned ? 1U : 0U, 0, 0},
                {factor.x, factor.y, factor.z, 1}};
            vkCmdPushConstants(command, resource_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            const VkDescriptorSet sets[]{gpu.materials->material_sets.at(std::size_t(draw.material + 1)),
                                         environment_set, pose_set};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resource_pipeline_layout, 0, 3, sets, 0,
                                    nullptr);
            vkCmdDrawIndexed(command, draw.index_count, 1, draw.first_index, 0, 0);
            if (shadow) {
                ++resources.shadow_draw_calls;
                resources.shadow_submitted_indices += draw.index_count;
            } else {
                ++resources.draw_calls;
                resources.submitted_indices += draw.index_count;
            }
        }
    }
}
ResourceStats resource_stats() const {
    auto value = resources;
    value.resident_material_samplers = std::count_if(material_samplers.begin(), material_samplers.end(),
                                                     [](const auto &entry) { return !entry.second.expired(); });
    value.cached_assets = resource_cache.size();
    value.resident_geometry_bytes = value.resident_texture_bytes = 0;
    for (const auto &[key, gpu] : resource_cache) {
        (void)key;
        if (gpu->vertices)
            value.resident_geometry_bytes += gpu->vertices->allocation_bytes + gpu->indices->allocation_bytes;
        for (const auto &texture : gpu->materials->textures)
            value.resident_texture_bytes += texture.allocation_bytes;
    }
    value.shadow_bytes = shadow_target ? shadow_target->texture.allocation_bytes : 0;
    value.shadow_bytes += detail_shadow_target ? detail_shadow_target->texture.allocation_bytes : 0;
    value.pose_buffer_bytes = pose_buffer ? pose_buffer->allocation_bytes : 0;
    return value;
}
void retire_resources() {
    resource_instances.clear();
    resource_draws.clear();
    resource_shadow_draws.clear();
    for (auto it = resource_cache.begin(); it != resource_cache.end();) {
        if (it->second->asset.use_count() == 1)
            it = resource_cache.erase(it);
        else
            ++it;
    }
}
