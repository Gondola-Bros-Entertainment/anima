// Bloom on the linear scene target (VulkanRenderer::set_bloom): the bright pass into the first level of a chain of
// halving levels, a downsample into each further level, and an upsample that adds each level to the one before it, from
// the coarsest up. Display conversion adds the first level to the exposed scene. Included inside Impl.
// The bloom chain's format, which choose_bloom_format() settles at construction.
VkFormat bloom_format = VK_FORMAT_R16G16B16A16_SFLOAT;
// Settles bloom_format: B10G11R11 where the device's optimal tiling can render, blend and linearly filter it, which
// halves the bytes that the passes move, and otherwise RGBA16F, which Vulkan requires to offer those uses.
void choose_bloom_format() {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &properties);
    // Every pass renders a level, upsamples blend onto one, and the next pass and display conversion filter it.
    constexpr VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    bloom_format = (properties.optimalTilingFeatures & needed) == needed ? VK_FORMAT_B10G11R11_UFLOAT_PACK32
                                                                         : VK_FORMAT_R16G16B16A16_SFLOAT;
}
// The bloom chain for scene targets of scene_size: one image whose mip levels halve from half that size down to 1
// pixel, as Vulkan sizes mip levels, rounding down to at least 1, with a view, a framebuffer and a descriptor of each
// level, since each pass draws one level and samples another.
struct BloomChain {
    VkDevice device{};
    VmaAllocator allocator{};
    VkExtent2D scene_size{};
    VkImage image{};
    VmaAllocation allocation{};
    VkDeviceSize allocation_bytes{};
    VkSampler sampler{};
    VkDescriptorPool pool{};
    // Each level's size, view, framebuffer and descriptor, from the first, the largest.
    std::vector<VkExtent2D> extents;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkDescriptorSet> descriptors;
    ~BloomChain() {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        for (const auto framebuffer : framebuffers)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        for (const auto view : views)
            vkDestroyImageView(device, view, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (image)
            vmaDestroyImage(allocator, image, allocation);
    }
    BloomChain() = default;
    BloomChain(const BloomChain &) = delete;
    BloomChain &operator=(const BloomChain &) = delete;
};
// Exists while bloom is enabled, from the first draw() that prepares a frame (ensure_bloom()).
std::unique_ptr<BloomChain> bloom_chain;
// The bloom passes, compatible with each other and so with every level's framebuffer: bloom_down_pass replaces a
// level's contents and bloom_up_pass loads them for the upsample to add to. Both keep the level in its attachment
// layout, which record_bloom()'s barriers move it into and out of.
VkRenderPass bloom_down_pass{}, bloom_up_pass{};
VkPipelineLayout bloom_pipeline_layout{};
VkShaderModule bloom_downsample_shader{}, bloom_upsample_shader{};
VkPipeline bloom_bright_pipeline{}, bloom_downsample_pipeline{}, bloom_upsample_pipeline{};
// The push constants of bloom.glsl's BloomPass: the size of a texel of the image that a pass reads, in texture
// coordinates, the bright pass's exposure, threshold and knee width, and the weight of the level that an upsample adds.
struct BloomPush {
    std::array<float, 2> texel;
    float exposure, threshold, knee, weight;
};
static_assert(sizeof(BloomPush) == 6 * sizeof(float), "bloom.glsl's BloomPass holds a vec2 and four floats");
// The levels that a frame's bloom sums, and the weight of the coarsest of them.
struct BloomLevels {
    std::uint32_t count{};
    float coarsest_weight{};
};

// Creates a bloom pass that draws one level, which it takes and leaves in its attachment layout. A pass that loads the
// level keeps what its downsample wrote; one that does not replaces it whole.
VkRenderPass create_bloom_pass(bool load) const {
    VkAttachmentDescription level{};
    level.format = bloom_format;
    level.samples = VK_SAMPLE_COUNT_1_BIT;
    level.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    level.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    level.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    level.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    level.initialLayout = level.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &level;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass{};
    check(vkCreateRenderPass(device, &info, nullptr, &pass), "Create bloom pass");
    return pass;
}
// Returns a new pipeline, which the caller owns, that draws a fullscreen triangle with @p fragment into a level: the
// downsample shader, as the bright pass where @p bright_pass, into bloom_down_pass, or the upsample shader, adding its
// output to the level, into bloom_up_pass.
[[nodiscard]] VkPipeline create_bloom_pipeline(VkShaderModule fragment, bool bright_pass, bool additive) const {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto &stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vertex_shader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    const VkBool32 bright = bright_pass ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entry{ANIMA_SPEC_BLOOM_BRIGHT_PASS, 0, sizeof(bright)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(bright), &bright};
    if (fragment == bloom_downsample_shader)
        stages[1].pSpecializationInfo = &specialization;
    GraphicsPipelineState state;
    state.stages = stages;
    // The levels hold light in red, green and blue; the packed format has no alpha to write.
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    if (additive) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    state.blend = blend;
    state.layout = bloom_pipeline_layout;
    state.pass = additive ? bloom_up_pass : bloom_down_pass;
    return create_graphics_pipeline(state, "Create bloom pipeline");
}
// Creates the bloom passes and their pipelines, which read their source through the scene input layout
// (create_world_layout()) and draw with the sky's fullscreen triangle. The chain's format depends on the device alone,
// so they last as long as the renderer.
void create_bloom(std::span<const std::uint32_t> downsample_code, std::span<const std::uint32_t> upsample_code) {
    choose_bloom_format();
    const auto make = [&](std::span<const std::uint32_t> code, VkShaderModule &shader) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size_bytes();
        info.pCode = code.data();
        check(vkCreateShaderModule(device, &info, nullptr, &shader), "Create bloom shader");
    };
    make(downsample_code, bloom_downsample_shader);
    make(upsample_code, bloom_upsample_shader);
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    static_assert(ANIMA_SET_SCENE_INPUT == 0, "The bloom pipeline layout holds the scene input set alone");
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &scene_input_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &bloom_pipeline_layout), "Create bloom pipeline layout");
    bloom_down_pass = create_bloom_pass(false);
    bloom_up_pass = create_bloom_pass(true);
    bloom_bright_pipeline = create_bloom_pipeline(bloom_downsample_shader, true, false);
    bloom_downsample_pipeline = create_bloom_pipeline(bloom_downsample_shader, false, false);
    bloom_upsample_pipeline = create_bloom_pipeline(bloom_upsample_shader, false, true);
}
void destroy_bloom() noexcept {
    bloom_chain.reset();
    for (auto *value : {&bloom_bright_pipeline, &bloom_downsample_pipeline, &bloom_upsample_pipeline}) {
        if (*value)
            vkDestroyPipeline(device, *value, nullptr);
        *value = VK_NULL_HANDLE;
    }
    for (auto *pass : {&bloom_down_pass, &bloom_up_pass}) {
        if (*pass)
            vkDestroyRenderPass(device, *pass, nullptr);
        *pass = VK_NULL_HANDLE;
    }
    if (bloom_pipeline_layout)
        vkDestroyPipelineLayout(device, bloom_pipeline_layout, nullptr);
    bloom_pipeline_layout = VK_NULL_HANDLE;
    for (auto *shader : {&bloom_downsample_shader, &bloom_upsample_shader}) {
        if (*shader)
            vkDestroyShaderModule(device, *shader, nullptr);
        *shader = VK_NULL_HANDLE;
    }
}
// Creates the bloom chain for scene targets of @p scene. Throws `std::runtime_error` when a Vulkan call fails,
// releasing what it created.
std::unique_ptr<BloomChain> create_bloom_chain(VkExtent2D scene) {
    auto next = std::make_unique<BloomChain>();
    next->device = device;
    next->allocator = allocator;
    next->scene_size = scene;
    const VkExtent2D first{std::max(scene.width / 2, 1U), std::max(scene.height / 2, 1U)};
    // One level for each halving of the larger side down to 1 pixel.
    const auto levels = static_cast<std::uint32_t>(std::bit_width(std::max(first.width, first.height)));
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = bloom_format;
    image.extent = {first.width, first.height, 1};
    image.mipLevels = levels;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    next->allocation_bytes = create_image(image, next->image, next->allocation, "Create bloom image");
    // Each descriptor's view holds one level, so samples read that level, interpolated bilinearly and clamped to its
    // edges.
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampler, nullptr, &next->sampler), "Create bloom sampler");
    const VkDescriptorPoolSize sampled{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, levels};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = levels;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &sampled;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &next->pool), "Create bloom descriptor pool");
    for (std::uint32_t level = 0; level < levels; ++level) {
        const VkExtent2D size{std::max(first.width >> level, 1U), std::max(first.height >> level, 1U)};
        next->extents.push_back(size);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = next->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = bloom_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        VkImageView created_view{};
        check(vkCreateImageView(device, &view, nullptr, &created_view), "Create bloom view");
        next->views.push_back(created_view);
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = bloom_down_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &created_view;
        framebuffer.width = size.width;
        framebuffer.height = size.height;
        framebuffer.layers = 1;
        VkFramebuffer created_framebuffer{};
        check(vkCreateFramebuffer(device, &framebuffer, nullptr, &created_framebuffer), "Create bloom framebuffer");
        next->framebuffers.push_back(created_framebuffer);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = next->pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &scene_input_layout;
        VkDescriptorSet descriptor{};
        check(vkAllocateDescriptorSets(device, &allocate, &descriptor), "Allocate bloom descriptor");
        next->descriptors.push_back(descriptor);
        const VkDescriptorImageInfo sampled_level{next->sampler, created_view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor;
        write.dstBinding = ANIMA_SCENE_INPUT_COLOR;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled_level;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    return next;
}
// Gives the scene targets a bloom chain of their size while bloom is enabled, replacing one of another size, and
// releases the chain while bloom is disabled. A replaced or released chain is destroyed once no frame in flight can use
// it. A failure keeps the current chain, and the next call tries again.
void ensure_bloom() {
    if (!options.bloom.enabled) {
        release_after_frames(std::move(bloom_chain));
        return;
    }
    if (bloom_chain && bloom_chain->scene_size.width == scene_extent.width &&
        bloom_chain->scene_size.height == scene_extent.height)
        return;
    auto next = create_bloom_chain(scene_extent);
    bloom_chain.swap(next);
    release_after_frames(std::move(next));
}
// The levels that the bloom sums for its radius: `ceil(log2(radius * height))` of the chain's, at least 1, and the
// weight of the coarsest, the fraction by which `log2(radius * height)` passes the count less 1, which is 1 for a
// single level. The count grows by one as that logarithm passes a whole number, where the new level's weight starts
// from 0, so the sum changes continuously with the radius and the scene targets' height.
[[nodiscard]] BloomLevels bloom_levels() const {
    const auto available = static_cast<double>(bloom_chain->extents.size());
    const double exact = std::log2(double(options.bloom.radius) * double(scene_extent.height));
    const double count = std::clamp(std::ceil(exact), 1.0, available);
    const double weight = count == 1 ? 1.0 : std::clamp(exact - (count - 1), 0.0, 1.0);
    return {static_cast<std::uint32_t>(count), static_cast<float>(weight)};
}
// The factor of the chain's first level that display conversion adds to the exposed scene: the intensity over the
// total weight of the levels that the first level sums, or 0 without a chain.
[[nodiscard]] float bloom_factor() const {
    if (!bloom_chain)
        return 0;
    const auto levels = bloom_levels();
    return options.bloom.intensity / (static_cast<float>(levels.count - 1) + levels.coarsest_weight);
}
// Records into @p command, the frame's scene batch, the bloom passes after the world pass, while there is a chain: the
// bright pass from the scene target into the first level, a downsample into each further level that bloom_levels()
// sums, and from the coarsest of those up, an upsample of each into the level before it, which leaves the first level
// for display conversion to sample.
//
// A pipeline barrier hands each image from the pass that writes it to the one that samples or loads it, as the shadow
// maps' barriers do, rather than the passes' own subpass dependencies and layout transitions: MoltenVK 1.4.1 on an
// Apple M2 does not order a pass that samples an image after a render pass of the same command buffer that wrote it
// through that render pass's outgoing dependency alone, so without a barrier the bright pass can read regions of the
// scene target that the world pass has not yet written, those of an expensive draw first.
void record_bloom(VkCommandBuffer command) {
    if (!bloom_chain)
        return;
    const auto &chain = *bloom_chain;
    const auto levels = bloom_levels();
    // Each barrier follows the fragment shaders that sampled the images that it changes and the passes that wrote
    // them, and precedes the fragment shaders that sample them next and the passes that draw them.
    const auto pipeline_barrier = [&](std::initializer_list<VkImageMemoryBarrier> barriers) {
        constexpr VkPipelineStageFlags stages =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        vkCmdPipelineBarrier(command, stages, stages, 0, 0, nullptr, 0, nullptr,
                             static_cast<std::uint32_t>(barriers.size()), barriers.begin());
    };
    // Moves @p count levels from @p first between @p from and @p to, the attachment layout in which the passes draw
    // them and the read layout in which the passes and display conversion sample them, after the passes' writes.
    const auto levels_barrier = [&](std::uint32_t first, std::uint32_t count, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = chain.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, first, count, 0, 1};
        return barrier;
    };
    constexpr auto attachment = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // The world pass's color writes become visible to the bright pass, which samples the target in the layout that
    // the world pass leaves it in, and the levels that the frame draws become attachments, their contents discarded.
    VkImageMemoryBarrier scene{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    scene.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    scene.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    scene.oldLayout = scene.newLayout = sampled;
    scene.srcQueueFamilyIndex = scene.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    scene.image = world_target->color.image;
    scene.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pipeline_barrier({scene, levels_barrier(0, levels.count, VK_IMAGE_LAYOUT_UNDEFINED, attachment)});
    BloomPush push{};
    push.exposure = environment.exposure;
    push.threshold = options.bloom.threshold;
    push.knee = options.bloom.threshold * options.bloom.knee;
    // Draws @p level from @p source, an image of @p source_size.
    const auto draw_level = [&](VkRenderPass pass, VkPipeline level_pipeline, std::uint32_t level,
                                VkDescriptorSet source, VkExtent2D source_size, float weight) {
        const auto size = chain.extents[level];
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = chain.framebuffers[level];
        begin.renderArea.extent = size;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        const VkRect2D scissor{{0, 0}, size};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, level_pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_pipeline_layout, ANIMA_SET_SCENE_INPUT,
                                1, &source, 0, nullptr);
        push.texel = {1.0F / static_cast<float>(source_size.width), 1.0F / static_cast<float>(source_size.height)};
        push.weight = weight;
        vkCmdPushConstants(command, bloom_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    };
    draw_level(bloom_down_pass, bloom_bright_pipeline, 0, world_target->descriptor, scene_extent, 1);
    for (std::uint32_t level = 1; level < levels.count; ++level) {
        pipeline_barrier({levels_barrier(level - 1, 1, attachment, sampled)});
        draw_level(bloom_down_pass, bloom_downsample_pipeline, level, chain.descriptors[level - 1],
                   chain.extents[level - 1], 1);
    }
    // Each upsample samples the level after its own, just drawn, and loads its own, which the next downsample sampled.
    for (std::uint32_t level = levels.count - 1; level-- > 0;) {
        pipeline_barrier(
            {levels_barrier(level + 1, 1, attachment, sampled), levels_barrier(level, 1, sampled, attachment)});
        draw_level(bloom_up_pass, bloom_upsample_pipeline, level, chain.descriptors[level + 1],
                   chain.extents[level + 1], level + 2 == levels.count ? levels.coarsest_weight : 1);
    }
    pipeline_barrier({levels_barrier(0, 1, attachment, sampled)});
}
