// Linear scene target and final display conversion. Included inside Impl.
static constexpr VkFormat world_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
struct WorldTarget {
    VkDevice device{};
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
            vkDestroyImage(device, color.image, nullptr);
        if (color.memory)
            vkFreeMemory(device, color.memory, nullptr);
    }
};
std::unique_ptr<WorldTarget> world_target;
VkDescriptorSetLayout scene_input_layout{};
VkPipelineLayout post_pipeline_layout{};
VkShaderModule resolve_fragment_shader{};
VkRenderPass present_pass{};
VkPipeline resolve_pipeline{};
VkDeviceSize depth_allocation_bytes{};

void create_world_layout() {
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &info, nullptr, &scene_input_layout), "Create scene input layout");
    const VkDescriptorSetLayout layouts[]{scene_input_layout, environment_layout};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 2;
    layout.pSetLayouts = layouts;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &post_pipeline_layout), "Create display pipeline layout");
}
void create_world_image(GpuTexture &target) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, world_color_format, &properties);
    const auto needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("Device lacks filtered RGBA16F world targets");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = world_color_format;
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(device, &image, nullptr, &target.image), "Create linear world image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, target.image, &requirements);
    target.allocation_bytes = requirements.size;
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &memory, nullptr, &target.memory), "Allocate linear world image");
    check(vkBindImageMemory(device, target.image, target.memory, 0), "Bind linear world image");
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
void create_world_targets() {
    present_pass = create_display_pass();
    auto next = std::make_unique<WorldTarget>();
    next->device = device;
    create_world_image(next->color);
    const VkImageView views[]{next->color.view, depth_view};
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = render_pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views;
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    check(vkCreateFramebuffer(device, &framebuffer, nullptr, &next->framebuffer), "Create world framebuffer");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
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
    world_target.swap(next);
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
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = present_pass;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = extent;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkDescriptorSet sets[]{world_target->descriptor, environment_set};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_layout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    // Caller draws UI here, in display colour, then ends this render pass.
}
void finish_world_writes() {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void destroy_world_targets() noexcept {
    world_target.reset();
    if (resolve_pipeline)
        vkDestroyPipeline(device, resolve_pipeline, nullptr);
    if (present_pass)
        vkDestroyRenderPass(device, present_pass, nullptr);
    resolve_pipeline = VK_NULL_HANDLE;
    present_pass = VK_NULL_HANDLE;
}
