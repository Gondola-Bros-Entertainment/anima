// Custom materials: application shaders drawn through pipelines, passes and descriptors that the renderer owns.
// Included inside Impl. custom_material.hpp documents the interface that these structures feed.

// The frame block of custom shaders, at set 0, binding 0.
struct CustomFrameUniform {
    Mat4 view_projection, inverse_view_projection;
    std::array<float, 4> view_origin, sun_direction, sun_radiance, fill_direction, fill_radiance, ambient_sky,
        ambient_ground, ambient_specular, fog, viewport;
    float time;
    std::array<float, 3> padding;
    std::array<float, 4> fog_shape, fog_sun;
};
static_assert(offsetof(CustomFrameUniform, inverse_view_projection) == 64 &&
              offsetof(CustomFrameUniform, view_origin) == 128 && offsetof(CustomFrameUniform, sun_direction) == 144 &&
              offsetof(CustomFrameUniform, ambient_specular) == 240 && offsetof(CustomFrameUniform, fog) == 256 &&
              offsetof(CustomFrameUniform, viewport) == 272 && offsetof(CustomFrameUniform, time) == 288 &&
              offsetof(CustomFrameUniform, fog_shape) == 304 && offsetof(CustomFrameUniform, fog_sun) == 320 &&
              sizeof(CustomFrameUniform) == 336);
// The per-draw push constants of custom shaders.
struct CustomPush {
    Mat4 view;
    std::uint32_t palette_offset, skinned, object_offset, placed;
    std::array<float, 4> factor;
    std::uint32_t ranged;
};
static_assert(offsetof(CustomPush, palette_offset) == 64 && offsetof(CustomPush, skinned) == 68 &&
              offsetof(CustomPush, object_offset) == 72 && offsetof(CustomPush, placed) == 76 &&
              offsetof(CustomPush, factor) == 80 && offsetof(CustomPush, ranged) == 96 && sizeof(CustomPush) == 100);
// Descriptor bindings of a custom material's set: its parameter block, then its textures.
static constexpr std::uint32_t custom_material_bindings = CustomMaterial::max_textures + 1;

// GPU state of one CustomMaterial, cached per object as meshes are. Its pipelines are compatible with every world
// and shadow pass, whose attachment formats never change, so they last as long as the material.
struct GpuCustomMaterial {
    VkDevice device{};
    VmaAllocator allocator{};
    std::shared_ptr<const CustomMaterial> source;
    VkShaderModule vertex{}, fragment{}, shadow_vertex{}, shadow_fragment{};
    std::unique_ptr<ResourceBuffer> parameters;
    // Images and views are owned; samplers are the shared material samplers.
    std::vector<GpuTexture> textures;
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    // The main-view pipeline and the depth-only variant's.
    VkPipeline pipeline{}, shadow_pipeline{};
    ~GpuCustomMaterial() {
        for (const auto handle : {pipeline, shadow_pipeline})
            if (handle)
                vkDestroyPipeline(device, handle, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        for (const auto shader : {vertex, fragment, shadow_vertex, shadow_fragment})
            if (shader)
                vkDestroyShaderModule(device, shader, nullptr);
        for (auto &texture : textures) {
            if (texture.view)
                vkDestroyImageView(device, texture.view, nullptr);
            if (texture.image)
                vmaDestroyImage(allocator, texture.image, texture.allocation);
        }
    }
    GpuCustomMaterial() = default;
    GpuCustomMaterial(const GpuCustomMaterial &) = delete;
    GpuCustomMaterial &operator=(const GpuCustomMaterial &) = delete;
};
// Copies of opaque depth and color that blended and additive custom materials sample, of the scene targets' size.
struct OpaqueInputs {
    VkDevice device{};
    VmaAllocator allocator{};
    GpuTexture depth, color;
    ~OpaqueInputs() {
        for (auto *texture : {&depth, &color}) {
            if (texture->sampler)
                vkDestroySampler(device, texture->sampler, nullptr);
            if (texture->view)
                vkDestroyImageView(device, texture->view, nullptr);
            if (texture->image)
                vmaDestroyImage(allocator, texture->image, texture->allocation);
        }
    }
};
VkDescriptorSetLayout custom_frame_layout{}, custom_material_layout{};
VkPipelineLayout custom_pipeline_layout{};
// Holds each frame slot's frame set (FrameSlot::custom_frame_set).
VkDescriptorPool custom_frame_pool{};
std::map<const CustomMaterial *, std::unique_ptr<GpuCustomMaterial>> custom_cache;
// Seconds that VulkanRenderer::set_time received.
float shader_time{};
// Created on the first frame that draws a custom material reading opaque inputs, released with the swapchain or when
// ensure_scene_targets() resizes the scene targets.
std::unique_ptr<OpaqueInputs> opaque_inputs;
// Counts the opaque input copies created, so that each frame slot's frame set can tell whether it points at the current
// ones (FrameSlot::opaque_generation).
std::uint64_t opaque_generation{};
// World passes that split the frame around the opaque input copy. They are compatible with render_pass: the same
// attachments and subpass, with other load and store operations and layouts.
VkRenderPass opaque_pass{}, composite_pass{};

// Whether the world color and depth formats can be copied, and the depth format sampled, for opaque inputs. The
// world targets are created as copy sources only when they can.
bool opaque_copies_supported() const {
    VkFormatProperties color_features{}, depth_features{};
    vkGetPhysicalDeviceFormatProperties(physical, world_color_format, &color_features);
    vkGetPhysicalDeviceFormatProperties(physical, depth_format, &depth_features);
    const auto copy = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const auto sampled_copy = copy | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (color_features.optimalTilingFeatures & copy) == copy &&
           (depth_features.optimalTilingFeatures & sampled_copy) == sampled_copy;
}

void create_custom_layout() {
    const VkDescriptorSetLayoutBinding frame[]{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<std::uint32_t>(std::size(frame));
    layout.pBindings = frame;
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &custom_frame_layout), "Create custom frame layout");
    std::array<VkDescriptorSetLayoutBinding, custom_material_bindings> material{};
    for (std::uint32_t i = 0; i < material.size(); ++i)
        material[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layout.bindingCount = static_cast<std::uint32_t>(material.size());
    layout.pBindings = material.data();
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &custom_material_layout),
          "Create custom material layout");
    const VkDescriptorSetLayout layouts[]{custom_frame_layout, pose_layout, custom_material_layout};
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CustomPush)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<std::uint32_t>(std::size(layouts));
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &info, nullptr, &custom_pipeline_layout), "Create custom pipeline layout");
    // Each set's opaque depth and color copies.
    constexpr std::uint32_t opaque_input_count = 2;
    const auto slots = static_cast<std::uint32_t>(frames.size());
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slots},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, opaque_input_count * slots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = slots;
    pool.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &custom_frame_pool), "Create custom frame pool");
    for (auto &slot : frames) {
        slot.custom_frame_buffer = resource_buffer(sizeof(CustomFrameUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = custom_frame_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &custom_frame_layout;
        check(vkAllocateDescriptorSets(device, &allocate, &slot.custom_frame_set), "Allocate custom frame set");
        // Bindings 1 and 2 are written when the opaque input copies exist; only materials that read them use them.
        const VkDescriptorBufferInfo buffer{slot.custom_frame_buffer->buffer, 0, sizeof(CustomFrameUniform)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = slot.custom_frame_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}
// Writes the frame block into the slot that records the next frame, whose previous frame has finished.
void update_custom_frame() {
    const auto vec = [](Vec3 v) { return std::array<float, 4>{v.x, v.y, v.z, 0}; };
    const auto &e = environment;
    // The view's pixels, which gl_FragCoord counts and the opaque input copies hold.
    const auto width = static_cast<float>(scene_extent.width), height = static_cast<float>(scene_extent.height);
    const CustomFrameUniform data{view_projection,
                                  inverse_view,
                                  view_origin,
                                  vec(normalized(e.sun.direction)),
                                  vec(sunlight),
                                  vec(normalized(e.fill.direction)),
                                  vec(e.fill.radiance),
                                  vec(e.ambient_sky),
                                  vec(e.ambient_ground),
                                  vec(e.ambient_specular),
                                  {e.fog_color.x, e.fog_color.y, e.fog_color.z, e.fog_density},
                                  {width, height, 1 / width, 1 / height},
                                  shader_time,
                                  {},
                                  fog_shape(e),
                                  fog_sunlight(e, sunlight)};
    auto &block = *frame().custom_frame_buffer;
    std::memcpy(block.mapping, &data, sizeof(data));
    flush_resource(block);
}
VkShaderModule custom_module(std::span<const std::uint32_t> words) const {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size_bytes();
    info.pCode = words.data();
    VkShaderModule shader{};
    check(vkCreateShaderModule(device, &info, nullptr, &shader), "Create custom shader module");
    return shader;
}
// The GPU state of @p material with its pipelines, created and uploaded on first use. Throws `std::invalid_argument`
// for a texture beyond the device's image limits and `std::runtime_error` for failed Vulkan calls; a failure leaves
// nothing cached for @p material.
GpuCustomMaterial &gpu_custom(const std::shared_ptr<const CustomMaterial> &material) {
    if (const auto found = custom_cache.find(material.get()); found != custom_cache.end())
        return *found->second;
    const auto &definition = material->definition();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    for (const auto &texture : definition.textures)
        if (texture.image->width > properties.limits.maxImageDimension2D ||
            texture.image->height > properties.limits.maxImageDimension2D)
            throw std::invalid_argument("Custom material texture exceeds device image dimensions");
    // Read the texels before creating anything, since a material with TexelRetention::until_upload may have let
    // them go.
    const auto texels = material->texel_images();
    auto value = std::make_unique<GpuCustomMaterial>();
    value->device = device;
    value->allocator = allocator;
    value->source = material;
    value->vertex = custom_module(definition.vertex_shader);
    value->fragment = custom_module(definition.fragment_shader);
    if (!definition.shadow_vertex_shader.empty())
        value->shadow_vertex = custom_module(definition.shadow_vertex_shader);
    if (!definition.shadow_fragment_shader.empty())
        value->shadow_fragment = custom_module(definition.shadow_fragment_shader);
    if (!definition.parameters.empty()) {
        value->parameters = resource_buffer(definition.parameters.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::memcpy(value->parameters->mapping, definition.parameters.data(), definition.parameters.size());
        flush_resource(*value->parameters);
    }
    if (!definition.textures.empty()) {
        require_texture_formats();
        // UploadBatch retires submitted work before the material can unwind.
        UploadBatch upload;
        upload.device = device;
        upload.allocator = allocator;
        upload.fatal = &fatal;
        upload.errors = &errors;
        upload.create(graphics_family);
        value->textures.resize(definition.textures.size());
        for (std::size_t i = 0; i < definition.textures.size(); ++i) {
            auto texture = definition.textures[i];
            texture.image = texels[i];
            (void)upload_image(value->textures[i], texture, nullptr, {}, upload, RendererFailureStage::none, false,
                               false);
        }
    }
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CustomMaterial::max_textures}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &value->pool), "Create custom material pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = value->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &custom_material_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &value->set), "Allocate custom material set");
    // Validation limited the shaders to the supplied parameters and textures, so the bindings left unwritten here
    // are never used.
    std::array<VkDescriptorImageInfo, CustomMaterial::max_textures> image_infos{};
    std::vector<VkWriteDescriptorSet> writes;
    const VkDescriptorBufferInfo buffer{value->parameters ? value->parameters->buffer : VK_NULL_HANDLE, 0,
                                        definition.parameters.size()};
    if (value->parameters) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = value->set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        writes.push_back(write);
    }
    for (std::size_t i = 0; i < value->textures.size(); ++i) {
        const auto &texture = value->textures[i];
        image_infos[i] = {texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = value->set;
        write.dstBinding = static_cast<std::uint32_t>(i + 1);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_infos[i];
        writes.push_back(write);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    if (value->shadow_vertex)
        create_custom_pipeline(*value, true);
    create_custom_pipeline(*value, false);
    auto &cached = *custom_cache.emplace(material.get(), std::move(value)).first->second;
    // Every copy has completed, so a material with TexelRetention::until_upload no longer needs its texels.
    material->release_texels();
    return cached;
}
// Creates @p gpu's main-view pipeline, or with @p shadow its depth-only variant's.
void create_custom_pipeline(GpuCustomMaterial &gpu, bool shadow) {
    const auto &source = *gpu.source;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto &stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shadow ? gpu.shadow_vertex : gpu.vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shadow ? gpu.shadow_fragment : gpu.fragment;
    // Only the attributes that the vertex shader declares, so that no supplied attribute goes unread, and the
    // bindings they read: the mesh's vertices and the placement rows.
    const auto declared = shadow ? source.shadow_vertex_attributes() : source.vertex_attributes();
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    for (const auto &binding : resource_bindings) {
        const auto first = attributes.size();
        const std::span<const VkVertexInputAttributeDescription> candidates =
            binding.binding == 0 ? std::span<const VkVertexInputAttributeDescription>(resource_attributes)
                                 : std::span<const VkVertexInputAttributeDescription>(placement_attributes);
        for (const auto &attribute : candidates)
            if (declared & (1U << attribute.location))
                attributes.push_back(attribute);
        if (attributes.size() != first)
            bindings.push_back(binding);
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex.pVertexAttributeDescriptions = attributes.data();
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
    // Opaque materials write depth; blended and additive ones test it without writing, as blended materials do.
    const bool opaque = shadow || source.blend() == CustomBlend::opaque;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = opaque ? VK_TRUE : VK_FALSE;
    // Reversed depth in the view; shadow maps keep forward depth, as the built-in shadow pass does.
    depth.depthCompareOp = shadow ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = opaque ? VK_FALSE : VK_TRUE;
    attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor =
        source.blend() == CustomBlend::additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = shadow ? 0 : 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    // A depth-only variant without a fragment shader writes the depth of every covered pixel.
    info.stageCount = stages[1].module ? 2 : 1;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = custom_pipeline_layout;
    info.renderPass = shadow ? shadow_pass : render_pass;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                    shadow ? &gpu.shadow_pipeline : &gpu.pipeline),
          "Create custom material pipeline");
}
// Binds @p gpu's pipeline, or with @p shadow its depth-only variant's, with the frame, pose and material sets.
void bind_custom(const GpuCustomMaterial &gpu, bool shadow) {
    const auto command = frame().command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? gpu.shadow_pipeline : gpu.pipeline);
    const VkDescriptorSet sets[]{frame().custom_frame_set, frame().pose_set, gpu.set};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, custom_pipeline_layout, 0,
                            static_cast<std::uint32_t>(std::size(sets)), sets, 0, nullptr);
}
// Records draw @p draw_index of @p render_instance with the bound custom pipeline and geometry, through @p view, once
// per run of placement_runs.
void record_custom_draw(const ResourceInstance &render_instance, std::size_t draw_index, const Mat4 &view,
                        bool shadow) {
    const auto &source = *render_instance.source;
    const auto &draw = source.asset->draws()[draw_index];
    const auto factor = source.factors.at(static_cast<std::size_t>(draw.material));
    const CustomPush push{view,
                          render_instance.palette_offset + draw.palette_offset,
                          draw.skinned ? 1U : 0U,
                          render_instance.object_offset,
                          render_instance.placements ? 1U : 0U,
                          {factor.x, factor.y, factor.z, 1},
                          render_instance.ranged ? 1U : 0U};
    vkCmdPushConstants(frame().command, custom_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    record_indexed(render_instance, draw_index, shadow);
}
// Records @p draws in order through @p view, culled by @p frustum when given: opaque custom materials in the main view,
// or with @p shadow the depth-only variants that a shadow pass's frustum contains.
void record_custom_draws(const std::vector<CustomDraw> &draws, const Mat4 &view, const RenderFrustum *frustum,
                         bool shadow) {
    const GpuCustomMaterial *bound_material = nullptr;
    const ResourceInstance *bound_geometry = nullptr;
    for (const auto &entry : draws) {
        const auto &render_instance = resource_instances[entry.instance];
        if (shadow && !frustum->intersects(render_instance.source->primitive_bounds[entry.draw]))
            continue;
        find_placement_runs(render_instance, frustum);
        if (placement_runs.empty())
            continue;
        if (entry.custom != bound_material) {
            bound_material = entry.custom;
            bind_custom(*bound_material, shadow);
        }
        if (&render_instance != bound_geometry) {
            bound_geometry = &render_instance;
            bind_geometry(render_instance);
        }
        record_custom_draw(render_instance, entry.draw, view, shadow);
    }
}
// Creates the passes that split a frame around the opaque input copy from @p world, render_pass's description.
// They differ from render_pass only in load and store operations and layouts, so they are compatible with it and
// with its framebuffers and pipelines. The opaque pass begins as render_pass does, then stores both attachments and
// leaves them in their attachment layouts for copy_opaque_inputs(); the composite pass loads them back and ends as
// render_pass does.
void create_opaque_input_passes(VkRenderPassCreateInfo world) {
    const std::array<VkAttachmentDescription, 2> ending{world.pAttachments[0], world.pAttachments[1]};
    auto attachments = ending;
    world.pAttachments = attachments.data();
    auto &color = attachments[0], &depth = attachments[1];
    color.storeOp = depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    check(vkCreateRenderPass(device, &world, nullptr, &opaque_pass), "Create opaque world pass");
    attachments = ending;
    color.loadOp = depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    check(vkCreateRenderPass(device, &world, nullptr, &composite_pass), "Create composite world pass");
}
void destroy_opaque_input_passes() noexcept {
    for (auto *pass : {&opaque_pass, &composite_pass}) {
        if (*pass)
            vkDestroyRenderPass(device, *pass, nullptr);
        *pass = VK_NULL_HANDLE;
    }
}
// Creates the opaque depth and color copies for the current scene targets, if they do not exist, and points the frame
// set of the slot that records the next frame at them, unless it already does. Throws `std::runtime_error` when the
// device cannot copy or sample them.
void ensure_opaque_inputs() {
    if (!opaque_inputs)
        create_opaque_inputs();
    auto &slot = frame();
    if (slot.opaque_generation == opaque_generation)
        return;
    const std::array<VkDescriptorImageInfo, 2> descriptors{
        VkDescriptorImageInfo{opaque_inputs->depth.sampler, opaque_inputs->depth.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{opaque_inputs->color.sampler, opaque_inputs->color.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = slot.custom_frame_set;
        writes[i].dstBinding = i + 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &descriptors[i];
    }
    // The slot's previous frame has finished, so nothing reads its frame set.
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    slot.opaque_generation = opaque_generation;
}
// Creates the opaque depth and color copies of the scene targets' size. Throws `std::runtime_error` when the device
// cannot copy or sample them.
void create_opaque_inputs() {
    if (!opaque_copies_supported())
        throw std::runtime_error("Device cannot copy the world color and depth that custom materials read");
    auto next = std::make_unique<OpaqueInputs>();
    next->device = device;
    next->allocator = allocator;
    const auto create = [&](GpuTexture &texture, VkFormat image_format, VkImageAspectFlags aspect, VkFilter filter) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = image_format;
        image.extent = {scene_extent.width, scene_extent.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        texture.allocation_bytes = create_image(image, texture.image, texture.allocation, "Create opaque input image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image_format;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create opaque input view");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = filter;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device, &sampler, nullptr, &texture.sampler), "Create opaque input sampler");
    };
    // Depth formats need not filter linearly, so depth samples the nearest texel.
    create(next->depth, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FILTER_NEAREST);
    create(next->color, world_color_format, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_LINEAR);
    opaque_inputs.swap(next);
    ++opaque_generation;
}
// Records the copy between the opaque pass, which stored the world color and depth, and the composite pass, which
// loads them back. The copies become readable by fragment shaders.
void copy_opaque_inputs() {
    const auto command = frame().command;
    const auto &inputs = *opaque_inputs;
    const auto barrier = [](VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                            VkAccessFlags source, VkAccessFlags destination) {
        VkImageMemoryBarrier value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        value.srcAccessMask = source;
        value.dstAccessMask = destination;
        value.oldLayout = from;
        value.newLayout = to;
        value.srcQueueFamilyIndex = value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        value.image = image;
        value.subresourceRange = {aspect, 0, 1, 0, 1};
        return value;
    };
    // The opaque pass's depth and color writes, including its stores, precede the copy. Earlier frames' fragment
    // shaders last read the copies, whose contents are replaced whole; the barrier's first scope holds every command
    // submitted before it, those of a frame still in flight included.
    const std::array sources{barrier(depth_target->image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                     VK_ACCESS_TRANSFER_READ_BIT),
                             barrier(world_target->color.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
                             barrier(inputs.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
                             barrier(inputs.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)};
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(sources.size()), sources.data());
    const auto region = [&](VkImageAspectFlags aspect) {
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {aspect, 0, 0, 1};
        copy.extent = {scene_extent.width, scene_extent.height, 1};
        return copy;
    };
    const auto depth_region = region(VK_IMAGE_ASPECT_DEPTH_BIT), color_region = region(VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImage(command, depth_target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, inputs.depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depth_region);
    vkCmdCopyImage(command, world_target->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, inputs.color.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &color_region);
    // The copies go to fragment shaders; the targets return to the attachment layouts that the composite pass loads.
    const std::array handoff{
        barrier(inputs.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        barrier(inputs.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        barrier(depth_target->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
        barrier(world_target->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(handoff.size()), handoff.data());
}
