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
    // Ranges of resource_draws, the instance's opaque and masked main-view draws, and of resource_shadow_draws.
    std::size_t first_draw{}, draw_count{}, first_shadow{}, shadow_count{};
};
struct GpuCustomMaterial;
// A main-view draw that composites over the target instead of replacing it, recorded after every opaque and
// masked draw in the order sort_back_to_front() gives: a blended material's, or a blended or additive custom
// material's.
struct BlendedDraw {
    // Index into resource_instances.
    std::size_t instance{};
    // Index into the instance's Mesh::draws().
    std::size_t draw{};
    // blend_order_key() of the draw's world bounds; larger is farther.
    double key{};
    // The custom material that draws it, or null for the draw's Material.
    const GpuCustomMaterial *custom{};
};
// A draw of an opaque custom material in the main view, or of a custom material's depth-only variant.
struct CustomDraw {
    // Index into resource_instances.
    std::size_t instance{};
    // Index into the instance's Mesh::draws().
    std::size_t draw{};
    const GpuCustomMaterial *custom{};
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
// SourceVertex attributes that the resource vertex shader reads, by location.
static constexpr auto resource_attributes = std::to_array<VkVertexInputAttributeDescription>({
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, position)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, normal)},
    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, color)},
    {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SourceVertex, uv)},
    {4, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(SourceVertex, joints)},
    {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, weights)},
    {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, tangent)},
    {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(SourceVertex, alpha)},
});
VkShaderModule resource_vertex_shader{};
VkPipelineLayout resource_pipeline_layout{};
VkDescriptorSetLayout pose_layout{};
VkPipeline resource_pipeline{}, blended_resource_pipeline{};
VkDescriptorPool pose_pool{};
VkDescriptorSet pose_set{};
std::vector<std::shared_ptr<const Scene>> resource_scenes;
std::map<const Mesh *, std::shared_ptr<GpuAsset>> resource_cache;
std::unique_ptr<ResourceBuffer> pose_buffer;
std::vector<ResourceInstance> resource_instances;
std::vector<std::size_t> resource_draws, resource_shadow_draws;
std::vector<BlendedDraw> resource_blended_draws;
std::vector<CustomDraw> resource_custom_draws, resource_custom_shadow_draws;
// Whether a blended or additive custom material in resource_blended_draws reads opaque depth or color.
bool resource_opaque_inputs{};
RenderFrustum resource_frustum;
ResourceStats resources{};

// The distance by which blended draws sort, from the center of @p bounds: squared from the eye in a perspective
// view, or along the view direction in an orthographic one, where view_origin holds the unit direction toward the
// camera. Either way a larger key is farther. Doubles keep the squares of finite float coordinates finite.
double blend_order_key(const RenderBounds &bounds) const {
    const std::array<double, 3> center{(double(bounds.minimum.x) + bounds.maximum.x) / 2,
                                       (double(bounds.minimum.y) + bounds.maximum.y) / 2,
                                       (double(bounds.minimum.z) + bounds.maximum.z) / 2};
    double key = 0;
    if (view_origin[3] != 0)
        for (std::size_t axis = 0; axis < center.size(); ++axis) {
            const auto offset = center[axis] - view_origin[axis];
            key += offset * offset;
        }
    else
        for (std::size_t axis = 0; axis < center.size(); ++axis)
            key -= center[axis] * view_origin[axis];
    return key;
}
// Orders @p draws back to front. The sort is stable, so draws with equal keys keep selection, instance and draw
// order, and their order cannot change from one frame to the next.
static void sort_back_to_front(std::vector<BlendedDraw> &draws) {
    std::stable_sort(draws.begin(), draws.end(),
                     [](const BlendedDraw &a, const BlendedDraw &b) { return a.key > b.key; });
}

std::unique_ptr<ResourceBuffer> resource_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags properties) {
    auto value = std::make_unique<ResourceBuffer>();
    value->allocator = allocator;
    value->bytes = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocated = create_buffer(info, properties, value->buffer, value->allocation, "Create resource buffer");
    value->allocation_bytes = allocated.size;
    value->mapping = allocated.pMappedData;
    return value;
}
void flush_resource(ResourceBuffer &buffer) {
    check(vmaFlushAllocation(allocator, buffer.allocation, 0, VK_WHOLE_SIZE), "Flush resource buffer");
}
void upload_resource_buffer(ResourceBuffer &destination, const void *data, VkAccessFlags access) {
    auto staging =
        resource_buffer(destination.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    std::memcpy(staging->mapping, data, static_cast<std::size_t>(destination.bytes));
    flush_resource(*staging);
    // UploadBatch retires submitted work before staging or the candidate can unwind.
    UploadBatch upload;
    upload.device = device;
    upload.allocator = allocator;
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
        properties.limits.maxVertexInputAttributes < resource_attributes.size() ||
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
    value->materials->allocator = allocator;
    value->materials->source = asset->materials();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (!asset->vertices().empty() && asset->vertices().size() - 1 > properties.limits.maxDrawIndexedIndexValue)
        throw std::invalid_argument("Resource index exceeds device indexed-draw range");
    for (const auto &texture : asset->materials()->textures)
        if (texture.image->width > properties.limits.maxImageDimension2D ||
            texture.image->height > properties.limits.maxImageDimension2D)
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
        upload.allocator = allocator;
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
    std::vector<BlendedDraw> blended;
    std::vector<CustomDraw> custom_draws, custom_shadows;
    std::uint64_t candidate_instances = 0, culled_instances = 0, candidate_draws = 0, culled_draws = 0;
    for (const auto &source : sources)
        for (auto id : source->instances()) {
            const auto &render_instance = source->instance(id);
            if (!render_instance.visible || !render_instance.active || render_instance.asset->draws().empty())
                continue;
            const auto first_draw = draws.size(), first_shadow = shadows.size(), first_blended = blended.size(),
                       first_custom = custom_draws.size(), first_custom_shadow = custom_shadows.size();
            bool candidate = false;
            const auto source_draws = render_instance.asset->draws();
            const bool cull = use_frustum && options.frustum_culling;
            const bool outside = cull && !resource_frustum.intersects(render_instance.bounds);
            for (std::size_t i = 0; i < source_draws.size(); ++i) {
                if (!render_instance.primitive_visible[i] || !source_draws[i].index_count)
                    continue;
                candidate = true;
                ++candidate_draws;
                const auto material_index = source_draws[i].material;
                const auto *material =
                    material_index < 0 ? nullptr : &render_instance.asset->materials()->material_data[material_index];
                // A custom material in the draw's material slot replaces its Material in every pass.
                const auto *custom =
                    material_index < 0 ? nullptr : render_instance.custom_materials[material_index].get();
                const bool blend = custom ? custom->blend() != CustomBlend::opaque
                                          : material && material->alpha_mode == AlphaMode::blend;
                const auto &bounds = render_instance.primitive_bounds[i];
                if (outside || (cull && !resource_frustum.intersects(bounds)))
                    ++culled_draws;
                else if (blend) // Every nonempty draw has valid bounds.
                    blended.push_back(
                        {instances.size(), i, blend_order_key(bounds),
                         custom ? &gpu_custom(render_instance.custom_materials[material_index]) : nullptr});
                else if (custom)
                    custom_draws.push_back(
                        {instances.size(), i, &gpu_custom(render_instance.custom_materials[material_index])});
                else
                    draws.push_back(i);
                // A depth-only shadow map holds neither unlit surfaces nor partial transmission; a custom material
                // casts through its depth-only variant.
                const bool casts = custom ? custom->casts_shadows() : !material || !(material->unlit || blend);
                if (casts && render_instance.casts_shadows &&
                    ((environment.shadow.enabled && shadow_frustum.intersects(bounds)) ||
                     (environment.detail_shadow.enabled && detail_shadow_frustum.intersects(bounds)))) {
                    if (custom)
                        custom_shadows.push_back(
                            {instances.size(), i, &gpu_custom(render_instance.custom_materials[material_index])});
                    else
                        shadows.push_back(i);
                }
            }
            if (!candidate)
                continue;
            ++candidate_instances;
            const bool main_view =
                draws.size() != first_draw || blended.size() != first_blended || custom_draws.size() != first_custom;
            if (!main_view)
                ++culled_instances;
            // An instance with blended or custom draws is always kept, so their instance index above stays valid.
            if (!main_view && shadows.size() == first_shadow && custom_shadows.size() == first_custom_shadow)
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
    sort_back_to_front(blended);
    // The frame copies opaque depth and color only when a drawn custom material reads them; draw() allocates the
    // copies.
    const bool reads_opaque = std::any_of(blended.begin(), blended.end(), [](const BlendedDraw &draw) {
        return draw.custom && (draw.custom->source->reads_opaque_depth() || draw.custom->source->reads_opaque_color());
    });
    inject_scene(failure, RendererFailureStage::ready, false);
    resource_instances.swap(instances);
    resource_draws.swap(draws);
    resource_shadow_draws.swap(shadows);
    resource_blended_draws.swap(blended);
    resource_custom_draws.swap(custom_draws);
    resource_custom_shadow_draws.swap(custom_shadows);
    resource_opaque_inputs = resources.opaque_inputs = reads_opaque;
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
            resource_blended_draws.clear();
            resource_custom_draws.clear();
            resource_custom_shadow_draws.clear();
            resource_opaque_inputs = resources.opaque_inputs = false;
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
void bind_geometry(const GpuAsset &gpu) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &gpu.vertices->buffer, &offset);
    vkCmdBindIndexBuffer(command, gpu.indices->buffer, 0, VK_INDEX_TYPE_UINT32);
}
// Records draw @p draw_index of @p render_instance with the bound pipeline and geometry, through @p view, and
// counts it in the shadow or main-view statistics.
void record_draw(const ResourceInstance &render_instance, std::size_t draw_index, const Mat4 &view, bool shadow) {
    const auto &source = *render_instance.source;
    const auto &draw = source.asset->draws()[draw_index];
    const Material fallback;
    const auto &material = draw.material < 0 ? fallback : source.asset->materials()->material_data.at(draw.material);
    const auto factor = draw.material < 0 ? Vec3{1, 1, 1} : source.factors.at(draw.material);
    const ResourcePush push{view,
                            view_origin,
                            {material.metallic, material.roughness, 0, 0},
                            {render_instance.palette_offset + draw.palette_offset, draw.skinned ? 1U : 0U, 0, 0},
                            {factor.x, factor.y, factor.z, 1}};
    vkCmdPushConstants(command, resource_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    const VkDescriptorSet sets[]{render_instance.gpu->materials->material_sets.at(std::size_t(draw.material + 1)),
                                 environment_set, pose_set};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resource_pipeline_layout, 0, 3, sets, 0, nullptr);
    record_indexed(draw, shadow);
}
// Records @p draw with the bound pipeline, geometry, descriptors and push constants, and counts it in the shadow or
// main-view statistics.
void record_indexed(const IndexedDraw &draw, bool shadow) {
    vkCmdDrawIndexed(command, draw.index_count, 1, draw.first_index, 0, 0);
    if (shadow) {
        ++resources.shadow_draw_calls;
        resources.shadow_submitted_indices += draw.index_count;
    } else {
        ++resources.draw_calls;
        resources.submitted_indices += draw.index_count;
    }
}
// Records one shadow region's casters or, when @p shadow is false, the main view: every opaque and masked draw in
// selection, instance and draw order, then the opaque custom material draws, then, unless the frame copies opaque
// inputs first, the blended draws back to front.
void record_resources(bool shadow = false, bool detail = false) {
    if (!shadow) {
        resources.draw_calls = 0;
        resources.submitted_indices = 0;
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? shadow_resource_pipeline : resource_pipeline);
    const auto &view = shadow ? (detail ? detail_shadow_view : shadow_view) : view_projection;
    for (const auto &render_instance : resource_instances) {
        const auto first = shadow ? render_instance.first_shadow : render_instance.first_draw;
        const auto count = shadow ? render_instance.shadow_count : render_instance.draw_count;
        if (!count)
            continue;
        bind_geometry(*render_instance.gpu);
        for (std::size_t i = first; i < first + count; ++i) {
            const auto draw_index = (shadow ? resource_shadow_draws : resource_draws)[i];
            if (shadow && !(detail ? detail_shadow_frustum : shadow_frustum)
                               .intersects(render_instance.source->primitive_bounds[draw_index]))
                continue;
            record_draw(render_instance, draw_index, view, shadow);
        }
    }
    record_custom_draws(shadow ? resource_custom_shadow_draws : resource_custom_draws, view, shadow, detail);
    if (!shadow && !resource_opaque_inputs)
        record_blended();
}
// Records resource_blended_draws in order, each through the blended pipeline or its custom material's. Depth then
// holds every opaque and masked surface, which these pipelines test against without writing.
void record_blended() {
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    const GpuAsset *bound = nullptr;
    for (const auto &blended : resource_blended_draws) {
        const auto &render_instance = resource_instances[blended.instance];
        const auto pipeline = blended.custom ? blended.custom->pipeline : blended_resource_pipeline;
        if (pipeline != bound_pipeline) {
            if (blended.custom)
                bind_custom(*blended.custom, false);
            else
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, blended_resource_pipeline);
            bound_pipeline = pipeline;
        }
        if (render_instance.gpu.get() != bound) {
            bound = render_instance.gpu.get();
            bind_geometry(*bound);
        }
        if (blended.custom)
            record_custom_draw(render_instance, blended.draw, view_projection, false);
        else
            record_draw(render_instance, blended.draw, view_projection, false);
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
    value.cached_custom_materials = custom_cache.size();
    for (const auto &[key, gpu] : custom_cache) {
        (void)key;
        for (const auto &texture : gpu->textures)
            value.resident_texture_bytes += texture.allocation_bytes;
    }
    value.opaque_input_bytes =
        opaque_inputs ? opaque_inputs->color.allocation_bytes + opaque_inputs->depth.allocation_bytes : 0;
    value.shadow_bytes = shadow_target ? shadow_target->texture.allocation_bytes : 0;
    value.shadow_bytes += detail_shadow_target ? detail_shadow_target->texture.allocation_bytes : 0;
    value.pose_buffer_bytes = pose_buffer ? pose_buffer->allocation_bytes : 0;
    return value;
}
void retire_resources() {
    resource_instances.clear();
    resource_draws.clear();
    resource_shadow_draws.clear();
    resource_blended_draws.clear();
    resource_custom_draws.clear();
    resource_custom_shadow_draws.clear();
    resource_opaque_inputs = false;
    for (auto it = resource_cache.begin(); it != resource_cache.end();) {
        if (it->second->asset.use_count() == 1)
            it = resource_cache.erase(it);
        else
            ++it;
    }
    for (auto it = custom_cache.begin(); it != custom_cache.end();) {
        if (it->second->source.use_count() == 1)
            it = custom_cache.erase(it);
        else
            ++it;
    }
}
