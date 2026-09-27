// Included only inside VulkanRenderer::Impl when ANIMA_BUILD_UI is enabled.
// Vulkan has no sampler mode without mipmapping. As the note on VkSamplerCreateInfo describes, NEAREST mip
// selection with a maximum level of detail of 0.25 always samples the base level, while the computed level of
// detail still chooses between the minification and magnification filters, which a maximum of 0 would not.
static constexpr float ui_non_mipmapped_max_lod = .25F;
// The View push constant block of ui.vert, member for member: a column-major mat4, then two vec2.
using UiMat4 = decltype(detail::UiBatch::transform);
using UiVec2 = decltype(detail::UiBatch::translation);
struct UiPushConstants {
    UiMat4 transform;
    UiVec2 translation;
    UiVec2 dimensions;
};
// The push constant layout packs the block without padding, since each vec2 starts at a multiple of its
// alignment. The struct declares the members in the block's order and must hold exactly their floats.
static_assert(sizeof(UiPushConstants) ==
                  sizeof(float) * (std::tuple_size_v<UiMat4> + std::tuple_size_v<UiVec2> + std::tuple_size_v<UiVec2>),
              "UI push constants must match the size of ui.vert's View block");
struct UiBuffer {
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    // Mapped until the buffer is destroyed.
    void *mapping{};
    VkDeviceSize capacity{};
    ~UiBuffer() {
        if (buffer)
            vmaDestroyBuffer(allocator, buffer, allocation);
    }
};
struct UiImage {
    VkDevice device{};
    VmaAllocator allocator{};
    GpuTexture texture;
    VkDescriptorPool pool{};
    VkDescriptorSet descriptor{};
    ~UiImage() {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (texture.sampler)
            vkDestroySampler(device, texture.sampler, nullptr);
        if (texture.view)
            vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image)
            vmaDestroyImage(allocator, texture.image, texture.allocation);
    }
};
using UiTexturePtr = std::shared_ptr<const detail::UiTexture>;
std::map<UiTexturePtr, std::unique_ptr<UiImage>, std::owner_less<UiTexturePtr>> ui_images;
std::unique_ptr<UiBuffer> ui_buffer;
UiTexturePtr ui_white = std::make_shared<detail::UiTexture>(detail::UiTexture{1, 1, {255, 255, 255, 255}});

void create_ui_layout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount = 1;
    descriptor.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &ui_texture_layout), "Create UI texture layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(UiPushConstants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = layout.setLayoutCount = 1;
    layout.pPushConstantRanges = &push;
    layout.pSetLayouts = &ui_texture_layout;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &ui_pipeline_layout), "Create UI pipeline layout");
}
void retire_ui_unused() {
    // Called only after the frame fence. RmlUi/recorded CPU batches retain sources;
    // the cache's sole reference means release is now safe on both CPU and GPU.
    for (auto it = ui_images.begin(); it != ui_images.end();) {
        if (it->first.use_count() == 1)
            it = ui_images.erase(it);
        else
            ++it;
    }
}
void upload_ui_texture(const UiTexturePtr &source) {
    if (ui_images.contains(source))
        return;
    VkPhysicalDeviceProperties limits{};
    vkGetPhysicalDeviceProperties(physical, &limits);
    if (!source->width || !source->height || source->width > limits.limits.maxImageDimension2D ||
        source->height > limits.limits.maxImageDimension2D ||
        source->rgba.size() != std::uint64_t(source->width) * source->height * 4)
        throw std::invalid_argument("Invalid UI texture dimensions/bytes");
    auto candidate = std::make_unique<UiImage>();
    candidate->device = device;
    candidate->allocator = allocator;
    // This guard outlives every submitted upload, and unwinds before candidate.
    UploadBatch upload;
    upload.device = device;
    upload.allocator = allocator;
    upload.fatal = &fatal;
    upload.errors = &errors;
    upload.create(graphics_family);
    auto &texture = candidate->texture;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {source->width, source->height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    texture.allocation_bytes = create_image(image, texture.image, texture.allocation, "Create UI texture");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &texture.view), "Create UI texture view");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = ui_non_mipmapped_max_lod;
    check(vkCreateSampler(device, &sampler, nullptr, &texture.sampler), "Create UI sampler");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = source->rgba.size();
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    std::memcpy(create_buffer(buffer, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, upload.staging_buffer,
                              upload.staging_allocation, "Create UI staging")
                    .pMappedData,
                source->rgba.data(), source->rgba.size());
    check(vmaFlushAllocation(allocator, upload.staging_allocation, 0, VK_WHOLE_SIZE), "Flush UI staging");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(upload.command, &begin), "Begin UI upload");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = view.subresourceRange;
    vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = image.extent;
    vkCmdCopyBufferToImage(upload.command, upload.staging_buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    check(vkEndCommandBuffer(upload.command), "End UI upload");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload.command;
    check(vkQueueSubmit(graphics_queue, 1, &submit, upload.fence), "Submit UI upload");
    upload.pending = true;
    upload.wait();
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &candidate->pool), "Create UI descriptor pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = candidate->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &ui_texture_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &candidate->descriptor), "Allocate UI descriptor");
    const VkDescriptorImageInfo sampled{texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = candidate->descriptor;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &sampled;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    ui_images.emplace(source, std::move(candidate));
}
// UI blending in linear light needs a format that decodes the destination and encodes the result. Encoding
// in the shader, as the display pass does for UNORM formats, would blend translucent UI in encoded values.
static bool srgb_format(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}
// Whether the swapchain that recreate() would create now can present UI; UiContext asks at construction.
bool srgb_presentation() const { return srgb_format(surface_format().format); }
void prepare_ui(const detail::UiFrame &frame) {
    // Each swapchain recreation chooses its format again, so a surface that stops offering an sRGB format
    // after UiContext construction is still rejected here rather than blended wrongly.
    if (!srgb_format(format))
        throw std::runtime_error(detail::srgb_presentation_required);
    if (frame.vertices.empty())
        return;
    if (!ui_pipeline)
        create_pipeline(PipelineKind::ui, ui_pipeline);
    const auto bytes = VkDeviceSize(frame.vertices.size()) * sizeof(detail::UiVertex);
    if (!ui_buffer || ui_buffer->capacity < bytes) {
        auto candidate = std::make_unique<UiBuffer>();
        candidate->allocator = allocator;
        candidate->capacity = bytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        candidate->mapping = create_buffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, candidate->buffer,
                                           candidate->allocation, "Create UI vertex buffer")
                                 .pMappedData;
        ui_buffer.swap(candidate);
    }
    std::memcpy(ui_buffer->mapping, frame.vertices.data(), static_cast<std::size_t>(bytes));
    check(vmaFlushAllocation(allocator, ui_buffer->allocation, 0, VK_WHOLE_SIZE), "Flush UI vertices");
    for (const auto &batch : frame.batches)
        upload_ui_texture(batch.texture ? batch.texture : ui_white);
}
void record_ui(const detail::UiFrame &frame) {
    if (frame.vertices.empty())
        return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &ui_buffer->buffer, &offset);
    for (const auto &batch : frame.batches) {
        VkRect2D scissor{{0, 0}, extent};
        if (batch.clipped) {
            const int left = std::clamp(batch.clip[0], 0, static_cast<int>(extent.width));
            const int top = std::clamp(batch.clip[1], 0, static_cast<int>(extent.height));
            const int right = std::clamp(batch.clip[2], left, static_cast<int>(extent.width));
            const int bottom = std::clamp(batch.clip[3], top, static_cast<int>(extent.height));
            if (right == left || bottom == top)
                continue;
            scissor = {{left, top},
                       {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
        }
        vkCmdSetScissor(command, 0, 1, &scissor);
        const UiPushConstants push{
            batch.transform, batch.translation, {static_cast<float>(extent.width), static_cast<float>(extent.height)}};
        vkCmdPushConstants(command, ui_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const auto descriptor = ui_images.at(batch.texture ? batch.texture : ui_white)->descriptor;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_layout, 0, 1, &descriptor, 0,
                                nullptr);
        vkCmdDraw(command, batch.count, 1, batch.first, 0);
    }
}
