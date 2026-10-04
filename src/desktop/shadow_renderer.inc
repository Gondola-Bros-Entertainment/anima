// The sun's shadow maps: the cascades (EnvironmentSettings::shadow_cascades), fitted to the view every frame as the
// layers of one image, and the optional detail region (EnvironmentSettings::detail_shadow). Each pass culls its own
// casters, so consumers can move the view or the detail region without changing scene membership.
struct ShadowTarget {
    VkDevice device{};
    VmaAllocator allocator{};
    // #layers square layers #size texels on an edge, which the shaders sample through one 2D array view.
    GpuTexture texture;
    // Each layer's view and framebuffer, which one shadow pass renders into.
    std::vector<VkImageView> layer_views;
    std::vector<VkFramebuffer> framebuffers;
    std::uint32_t size{}, layers{};
    ~ShadowTarget() {
        for (const auto framebuffer : framebuffers)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        for (const auto view : layer_views)
            vkDestroyImageView(device, view, nullptr);
        if (texture.sampler)
            vkDestroySampler(device, texture.sampler, nullptr);
        if (texture.view)
            vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image)
            vmaDestroyImage(allocator, texture.image, texture.allocation);
    }
    ShadowTarget() = default;
    ShadowTarget(const ShadowTarget &) = delete;
    ShadowTarget &operator=(const ShadowTarget &) = delete;
};
// One shadow pass: the view-projection that its casters draw through, and the frustum that culls them.
struct ShadowPass {
    Mat4 view = identity();
    RenderFrustum frustum;
};
std::unique_ptr<ShadowTarget> cascade_target;
std::unique_ptr<ShadowTarget> detail_shadow_target;
// Counts the shadow targets created, so that each frame slot's environment set can tell whether it points at the
// current ones (FrameSlot::shadow_generation).
std::uint64_t shadow_generation{};
VkFormat shadow_format{};
VkRenderPass shadow_pass{};
VkShaderModule shadow_resource_vertex_shader{}, shadow_fragment_shader{};
// Opaque casters write depth without a fragment shader; masked ones discard through shadow.frag.
VkPipeline shadow_opaque_pipeline{}, shadow_masked_pipeline{};
// Whether set_view() has given a view, which the cascades fit.
bool view_set{};
// The sun's axes (detail::sun_axes()) and the cascades fitted to the latest view, none while the cascades are disabled
// or before a view is set. Each cascade's depth begins at cascade_near along the sun, which prepare_resources()
// extends toward its casters, and spans cascade_depth once finish_shadow_cascades() has rounded both ends.
std::array<std::array<double, 3>, 3> sun_axes{};
detail::CascadeFits cascade_fits;
std::array<double, ShadowCascades::max_count> cascade_near{}, cascade_depth{};
std::array<ShadowPass, ShadowCascades::max_count> cascade_passes;
ShadowPass detail_pass;
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
// Gives @p target @p layers layers of @p size texels on an edge, replacing it when either changes. The replaced target
// is destroyed once no frame in flight can use it.
void ensure_shadow_target(std::unique_ptr<ShadowTarget> &target, std::uint32_t size, std::uint32_t layers) {
    if (target && target->size == size && target->layers == layers)
        return;
    auto next = std::make_unique<ShadowTarget>();
    next->device = device;
    next->allocator = allocator;
    next->size = size;
    next->layers = layers;
    auto &texture = next->texture;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = shadow_format;
    image.extent = {size, size, 1};
    image.mipLevels = 1;
    image.arrayLayers = layers;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    texture.allocation_bytes = create_image(image, texture.image, texture.allocation, "Create shadow image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = shadow_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create shadow view");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampler, nullptr, &texture.sampler), "Create shadow sampler");
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        check(vkCreateImageView(device, &view, nullptr, &next->layer_views.emplace_back()), "Create shadow layer view");
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        frame.renderPass = shadow_pass;
        frame.attachmentCount = 1;
        frame.pAttachments = &next->layer_views.back();
        frame.width = frame.height = size;
        frame.layers = 1;
        check(vkCreateFramebuffer(device, &frame, nullptr, &next->framebuffers.emplace_back()),
              "Create shadow framebuffer");
    }
    target.swap(next);
    release_after_frames(std::move(next));
    ++shadow_generation;
}
// Sizes the shadow targets for the environment, then points bindings 1 and 2 of the environment set of the slot that
// records the next frame at the cascades' and the detail region's 2D array views, unless it already does. The slot's
// previous frame has finished, so nothing reads that set.
void ensure_shadow_targets() {
    const auto &cascades = environment.shadow_cascades;
    ensure_shadow_target(cascade_target, cascades.enabled ? cascades.resolution : 1U,
                         cascades.enabled ? cascades.count : 1U);
    const auto &detail = environment.detail_shadow;
    ensure_shadow_target(detail_shadow_target, detail.enabled ? detail.resolution : 1U, 1);
    auto &slot = frame();
    if (slot.shadow_generation == shadow_generation)
        return;
    // The cascades at binding 1, then the detail region.
    const std::array<const ShadowTarget *, 2> targets{cascade_target.get(), detail_shadow_target.get()};
    constexpr std::uint32_t first_binding = 1;
    std::array<VkDescriptorImageInfo, 2> infos{};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        const auto &texture = targets[i]->texture;
        infos[i] = {texture.sampler, texture.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = slot.environment_set;
        writes[i].dstBinding = first_binding + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    slot.shadow_generation = shadow_generation;
}
// Fits the cascades to the current view, each with its depth beginning where its sphere faces the sun, which
// prepare_resources() extends toward its casters.
void fit_shadow_cascades() {
    cascade_fits = {};
    if (!environment.shadow_cascades.enabled || !view_set)
        return;
    sun_axes = detail::sun_axes(environment.sun.direction);
    cascade_fits = detail::fit_cascades(environment, view_projection, sun_axes);
    for (std::uint32_t i = 0; i < cascade_fits.count; ++i)
        cascade_near[i] = cascade_fits.cascades[i].depth - cascade_fits.cascades[i].radius;
}
// Whether @p bounds reach into the column toward the sun of cascade @p index's square, and so may shadow what the
// cascade holds; if so, extends the cascade's depth toward the sun over them. Comparing the box's projected extent with
// the square, with a texel of allowance for rounding, keeps every box that touches the column.
bool cascade_casts(std::uint32_t index, const RenderBounds &bounds) {
    const auto &fit = cascade_fits.cascades[index];
    const std::array<double, 3> center{(double(bounds.minimum.x) + bounds.maximum.x) / 2,
                                       (double(bounds.minimum.y) + bounds.maximum.y) / 2,
                                       (double(bounds.minimum.z) + bounds.maximum.z) / 2};
    const std::array<double, 3> half{(double(bounds.maximum.x) - bounds.minimum.x) / 2,
                                     (double(bounds.maximum.y) - bounds.minimum.y) / 2,
                                     (double(bounds.maximum.z) - bounds.minimum.z) / 2};
    // The box's center and half extent along @p axis.
    const auto along = [&](const std::array<double, 3> &axis) {
        return std::array{axis[0] * center[0] + axis[1] * center[1] + axis[2] * center[2],
                          std::abs(axis[0]) * half[0] + std::abs(axis[1]) * half[1] + std::abs(axis[2]) * half[2]};
    };
    const auto x = along(sun_axes[0]), y = along(sun_axes[1]), z = along(sun_axes[2]);
    if (std::abs(x[0] - fit.across[0]) > fit.radius + x[1] + fit.texel ||
        std::abs(y[0] - fit.across[1]) > fit.radius + y[1] + fit.texel ||
        z[0] - z[1] > fit.depth + fit.radius + fit.texel)
        return false;
    // Bounds beyond the float range would leave no finite depth to extend to; such a caster casts where it can.
    if (const auto nearest = z[0] - z[1]; std::isfinite(nearest))
        cascade_near[index] = std::min(cascade_near[index], nearest);
    return true;
}
// Builds each fitted cascade's view-projection and frustum, its depth from its nearest caster toward the sun to the far
// side of its sphere, each end rounded outward to whole texels.
void finish_shadow_cascades() {
    for (std::uint32_t i = 0; i < cascade_fits.count; ++i) {
        const auto &fit = cascade_fits.cascades[i];
        const auto near_depth = std::floor(cascade_near[i] / fit.texel) * fit.texel;
        const auto far_depth = std::ceil((fit.depth + fit.radius) / fit.texel) * fit.texel;
        cascade_depth[i] = far_depth - near_depth;
        auto &pass = cascade_passes[i];
        pass.view = detail::sun_projection(sun_axes, fit.across, fit.radius, near_depth, far_depth);
        pass.frustum = RenderFrustum(pass.view);
    }
}
// Clears every layer of @p target and renders the casters of pass @p passes[layer] into each layer that has one, so
// that the shaders read defined depth in every layer, the placeholder of a disabled map included.
void record_shadow_target(const ShadowTarget &target, std::span<const ShadowPass> passes) {
    const auto command = frame().command;
    // Keep the render-to-sample dependency explicit for each image. Following
    // regions exposed intermittent stale depth with implicit render-pass
    // transitions on the native backend. Both early and late depth tests write
    // this image; the next pass samples it in the fragment stage.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Every pass clears its whole layer.
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, target.layers};
    const auto tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, tests, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearValue clear{};
    clear.depthStencil = {1, 0};
    const auto size = static_cast<float>(target.size);
    const VkViewport viewport{0, 0, size, size, 0, 1};
    const VkRect2D scissor{{0, 0}, {target.size, target.size}};
    for (std::uint32_t layer = 0; layer < target.layers; ++layer) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = shadow_pass;
        begin.framebuffer = target.framebuffers[layer];
        begin.renderArea.extent = {target.size, target.size};
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        if (layer < passes.size() && !resource_scenes.empty())
            record_resources(&passes[layer]);
        vkCmdEndRenderPass(command);
    }
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, tests, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
void record_shadow() {
    resources.shadow_draw_calls = resources.shadow_submitted_indices = 0;
    record_shadow_target(*cascade_target, std::span(cascade_passes.data(), cascade_fits.count));
    // Even a disabled region clears its 1x1 placeholder into a shader-readable
    // layout so all statically used descriptors remain valid.
    record_shadow_target(*detail_shadow_target, environment.detail_shadow.enabled
                                                    ? std::span<const ShadowPass>(&detail_pass, 1)
                                                    : std::span<const ShadowPass>{});
}
