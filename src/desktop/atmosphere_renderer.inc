// The atmosphere's tables (shaders/atmosphere.glsl), which compute dispatches build at the start of a frame that draws
// the sky: the transmittance and multiple scattering tables when the medium changes, and the sky view table when they
// do or when the altitude that the sky is seen from (sky_altitude()), the sun's elevation or Mie scattering's asymmetry
// changes. Included inside Impl. The tables stay in the GENERAL layout from their first frame, which both their
// storage writes and their sampled reads accept.
struct AtmosphereTable {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView view{};
    VkDeviceSize bytes{};
};
// Indices of atmosphere_tables and atmosphere_pipelines, in the order a rebuild dispatches them.
enum AtmosphereTableIndex : std::size_t { transmittance_table, multiple_scattering_table, sky_view_table };
// Their sizes, which atmosphere.glsl reads from shaders/shader_interface.h.
static constexpr std::array<VkExtent2D, 3> atmosphere_table_sizes{
    {{ANIMA_TRANSMITTANCE_WIDTH, ANIMA_TRANSMITTANCE_HEIGHT},
     {ANIMA_MULTIPLE_SCATTERING_WIDTH, ANIMA_MULTIPLE_SCATTERING_HEIGHT},
     {ANIMA_SKY_VIEW_WIDTH, ANIMA_SKY_VIEW_HEIGHT}}};
// A binding of a table: in the atmosphere set, as a storage image that its compute pass writes or as a sampled image
// that later passes read, or in the environment set, sampled.
struct AtmosphereBinding {
    std::uint32_t binding;
    AtmosphereTableIndex table;
    bool sampled;
};
// The atmosphere set of the compute passes: each table to write, then the transmittance and multiple scattering tables
// to read.
static constexpr std::array<AtmosphereBinding, 5> atmosphere_bindings{
    {{ANIMA_ATMOSPHERE_WRITE_TRANSMITTANCE, transmittance_table, false},
     {ANIMA_ATMOSPHERE_WRITE_MULTIPLE_SCATTERING, multiple_scattering_table, false},
     {ANIMA_ATMOSPHERE_WRITE_SKY_VIEW, sky_view_table, false},
     {ANIMA_ATMOSPHERE_READ_TRANSMITTANCE, transmittance_table, true},
     {ANIMA_ATMOSPHERE_READ_MULTIPLE_SCATTERING, multiple_scattering_table, true}}};
// The environment set's sky view and transmittance tables, which the sky reads.
static constexpr std::array<AtmosphereBinding, 2> environment_table_bindings{
    {{ANIMA_ENVIRONMENT_SKY_VIEW, sky_view_table, true}, {ANIMA_ENVIRONMENT_TRANSMITTANCE, transmittance_table, true}}};
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
// sky view table and the sky read: the ground's height, through the altitude above it that the sky is seen from, Mie
// scattering's asymmetry, which sky_view_inputs holds with that altitude, and the sun's angular radius, which only the
// sky reads.
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
// The sky view table's inputs besides the medium, as update_environment() writes them into the environment's uniform
// block and atmosphere-sky.comp reads them: Mie scattering's asymmetry, the altitude that the sky is seen from
// (sky_altitude()), before the block rounds it to float, and the Y of the unit direction toward the sun, the cosine of
// its zenith angle.
struct SkyViewInputs {
    float mie_anisotropy{};
    double altitude{};
    float sun_cos_zenith{};
    bool operator==(const SkyViewInputs &) const = default;
};
// Those that the table was last written with. None until it is first written.
std::optional<SkyViewInputs> sky_view_inputs;
// How far eye_altitude() may move from the altitude that the sky view table was last written with while the sky is
// still seen from that one: the larger of sky_altitude_slack, in meters, and sky_altitude_rounding times the eye's
// distance from the world's origin. set_view() recovers the eye from a float view-projection, whose elements round
// differently as the camera turns, so a camera that only turns moves that eye. Over whole turns, 0.5 degrees a frame,
// of look_at() views from 60,000 random eyes and Camera views from 30,000, 0.5 m to 1,600 km from the origin, with
// pitches up to 80 degrees, near planes from 0.001 to 1 and a far plane at 1,000, its altitude spanned at most 3.99
// float epsilons of its distance, and 3.9 for eyes beyond 1 km, where this term exceeds sky_altitude_slack;
// sky_altitude_rounding allows 8, twice that. A millimeter moves the horizon, around which the table spends its rows,
// by under 3e-7 radians at Earth's radius, even from the lowest altitude, 1 m, where it moves fastest.
static constexpr double sky_altitude_slack = 1e-3;
static constexpr double sky_altitude_rounding = 8 * double(std::numeric_limits<float>::epsilon());

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
// Creates the tables, their compute pipelines and descriptors, and points the environment_table_bindings of each frame
// slot's environment set at the sky view and transmittance tables. Needs the environment sets. Throws
// `std::runtime_error` when the device cannot store and filter half-float images, which Vulkan requires of every
// device.
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
    const auto descriptor_type = [](const AtmosphereBinding &binding) {
        return binding.sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    };
    std::array<VkDescriptorSetLayoutBinding, atmosphere_bindings.size()> bindings{};
    for (std::size_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {atmosphere_bindings[i].binding, descriptor_type(atmosphere_bindings[i]), 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &atmosphere_layout), "Create atmosphere layout");
    const auto sampled_count =
        static_cast<std::uint32_t>(std::ranges::count(atmosphere_bindings, true, &AtmosphereBinding::sampled));
    const VkDescriptorPoolSize sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<std::uint32_t>(atmosphere_bindings.size()) - sampled_count},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled_count}};
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
    // Points each of @p tables, bindings of @p set, at its table.
    const auto write_tables = [&](VkDescriptorSet set, std::span<const AtmosphereBinding> tables) {
        for (const auto &binding : tables) {
            const VkDescriptorImageInfo info{binding.sampled ? atmosphere_sampler : VkSampler{},
                                             atmosphere_tables[binding.table].view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = descriptor_type(binding);
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    };
    write_tables(atmosphere_set, atmosphere_bindings);
    for (const auto &slot : frames)
        write_tables(slot.environment_set, environment_table_bindings);
    std::array<VkDescriptorSetLayout, ANIMA_SET_ENVIRONMENT + 1> layouts{};
    layouts[ANIMA_SET_ATMOSPHERE] = atmosphere_layout;
    layouts[ANIMA_SET_ENVIRONMENT] = environment_layout;
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
    pipeline_layout_info.pSetLayouts = layouts.data();
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
// The altitude that the sky is seen from, which update_environment() writes for atmosphere-sky.comp and the sky to
// read: that of the sky view table's last write while eye_altitude() stays within the tolerance of it
// (sky_altitude_slack and sky_altitude_rounding) and it stays inside_atmosphere(), which a thinner atmosphere can leave
// it above; else eye_altitude(). Holding it keeps a camera that only turns from redrawing the table, and keeps the
// table and the sky that samples it seeing from one altitude. For an orthographic view, view_origin holds a unit
// direction, so the distance is 1, and eye_altitude() stays at sky_altitude_margin. The atmosphere GPU check's
// check_thinned() thins the atmosphere under an eye above it, 10,000 km from the world's origin, and requires the sky
// that a detour shows, from 1 m below the new top.
[[nodiscard]] double sky_altitude() const {
    const double altitude = eye_altitude();
    if (!sky_view_inputs)
        return altitude;
    const double held = sky_view_inputs->altitude;
    const double distance = std::hypot(double(view_origin[0]), double(view_origin[1]), double(view_origin[2]));
    const double tolerance = std::max(sky_altitude_slack, sky_altitude_rounding * distance);
    return std::abs(altitude - held) <= tolerance && inside_atmosphere(held) == held ? held : altitude;
}
// Records the atmosphere's dispatches while it is enabled: the transmittance and multiple scattering tables when the
// medium differs from the one they hold, then the sky view table when they were rebuilt or sky_view_inputs differ,
// then a barrier that makes every write visible to the frame's later shaders. A recording that writes begins, after
// the first, with a barrier that waits for the fragment shaders submitted before it, which include the sky's reads of
// the tables in earlier frames that may still be in flight. A frame that writes no table records nothing: the barrier
// after the tables' last writes reaches every command submitted to the queue after it, so it makes them visible to this
// frame's shaders too, and without a write there is no earlier read to wait for. The atmosphere GPU check's
// report_cost() requires nine tenths or more of the frames that change nothing, and of those that only turn the camera
// 100 km from the world's origin, to take under half the time of frames that redraw the sky view table, and frames
// that raise the eye there by about twice sky_altitude()'s tolerance to redraw it.
void record_atmosphere() {
    const auto &atmosphere = environment.atmosphere;
    if (!atmosphere.enabled)
        return;
    const bool rebuild = !atmosphere_ready || !atmosphere_built || !same_medium(*atmosphere_built, atmosphere);
    const SkyViewInputs sky_inputs{atmosphere.mie_anisotropy, sky_altitude(), normalized(environment.sun.direction).y};
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
    std::array<VkDescriptorSet, ANIMA_SET_ENVIRONMENT + 1> sets{};
    sets[ANIMA_SET_ATMOSPHERE] = atmosphere_set;
    sets[ANIMA_SET_ENVIRONMENT] = frame().environment_set;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, atmosphere_pipeline_layout, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const auto dispatch = [&](AtmosphereTableIndex index, VkExtent2D groups) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, atmosphere_pipelines[index]);
        vkCmdDispatch(command, groups.width, groups.height, 1);
    };
    const auto then = [&] {
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &written, 0, nullptr, 0, nullptr);
    };
    // Square groups of ANIMA_ATMOSPHERE_GROUP_EDGE invocations on an edge, one per texel, except the multiple
    // scattering table's one group of ANIMA_SCATTERING_INVOCATIONS per texel.
    const auto cover = [](VkExtent2D size) {
        constexpr std::uint32_t edge = ANIMA_ATMOSPHERE_GROUP_EDGE;
        return VkExtent2D{(size.width + edge - 1) / edge, (size.height + edge - 1) / edge};
    };
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
