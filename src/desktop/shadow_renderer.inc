// Explicit world and optional detail regions. Each pass culls its own casters;
// consumers can move/resize either region without changing scene membership.
struct ShadowTarget {
    VkDevice device{};
    GpuTexture texture;
    VkFramebuffer framebuffer{};
    std::uint32_t size{};
    ~ShadowTarget() {
        if (framebuffer)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (texture.sampler)
            vkDestroySampler(device, texture.sampler, nullptr);
        if (texture.view)
            vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image)
            vkDestroyImage(device, texture.image, nullptr);
        if (texture.memory)
            vkFreeMemory(device, texture.memory, nullptr);
    }
};
std::unique_ptr<ShadowTarget> shadow_target;
std::unique_ptr<ShadowTarget> detail_shadow_target;
VkFormat shadow_format{};
VkRenderPass shadow_pass{};
VkShaderModule shadow_resource_vertex_shader{}, shadow_fragment_shader{};
VkPipeline shadow_resource_pipeline{};
Mat4 shadow_view = identity();
RenderFrustum shadow_frustum;
Mat4 detail_shadow_view = identity();
RenderFrustum detail_shadow_frustum;
void create_shadow_pass() {
    for (const auto format_candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, format_candidate, &properties);
        const auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((properties.optimalTilingFeatures & needed) == needed) {
            shadow_format = format_candidate;
            break;
        }
    }
    if (!shadow_format)
        throw std::runtime_error("Device lacks sampled depth images");
    VkAttachmentDescription depth{};
    depth.format = shadow_format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &depth;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    check(vkCreateRenderPass(device, &pass, nullptr, &shadow_pass), "Create shadow pass");
}
void ensure_shadow_target(std::unique_ptr<ShadowTarget> &target, const DirectionalShadow &region,
                          std::uint32_t binding) {
    const auto size = region.enabled ? region.resolution : 1U;
    if (target && target->size == size)
        return;
    auto next = std::make_unique<ShadowTarget>();
    next->device = device;
    next->size = size;
    auto &texture = next->texture;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = shadow_format;
    image.extent = {size, size, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(device, &image, nullptr, &texture.image), "Create shadow image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, texture.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    texture.allocation_bytes = requirements.size;
    check(vkAllocateMemory(device, &allocation, nullptr, &texture.memory), "Allocate shadow memory");
    check(vkBindImageMemory(device, texture.image, texture.memory, 0), "Bind shadow memory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = shadow_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create shadow view");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampler, nullptr, &texture.sampler), "Create shadow sampler");
    VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    frame.renderPass = shadow_pass;
    frame.attachmentCount = 1;
    frame.pAttachments = &texture.view;
    frame.width = frame.height = size;
    frame.layers = 1;
    check(vkCreateFramebuffer(device, &frame, nullptr, &next->framebuffer), "Create shadow framebuffer");
    const VkDescriptorImageInfo info{texture.sampler, texture.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = environment_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    target.swap(next); // Previous readers retired at the existing frame fence.
}
void ensure_shadow_targets() {
    ensure_shadow_target(shadow_target, environment.shadow, 1);
    ensure_shadow_target(detail_shadow_target, environment.detail_shadow, 2);
}
void record_shadow_region(const ShadowTarget &target, bool detail) {
    // Keep the render-to-sample dependency explicit for each image. Following
    // regions exposed intermittent stale depth with implicit render-pass
    // transitions on the native backend. Both early and late depth tests write
    // this image; the next pass samples it in the fragment stage.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Every pass clears the full image.
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    const auto tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, tests, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearValue clear{};
    clear.depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = shadow_pass;
    begin.framebuffer = target.framebuffer;
    begin.renderArea.extent = {target.size, target.size};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const auto size = static_cast<float>(target.size);
    const VkViewport viewport{0, 0, size, size, 0, 1};
    const VkRect2D scissor{{0, 0}, {target.size, target.size}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const auto &region = detail ? environment.detail_shadow : environment.shadow;
    if (region.enabled && !resource_scenes.empty())
        record_resources(true, detail);
    vkCmdEndRenderPass(command);
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, tests, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
void record_shadow() {
    resources.shadow_draw_calls = resources.shadow_submitted_indices = 0;
    record_shadow_region(*shadow_target, false);
    // Even a disabled region clears its 1x1 placeholder into a shader-readable
    // layout so all statically used descriptors remain valid.
    record_shadow_region(*detail_shadow_target, true);
}
