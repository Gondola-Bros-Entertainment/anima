// Linear scene target, its size for the render scale, and final display conversion. Included inside Impl.
// The linear color target's format, which choose_world_color_format() settles at construction.
VkFormat world_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
// Settles world_color_format from RendererOptions::scene_color_format: B10G11R11 where it is requested and the device's
// optimal tiling offers every use that the target and its opaque color copy make of it, and otherwise RGBA16F.
void choose_world_color_format() {
    world_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (options.scene_color_format != SceneColorFormat::b10g11r11)
        return;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &properties);
    // Meshes render and blend into the target and display conversion filters it. Opaque inputs copy it into an image of
    // its format, which custom materials sample, and are required too, so that the smaller format never costs a device
    // the opaque inputs that RGBA16F would give it.
    constexpr VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((properties.optimalTilingFeatures & needed) == needed)
        world_color_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}
// The linear color target with the framebuffer that also attaches the depth target of its size, and the descriptor
// that display conversion samples it through.
struct WorldTarget {
    VkDevice device{};
    VmaAllocator allocator{};
    GpuTexture color;
    VkFramebuffer framebuffer{};
    VkDescriptorPool pool{};
    VkDescriptorSet descriptor{};
    ~WorldTarget() {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (framebuffer)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (color.sampler)
            vkDestroySampler(device, color.sampler, nullptr);
        if (color.view)
            vkDestroyImageView(device, color.view, nullptr);
        if (color.image)
            vmaDestroyImage(allocator, color.image, color.allocation);
    }
};
std::unique_ptr<WorldTarget> world_target;
VkDescriptorSetLayout scene_input_layout{};
VkPipelineLayout post_pipeline_layout{};
VkShaderModule resolve_fragment_shader{};
VkRenderPass present_pass{};
VkPipeline resolve_pipeline{};
// The largest scene targets that the device can create and attach: its 2D image and framebuffer limits.
VkExtent2D max_scene_extent{};
// The push constant of resolve.frag: scene target pixels per window pixel along each axis.
using DisplayFootprint = std::array<float, 2>;

void create_world_layout() {
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &info, nullptr, &scene_input_layout), "Create scene input layout");
    const VkDescriptorSetLayout layouts[]{scene_input_layout, environment_layout};
    // The display shader's footprint (DisplayFootprint).
    const VkPushConstantRange footprint{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DisplayFootprint)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 2;
    layout.pSetLayouts = layouts;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &footprint;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &post_pipeline_layout), "Create display pipeline layout");
}
// The scene targets' size for a swapchain of @p size: each side times the render scale, rounded to the nearest pixel
// and kept from 1 to max_scene_extent.
VkExtent2D scene_size(VkExtent2D size) const {
    const auto scaled = [&](std::uint32_t pixels, std::uint32_t limit) {
        const double rounded = std::round(double(pixels) * double(options.render_scale));
        return static_cast<std::uint32_t>(std::clamp(rounded, 1.0, double(limit)));
    };
    return {scaled(size.width, max_scene_extent.width), scaled(size.height, max_scene_extent.height)};
}
void create_world_image(GpuTexture &target, VkExtent2D size) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, world_color_format, &properties);
    // Blended meshes composite into the target, which the display conversion then samples.
    const auto needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("Device lacks filtered, blendable RGBA16F world targets");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = world_color_format;
    image.extent = {size.width, size.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // Custom materials that read opaque color sample a copy of this image.
    if (opaque_copies_supported())
        image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    target.allocation_bytes = create_image(image, target.image, target.allocation, "Create linear world image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = world_color_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &target.view), "Create linear world view");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampler, nullptr, &target.sampler), "Create world sampler");
}
VkRenderPass create_display_pass() {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency incoming{};
    incoming.srcSubpass = VK_SUBPASS_EXTERNAL;
    incoming.dstSubpass = 0;
    incoming.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    incoming.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    incoming.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    incoming.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &incoming;
    VkRenderPass pass{};
    check(vkCreateRenderPass(device, &info, nullptr, &pass), "Create display pass");
    return pass;
}
// Creates the world target of @p size, whose framebuffer attaches @p depth_view, a depth target of that size. Throws
// `std::runtime_error` when the device lacks the target's format or a Vulkan call fails, releasing what it created.
std::unique_ptr<WorldTarget> create_world_target(VkImageView depth_view, VkExtent2D size) {
    auto next = std::make_unique<WorldTarget>();
    next->device = device;
    next->allocator = allocator;
    create_world_image(next->color, size);
    const VkImageView views[]{next->color.view, depth_view};
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = render_pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views;
    framebuffer.width = size.width;
    framebuffer.height = size.height;
    framebuffer.layers = 1;
    check(vkCreateFramebuffer(device, &framebuffer, nullptr, &next->framebuffer), "Create world framebuffer");
    const VkDescriptorPoolSize sampled{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &sampled;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &next->pool), "Create world descriptor pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = next->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &scene_input_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &next->descriptor), "Allocate world descriptor");
    const VkDescriptorImageInfo image{next->color.sampler, next->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = next->descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return next;
}
// Gives the scene targets the size that the render scale sets for the swapchain: creates the depth and world targets
// after a swapchain creation, which destroyed the previous ones, and replaces them when that size changes. The replaced
// targets and the opaque input copies, which the next frame that reads them creates at the new size, are destroyed once
// no frame in flight can use them. A failure keeps the current targets, or none after a swapchain creation, and the
// next call tries again.
void ensure_scene_targets() {
    const auto size = scene_size(extent);
    if (size.width == scene_extent.width && size.height == scene_extent.height)
        return;
    auto next_depth = create_depth(size);
    // Between the two creations, so that an injected failure releases the new depth target alone, as a failure to
    // create the world target does.
    if (options.render_scale > 1)
        fail_after(RendererFailureStage::scene_targets);
    auto next_world = create_world_target(next_depth->view, size);
    depth_target.swap(next_depth);
    world_target.swap(next_world);
    scene_extent = size;
    release_after_frames(std::move(next_depth));
    release_after_frames(std::move(next_world));
    release_after_frames(std::move(opaque_inputs));
}
void create_display_pipeline() {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto &s : stages) {
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vertex_shader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = resolve_fragment_shader;
    const VkBool32 encode = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM);
    const VkSpecializationMapEntry entry{0, 0, sizeof(encode)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(encode), &encode};
    stages[1].pSpecializationInfo = &specialization;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = post_pipeline_layout;
    info.renderPass = present_pass;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &resolve_pipeline),
          "Create world output pipeline");
}
void record_display(VkFramebuffer framebuffer) {
    const auto command = frame().command;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = present_pass;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = extent;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    // The swapchain's size, which UI also draws at, rather than the scene targets' that the world pass set. The
    // fullscreen triangle maps the whole world target onto it: a target no larger than the window is interpolated
    // through normalized coordinates, and a larger one averaged over each window pixel's area, which the footprint
    // measures in target pixels.
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const DisplayFootprint footprint{static_cast<float>(double(scene_extent.width) / extent.width),
                                     static_cast<float>(double(scene_extent.height) / extent.height)};
    const VkDescriptorSet sets[]{world_target->descriptor, frame().environment_set};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_layout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(command, post_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(footprint),
                       footprint.data());
    vkCmdDraw(command, 3, 1, 0, 0);
    // Caller draws UI here, in display colour, then ends this render pass.
}
void finish_world_writes() {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(frame().command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void destroy_world_targets() noexcept {
    destroy_opaque_inputs();
    world_target.reset();
    if (resolve_pipeline)
        vkDestroyPipeline(device, resolve_pipeline, nullptr);
    if (present_pass)
        vkDestroyRenderPass(device, present_pass, nullptr);
    resolve_pipeline = VK_NULL_HANDLE;
    present_pass = VK_NULL_HANDLE;
}
