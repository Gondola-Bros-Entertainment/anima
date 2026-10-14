// Sprites: batches of textured quads that the world pass draws after every other draw. Included inside Impl.
// anima/sprite.hpp and VulkanRenderer::set_sprites() document the contract.

// One sprite as sprite.vert reads it, per instance: its attributes from ANIMA_SPRITE_PLACEMENT to ANIMA_SPRITE_COLOR.
struct SpriteInstance {
    std::array<float, 4> placement, shape, frame, color;
};
static_assert(sizeof(SpriteInstance) == 16 * sizeof(float), "sprite.vert reads four vec4 attributes per instance");
// Two triangles per sprite, whose corners sprite.vert finds from gl_VertexIndex.
static constexpr std::uint32_t sprite_vertices = 6;

// GPU state of one SpriteSheet, cached per object as custom materials are.
struct GpuSpriteSheet {
    VkDevice device{};
    VmaAllocator allocator{};
    std::shared_ptr<const SpriteSheet> source;
    // The image and view are owned; the sampler is a shared material sampler.
    GpuTexture texture;
    // Each frame's texture coordinates at its first texel and past its last, as SpriteInstance::frame holds them.
    std::vector<std::array<float, 4>> frames;
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    ~GpuSpriteSheet() {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (texture.view)
            vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image)
            vmaDestroyImage(allocator, texture.image, texture.allocation);
    }
    GpuSpriteSheet() = default;
    GpuSpriteSheet(const GpuSpriteSheet &) = delete;
    GpuSpriteSheet &operator=(const GpuSpriteSheet &) = delete;
};
// A nonempty batch of the frame: its sheet, its blend mode and its sprites' range in the frame's sprite buffer.
struct SpriteDraw {
    const GpuSpriteSheet *sheet{};
    SpriteBlend blend{};
    std::uint32_t first{}, count{};
};
VkShaderModule sprite_vertex_shader{}, sprite_fragment_shader{};
VkDescriptorSetLayout sprite_sheet_layout{};
VkPipelineLayout sprite_pipeline_layout{};
// The pipelines of each SpriteBlend, by enumerator, each created on the first frame that draws a batch with it.
std::array<VkPipeline, 2> sprite_pipelines{};
std::map<const SpriteSheet *, std::unique_ptr<GpuSpriteSheet>> sprite_cache;
// The batches of the latest VulkanRenderer::set_sprites(), which every frame draws.
std::vector<SpriteBatch> sprite_batches;
// The frame's draws and its sprites as the sprite buffer holds them, which prepare_sprites() writes for
// record_sprites().
std::vector<SpriteDraw> sprite_draws;
std::vector<SpriteInstance> sprite_instances;

void create_sprite_layout() {
    const VkDescriptorSetLayoutBinding binding{ANIMA_SPRITE_SHEET_TEXTURE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount = 1;
    descriptor.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &sprite_sheet_layout),
          "Create sprite sheet layout");
    // sprite.vert's View block: the view-projection.
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &sprite_sheet_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &sprite_pipeline_layout), "Create sprite pipeline layout");
}
// Returns a new pipeline that draws sprites with @p blend into the world pass, which the caller owns.
[[nodiscard]] VkPipeline create_sprite_pipeline(SpriteBlend blend) const {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto &stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sprite_vertex_shader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sprite_fragment_shader;
    const VkVertexInputBindingDescription binding{0, sizeof(SpriteInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    const std::array<VkVertexInputAttributeDescription, 4> attributes{{
        {ANIMA_SPRITE_PLACEMENT, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, placement)},
        {ANIMA_SPRITE_SHAPE, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, shape)},
        {ANIMA_SPRITE_FRAME, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, frame)},
        {ANIMA_SPRITE_COLOR, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, color)},
    }};
    GraphicsPipelineState state;
    state.stages = stages;
    state.vertex.vertexBindingDescriptionCount = 1;
    state.vertex.pVertexBindingDescriptions = &binding;
    state.vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    state.vertex.pVertexAttributeDescriptions = attributes.data();
    // As blended meshes: tested against the view's reversed depth, written nowhere. A mirrored sprite winds its
    // triangles clockwise, so neither face is culled.
    state.depth.depthTestEnable = VK_TRUE;
    state.depth.depthWriteEnable = VK_FALSE;
    state.depth.depthCompareOp = VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState color{};
    color.blendEnable = VK_TRUE;
    color.srcColorBlendFactor = color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color.dstColorBlendFactor = color.dstAlphaBlendFactor =
        blend == SpriteBlend::additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blend = color;
    state.layout = sprite_pipeline_layout;
    state.pass = render_pass;
    return create_graphics_pipeline(state, "Create sprite pipeline");
}
// Points @p gpu's set at its texture's view and sampler. No frame in flight may use the set.
void write_sprite_texture(const GpuSpriteSheet &gpu) const {
    const VkDescriptorImageInfo image{gpu.texture.sampler, gpu.texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = gpu.set;
    write.dstBinding = ANIMA_SPRITE_SHEET_TEXTURE;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
// The GPU state of @p sheet, created and uploaded on first use: its texture uploads as a custom material's does, except
// that an RGBA8 chain built here weights color by alpha, as blended base-color maps need. Throws
// `std::invalid_argument` for a texture beyond the device's image limits and `std::runtime_error` for failed Vulkan
// calls; a failure leaves nothing cached for @p sheet.
GpuSpriteSheet &gpu_sprite_sheet(const std::shared_ptr<const SpriteSheet> &sheet) {
    if (const auto found = sprite_cache.find(sheet.get()); found != sprite_cache.end())
        return *found->second;
    const auto &definition = sheet->definition();
    const auto &image = *definition.texture.image;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (image.width > properties.limits.maxImageDimension2D || image.height > properties.limits.maxImageDimension2D)
        throw std::invalid_argument("Sprite sheet texture exceeds device image dimensions");
    require_texture_formats();
    auto value = std::make_unique<GpuSpriteSheet>();
    value->device = device;
    value->allocator = allocator;
    value->source = sheet;
    {
        // UploadBatch retires submitted work before the sheet can unwind.
        UploadBatch upload{device, allocator, &fatal, &errors, graphics_family};
        TextureMipOptions mips;
        mips.alpha_weighted_color = true;
        (void)upload_image(value->texture, definition.texture, nullptr, mips, options.texture_mip_skip, upload,
                           RendererFailureStage::none, false, false);
    }
    // Divided in double, which holds every texel count and frame edge exactly, unlike float past 2^24.
    const double width = image.width, height = image.height;
    for (const auto &rectangle : definition.frames)
        value->frames.push_back({static_cast<float>(rectangle.x / width), static_cast<float>(rectangle.y / height),
                                 static_cast<float>((double(rectangle.x) + rectangle.width) / width),
                                 static_cast<float>((double(rectangle.y) + rectangle.height) / height)});
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &value->pool), "Create sprite sheet pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = value->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &sprite_sheet_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &value->set), "Allocate sprite sheet set");
    write_sprite_texture(*value);
    return *sprite_cache.emplace(sheet.get(), std::move(value)).first->second;
}
// Uploads the sheets of the nonempty batches and writes their sprites, in order, into the sprite buffer of the slot
// that records the next frame, whose previous frame has finished, creating the pipelines that they draw with.
void prepare_sprites() {
    sprite_draws.clear();
    sprite_instances.clear();
    resources.sprite_draw_calls = 0;
    for (const auto &batch : sprite_batches) {
        if (batch.sprites.empty())
            continue;
        auto &batch_pipeline = sprite_pipelines[static_cast<std::size_t>(batch.blend)];
        if (!batch_pipeline)
            batch_pipeline = create_sprite_pipeline(batch.blend);
        const auto &sheet = gpu_sprite_sheet(batch.sheet);
        sprite_draws.push_back({&sheet, batch.blend, static_cast<std::uint32_t>(sprite_instances.size()),
                                static_cast<std::uint32_t>(batch.sprites.size())});
        for (const auto &sprite : batch.sprites)
            sprite_instances.push_back({{sprite.position.x, sprite.position.y, sprite.position.z, sprite.rotation},
                                        {sprite.size.x, sprite.size.y, sprite.pivot.x, sprite.pivot.y},
                                        sheet.frames[sprite.frame],
                                        {sprite.color.x, sprite.color.y, sprite.color.z, sprite.alpha}});
    }
    if (sprite_instances.empty())
        return;
    auto &buffer = frame().sprite_buffer;
    const VkDeviceSize bytes = sprite_instances.size() * sizeof(SpriteInstance);
    if (!buffer || buffer->bytes < bytes) {
        VkDeviceSize capacity = buffer ? buffer->bytes : sizeof(SpriteInstance);
        while (capacity < bytes)
            capacity *= 2;
        auto candidate = resource_buffer(capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        buffer.swap(candidate); // The slot's fence has retired the only frame that read the old buffer.
    }
    std::memcpy(buffer->mapping, sprite_instances.data(), static_cast<std::size_t>(bytes));
    flush_resource(*buffer);
    resources.sprite_draw_calls = sprite_draws.size();
}
// Records the frame's sprites into the world pass after its other draws, with the viewport and scissor that it set:
// one instanced draw per nonempty batch. Binding the sheet's set disturbs the sets bound through
// resource_pipeline_layout, so it clears resource_bound.
void record_sprites() {
    if (sprite_draws.empty())
        return;
    const auto command = frame().command;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &frame().sprite_buffer->buffer, &offset);
    vkCmdPushConstants(command, sprite_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(view_projection),
                       view_projection.data());
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    const GpuSpriteSheet *bound_sheet = nullptr;
    for (const auto &draw : sprite_draws) {
        const auto draw_pipeline = sprite_pipelines[static_cast<std::size_t>(draw.blend)];
        if (draw_pipeline != bound_pipeline) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline);
            bound_pipeline = draw_pipeline;
        }
        if (draw.sheet != bound_sheet) {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sprite_pipeline_layout,
                                    ANIMA_SET_SPRITE_SHEET, 1, &draw.sheet->set, 0, nullptr);
            bound_sheet = draw.sheet;
        }
        vkCmdDraw(command, sprite_vertices, draw.count, 0, draw.first);
    }
    resource_bound = {};
}
void destroy_sprites() noexcept {
    sprite_draws.clear();
    sprite_batches.clear();
    sprite_cache.clear();
    for (auto &blend_pipeline : sprite_pipelines) {
        if (blend_pipeline)
            vkDestroyPipeline(device, blend_pipeline, nullptr);
        blend_pipeline = VK_NULL_HANDLE;
    }
    if (sprite_pipeline_layout)
        vkDestroyPipelineLayout(device, sprite_pipeline_layout, nullptr);
    if (sprite_sheet_layout)
        vkDestroyDescriptorSetLayout(device, sprite_sheet_layout, nullptr);
    for (const auto shader : {sprite_vertex_shader, sprite_fragment_shader})
        if (shader)
            vkDestroyShaderModule(device, shader, nullptr);
}
