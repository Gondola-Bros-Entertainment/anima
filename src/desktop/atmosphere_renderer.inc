// The atmosphere's tables (shaders/atmosphere.glsl), which compute dispatches build at the start of each frame that
// draws the sky: the transmittance and multiple scattering tables when the medium changes, and the sky view table
// every frame, from the eye's altitude and the sun's direction. Included inside Impl. The tables stay in the GENERAL
// layout from their first frame, which both their storage writes and their sampled reads accept.
struct AtmosphereTable {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView view{};
    VkDeviceSize bytes{};
};
// Indices of atmosphere_tables and atmosphere_pipelines, in the order a rebuild dispatches them.
enum AtmosphereTableIndex : std::size_t { transmittance_table, multiple_scattering_table, sky_view_table };
// Their sizes, as atmosphere.glsl states them.
static constexpr std::array<VkExtent2D, 3> atmosphere_table_sizes{{{256, 64}, {64, 32}, {192, 108}}};
std::array<AtmosphereTable, 3> atmosphere_tables{};
VkSampler atmosphere_sampler{};
VkDescriptorSetLayout atmosphere_layout{};
VkDescriptorPool atmosphere_pool{};
VkDescriptorSet atmosphere_set{};
VkPipelineLayout atmosphere_pipeline_layout{};
std::array<VkShaderModule, 3> atmosphere_shaders{};
std::array<VkPipeline, 3> atmosphere_pipelines{};
// Whether the tables have left the undefined layout, and the medium that the transmittance and multiple scattering
// tables hold.
bool atmosphere_ready{};
std::optional<Atmosphere> atmosphere_built;

// Whether @p a and @p b give the same transmittance and multiple scattering tables.
static bool same_medium(const Atmosphere &a, const Atmosphere &b) {
    const auto same = [](Vec3 x, Vec3 y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return a.planet_radius == b.planet_radius && a.thickness == b.thickness &&
           same(a.rayleigh_scattering, b.rayleigh_scattering) && a.rayleigh_scale_height == b.rayleigh_scale_height &&
           same(a.mie_scattering, b.mie_scattering) && same(a.mie_absorption, b.mie_absorption) &&
           a.mie_scale_height == b.mie_scale_height && same(a.ozone_absorption, b.ozone_absorption) &&
           a.ozone_altitude == b.ozone_altitude && a.ozone_width == b.ozone_width &&
           same(a.ground_albedo, b.ground_albedo);
}
// Creates the tables, their compute pipelines and descriptors, and points the environment set's bindings 3 and 4 at
// the sky view and transmittance tables. Needs the environment set. Throws `std::runtime_error` when the device cannot
// store and filter half-float images, which Vulkan requires of every device.
void create_atmosphere(std::span<const std::uint32_t> transmittance, std::span<const std::uint32_t> scattering,
                       std::span<const std::uint32_t> sky) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("Device lacks half-float storage images");
    for (std::size_t i = 0; i < atmosphere_tables.size(); ++i) {
        auto &table = atmosphere_tables[i];
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        image.extent = {atmosphere_table_sizes[i].width, atmosphere_table_sizes[i].height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        table.bytes = create_image(image, table.image, table.allocation, "Create atmosphere table");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = table.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &table.view), "Create atmosphere table view");
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampler, nullptr, &atmosphere_sampler), "Create atmosphere sampler");
    // Set 0 of the compute passes: each table to write, then the transmittance and multiple scattering tables to read.
    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<std::uint32_t>(std::size(bindings));
    layout.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &atmosphere_layout), "Create atmosphere layout");
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &atmosphere_pool), "Create atmosphere pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = atmosphere_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &atmosphere_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &atmosphere_set), "Allocate atmosphere set");
    const auto image_info = [&](AtmosphereTableIndex index, bool sampled) {
        return VkDescriptorImageInfo{sampled ? atmosphere_sampler : VkSampler{}, atmosphere_tables[index].view,
                                     VK_IMAGE_LAYOUT_GENERAL};
    };
    const std::array<VkDescriptorImageInfo, 7> infos{
        image_info(transmittance_table, false), image_info(multiple_scattering_table, false),
        image_info(sky_view_table, false), image_info(transmittance_table, true),
        image_info(multiple_scattering_table, true),
        // The environment set's sky view and transmittance tables.
        image_info(sky_view_table, true), image_info(transmittance_table, true)};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        auto &write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = i < 5 ? atmosphere_set : environment_set;
        write.dstBinding = i < 5 ? i : i - 2;
        write.descriptorCount = 1;
        write.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    const VkDescriptorSetLayout layouts[]{atmosphere_layout, environment_layout};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 2;
    pipeline_layout_info.pSetLayouts = layouts;
    check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &atmosphere_pipeline_layout),
          "Create atmosphere pipeline layout");
    const std::array codes{transmittance, scattering, sky};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        atmosphere_shaders[i] = custom_module(codes[i]);
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = atmosphere_shaders[i];
        info.stage.pName = "main";
        info.layout = atmosphere_pipeline_layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &atmosphere_pipelines[i]),
              "Create atmosphere pipeline");
    }
}
void destroy_atmosphere() noexcept {
    for (auto &compute : atmosphere_pipelines)
        if (compute)
            vkDestroyPipeline(device, std::exchange(compute, VK_NULL_HANDLE), nullptr);
    for (auto &shader : atmosphere_shaders)
        if (shader)
            vkDestroyShaderModule(device, std::exchange(shader, VK_NULL_HANDLE), nullptr);
    if (atmosphere_pipeline_layout)
        vkDestroyPipelineLayout(device, std::exchange(atmosphere_pipeline_layout, VK_NULL_HANDLE), nullptr);
    if (atmosphere_pool)
        vkDestroyDescriptorPool(device, std::exchange(atmosphere_pool, VK_NULL_HANDLE), nullptr);
    if (atmosphere_layout)
        vkDestroyDescriptorSetLayout(device, std::exchange(atmosphere_layout, VK_NULL_HANDLE), nullptr);
    if (atmosphere_sampler)
        vkDestroySampler(device, std::exchange(atmosphere_sampler, VK_NULL_HANDLE), nullptr);
    for (auto &table : atmosphere_tables) {
        if (table.view)
            vkDestroyImageView(device, table.view, nullptr);
        if (table.image)
            vmaDestroyImage(allocator, table.image, table.allocation);
        table = {};
    }
}
// Records the atmosphere's dispatches while it is enabled: the transmittance and multiple scattering tables when the
// medium differs from the one they hold, then the sky view table, then a barrier that makes every write visible to
// the frame's later shaders. The previous frame's reads finished at draw()'s wait for its fence.
void record_atmosphere() {
    const auto &atmosphere = environment.atmosphere;
    if (!atmosphere.enabled)
        return;
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (!atmosphere_ready) {
        std::array<VkImageMemoryBarrier, 3> general{};
        for (std::size_t i = 0; i < general.size(); ++i) {
            general[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            general[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            general[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            general[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            general[i].srcQueueFamilyIndex = general[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            general[i].image = atmosphere_tables[i].image;
            general[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, static_cast<std::uint32_t>(general.size()), general.data());
        atmosphere_ready = true;
        atmosphere_built.reset();
    }
    const VkDescriptorSet sets[]{atmosphere_set, environment_set};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, atmosphere_pipeline_layout, 0, 2, sets, 0,
                            nullptr);
    const auto dispatch = [&](AtmosphereTableIndex index, VkExtent2D groups) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, atmosphere_pipelines[index]);
        vkCmdDispatch(command, groups.width, groups.height, 1);
    };
    const auto then = [&] {
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &written, 0, nullptr, 0, nullptr);
    };
    // Groups of 8 by 8 invocations, except the multiple scattering table's one group of 64 per texel.
    const auto cover = [](VkExtent2D size) { return VkExtent2D{(size.width + 7) / 8, (size.height + 7) / 8}; };
    if (!atmosphere_built || !same_medium(*atmosphere_built, atmosphere)) {
        dispatch(transmittance_table, cover(atmosphere_table_sizes[transmittance_table]));
        then();
        dispatch(multiple_scattering_table, atmosphere_table_sizes[multiple_scattering_table]);
        then();
        atmosphere_built = atmosphere;
    }
    dispatch(sky_view_table, cover(atmosphere_table_sizes[sky_view_table]));
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &written, 0, nullptr, 0, nullptr);
}
// Bytes of the atmosphere's tables.
std::uint64_t atmosphere_bytes() const {
    std::uint64_t bytes = 0;
    for (const auto &table : atmosphere_tables)
        bytes += table.bytes;
    return bytes;
}
