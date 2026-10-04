// The atmosphere's tables (shaders/atmosphere.glsl), which compute dispatches build at the start of a frame that draws
// the sky: the transmittance and multiple scattering tables when the medium changes, and the sky view table when they
// do or when the eye's altitude, the sun's elevation or Mie scattering's asymmetry changes. Included inside Impl. The
// tables stay in the GENERAL layout from their first frame, which both their storage writes and their sampled reads
// accept.
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
// The fields of an Atmosphere that the transmittance and multiple scattering tables depend on, which same_medium()
// compares: those that atmosphere-transmittance.comp and atmosphere-scattering.comp read from the environment's uniform
// block.
static constexpr auto medium_fields =
    std::tuple{&Atmosphere::planet_radius,         &Atmosphere::thickness,        &Atmosphere::rayleigh_scattering,
               &Atmosphere::rayleigh_scale_height, &Atmosphere::mie_scattering,   &Atmosphere::mie_absorption,
               &Atmosphere::mie_scale_height,      &Atmosphere::ozone_absorption, &Atmosphere::ozone_altitude,
               &Atmosphere::ozone_width,           &Atmosphere::ground_albedo};
// The fields that neither shader reads: `enabled`, which is true whenever the tables are built, and three that only the
// sky view table and the sky read: the ground's height, through the eye's altitude above it, Mie scattering's
// asymmetry, which sky_view_inputs holds with that altitude, and the sun's angular radius, which only the sky reads.
// The atmosphere GPU check, check_updates() in tests/consumer/atmosphere.hpp, changes each field of both lists but
// `enabled`, and the sun's direction and irradiance. It requires a medium field's change to rebuild the tables, and the
// frame drawn after any other change to match one drawn after a rebuild.
static constexpr auto unread_fields = std::tuple{&Atmosphere::enabled, &Atmosphere::ground_height,
                                                 &Atmosphere::mie_anisotropy, &Atmosphere::sun_angular_radius};
// The lists name every field of Atmosphere: removing or renaming one stops the build at its name, and adding one stops
// it here, since Atmosphere then has more fields than both lists. A field listed twice in place of another is missed.
static_assert(
    has_fields<Atmosphere, std::tuple_size_v<decltype(medium_fields)> + std::tuple_size_v<decltype(unread_fields)>>,
    "Sort each Atmosphere field into medium_fields or unread_fields");
// Whether the tables have left the undefined layout, and the atmosphere whose medium the transmittance and multiple
// scattering tables hold.
bool atmosphere_ready{};
std::optional<Atmosphere> atmosphere_built;
// The sky view table's inputs besides the medium when it was last written, as atmosphere-sky.comp reads them from the
// environment's uniform block: Mie scattering's asymmetry, the eye's altitude and the Y of the unit direction toward
// the sun, as update_environment() writes them. None until the table is first written.
std::optional<std::array<float, 3>> sky_view_inputs;

// Whether @p a and @p b give the same transmittance and multiple scattering tables: whether they hold the same value in
// each of medium_fields, every channel of a Vec3.
static bool same_medium(const Atmosphere &a, const Atmosphere &b) {
    const auto same = [&](auto field) {
        const auto &x = a.*field, &y = b.*field;
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, Vec3>)
            return x.x == y.x && x.y == y.y && x.z == y.z;
        else
            return x == y;
    };
    return std::apply([&](auto... field) { return (same(field) && ...); }, medium_fields);
}
// The sun at the ground for @p e, which validate_environment() accepted, as atmosphere_sunlight() gives it: #sunlight,
// which holds it for #environment, while the two environments' suns match and their atmospheres match in `enabled`, the
// sun's angular radius and the medium; else integrated afresh. The medium holds every field that the transmittance
// reads, on the CPU as on the GPU, and the ground's albedo besides, so set_environment() integrates the transmittance
// only when an input of the sunlight changes. The atmosphere GPU check's check_sunlight_cache() requires that.
[[nodiscard]] Vec3 ground_sunlight(const Environment &e) const {
    const auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    const auto &held = environment.atmosphere, &next = e.atmosphere;
    if (same(environment.sun.direction, e.sun.direction) && same(environment.sun.irradiance, e.sun.irradiance) &&
        held.enabled == next.enabled && held.sun_angular_radius == next.sun_angular_radius && same_medium(held, next))
        return sunlight;
    return detail::atmosphere_sunlight_of(e);
}
// Creates the tables, their compute pipelines and descriptors, and points bindings 3 and 4 of each frame slot's
// environment set at the sky view and transmittance tables. Needs the environment sets. Throws `std::runtime_error`
// when the device cannot store and filter half-float images, which Vulkan requires of every device.
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
    const std::array<VkDescriptorImageInfo, 5> infos{
        image_info(transmittance_table, false), image_info(multiple_scattering_table, false),
        image_info(sky_view_table, false), image_info(transmittance_table, true),
        image_info(multiple_scattering_table, true)};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        auto &write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = atmosphere_set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    // The environment sets' bindings 3 and 4, which the sky samples.
    const std::array<VkDescriptorImageInfo, 2> sampled{image_info(sky_view_table, true),
                                                       image_info(transmittance_table, true)};
    constexpr std::uint32_t first_sampled = 3;
    for (const auto &slot : frames)
        for (std::uint32_t i = 0; i < sampled.size(); ++i) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = slot.environment_set;
            write.dstBinding = first_sampled + i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &sampled[i];
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
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
// medium differs from the one they hold, then the sky view table when they were rebuilt or sky_view_inputs differ,
// then a barrier that makes every write visible to the frame's later shaders. A recording that writes begins, after
// the first, with a barrier that waits for the fragment shaders submitted before it, which include the sky's reads of
// the tables in earlier frames that may still be in flight. A frame that writes no table records nothing: the barrier
// after the tables' last writes reaches every command submitted to the queue after it, so it makes them visible to this
// frame's shaders too, and without a write there is no earlier read to wait for. The atmosphere GPU check's
// report_cost() requires the frames that record nothing to take under half the time of frames that redraw the sky view
// table.
void record_atmosphere() {
    const auto &atmosphere = environment.atmosphere;
    if (!atmosphere.enabled)
        return;
    const bool rebuild = !atmosphere_ready || !atmosphere_built || !same_medium(*atmosphere_built, atmosphere);
    const std::array sky_inputs{atmosphere.mie_anisotropy, float(eye_altitude()),
                                normalized(environment.sun.direction).y};
    if (!rebuild && sky_view_inputs == sky_inputs)
        return;
    const auto command = frame().command;
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (atmosphere_ready) {
        // Earlier frames' sky shaders sampled the sky view and transmittance tables that these dispatches rewrite, and
        // a write after a read needs only an execution dependency. The barrier that ends each recording orders the
        // earlier dispatches' reads, and their writes made available, before the frame's later fragment shaders, which
        // this barrier waits for, so those accesses precede the rewrite as well.
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 0, nullptr);
    } else {
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
    const VkDescriptorSet sets[]{atmosphere_set, frame().environment_set};
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
    if (rebuild) {
        dispatch(transmittance_table, cover(atmosphere_table_sizes[transmittance_table]));
        then();
        dispatch(multiple_scattering_table, atmosphere_table_sizes[multiple_scattering_table]);
        then();
        atmosphere_built = atmosphere;
    }
    dispatch(sky_view_table, cover(atmosphere_table_sizes[sky_view_table]));
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &written, 0, nullptr, 0, nullptr);
    sky_view_inputs = sky_inputs;
}
// Bytes of the atmosphere's tables.
std::uint64_t atmosphere_bytes() const {
    std::uint64_t bytes = 0;
    for (const auto &table : atmosphere_tables)
        bytes += table.bytes;
    return bytes;
}
