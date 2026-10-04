// Included inside Impl. Set 1, each frame slot's environment set, is shared by world passes; material set 0 and
// palette set 2 stay independent.
Environment environment;
// The sun at the ground, atmosphere_sunlight() of the environment, which set_environment() keeps with it and
// integrates again only when an input of it changes (ground_sunlight()).
Vec3 sunlight = Environment{}.sun.radiance;
// As shaders/environment.glsl lays it out, in std140.
struct EnvironmentUniform {
    std::array<float, 4> sun_direction, sun_radiance, fill_direction, fill_radiance;
    std::array<float, 4> ambient_sky, ambient_ground, ambient_specular;
    // The atmosphere's Rayleigh and Mie scattering with their scale heights, and its Mie absorption with the Mie
    // phase function's asymmetry.
    std::array<float, 4> atmosphere_rayleigh, atmosphere_mie, atmosphere_mie_absorption, fog, controls;
    // The sky's view rays (rays_of()) and the eye.
    Mat4 view_rays;
    std::array<float, 4> view_origin;
    // Each fitted cascade's view-projection, the view depth where it ends, and its constant and slope biases in its
    // normalized depth with the world size of its texels.
    std::array<Mat4, 4> cascade_views;
    std::array<float, 4> cascade_ends;
    std::array<std::array<float, 4>, 4> cascade_biases;
    // The point that view depths are measured from, with the depth where the first cascade begins; the unit view
    // direction, with the share of each cascade that blends into the next; and the number of cascades.
    std::array<float, 4> cascade_origin, cascade_axis, cascades;
    Mat4 detail_shadow_view;
    std::array<float, 4> detail_shadow;
    // The fog's height, falloff, sky distance and phase asymmetry, and fog_sun_scattering times the sun at the ground.
    std::array<float, 4> fog_shape, fog_sun;
    // The atmosphere's ozone with its peak's altitude; the ground's radius, the thickness, the ozone layer's width and
    // the sun's angular radius; the ground's albedo with the eye's altitude; and the sun above the atmosphere, with 1
    // while the atmosphere is enabled.
    std::array<float, 4> atmosphere_ozone, atmosphere_shape, atmosphere_ground, atmosphere_sun;
};
static_assert(offsetof(EnvironmentUniform, view_rays) == 192);
static_assert(offsetof(EnvironmentUniform, view_origin) == 256);
static_assert(offsetof(EnvironmentUniform, cascade_views) == 272 && offsetof(EnvironmentUniform, cascade_ends) == 528 &&
              offsetof(EnvironmentUniform, cascade_biases) == 544 &&
              offsetof(EnvironmentUniform, cascade_origin) == 608 && offsetof(EnvironmentUniform, cascades) == 640);
static_assert(offsetof(EnvironmentUniform, detail_shadow_view) == 656 &&
              offsetof(EnvironmentUniform, detail_shadow) == 720 && offsetof(EnvironmentUniform, fog_shape) == 736 &&
              offsetof(EnvironmentUniform, fog_sun) == 752 && offsetof(EnvironmentUniform, atmosphere_sun) == 816 &&
              sizeof(EnvironmentUniform) == 832);
VkDescriptorSetLayout environment_layout{};
// Holds each frame slot's environment set (FrameSlot::environment_set).
VkDescriptorPool environment_pool{};
Mat4 inverse_view = identity(), view_rays{};
// A matrix that takes a pixel's normalized device coordinates (x, y, 0, 1) to a vector along its ray from the eye of
// @p view_projection, a perspective view whose eye is @p origin: for its inverse Q and the eye e, Q's third column,
// `e.w * Q * n - (Q * n).w * e`, in which the depth's terms cancel. Formed in double precision, so that the eye's
// distance from the world's origin, which cancels in it, costs the rays none of their precision. Zero for an
// orthographic view, whose rays all run along its direction.
static Mat4 rays_of(const Mat4 &view_projection, const std::array<float, 4> &origin) {
    if (origin[3] == 0)
        return {};
    const auto q = detail::inverse_in_double(view_projection);
    const double eye = q[11];
    std::array<double, 16> rays{};
    for (const unsigned column : {0U, 1U, 3U})
        for (unsigned row = 0; row < 3; ++row)
            rays[column * 4 + row] = eye * q[column * 4 + row] - q[column * 4 + 3] * q[8 + row];
    // Toward the near plane's center, whose w is the eye's plus the fourth column's: the vector from the eye to a
    // point is the formula's over both w.
    const double toward = (eye + q[15]) * eye < 0 ? -1 : 1;
    double largest = 0;
    for (const double value : rays)
        largest = std::max(largest, std::abs(value));
    Mat4 result{};
    if (largest > 0)
        for (std::size_t i = 0; i < rays.size(); ++i)
            result[i] = static_cast<float>(toward * rays[i] / largest);
    return result;
}
VkShaderModule sky_vertex_shader{}, sky_fragment_shader{};
VkPipeline sky_pipeline{};
void create_environment_layout() {
    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        // The atmosphere's sky view and transmittance tables, which the sky reads.
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 5;
    layout.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &environment_layout), "Create environment layout");
    // Each set's four image bindings: the cascades, the detail region and the atmosphere's two tables.
    constexpr std::uint32_t environment_images = 4;
    const auto slots = static_cast<std::uint32_t>(frames.size());
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slots},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, environment_images * slots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = slots;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &environment_pool), "Create environment pool");
    for (auto &slot : frames) {
        slot.environment_buffer =
            resource_buffer(sizeof(EnvironmentUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = environment_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &environment_layout;
        check(vkAllocateDescriptorSets(device, &allocate, &slot.environment_set), "Allocate environment set");
        const VkDescriptorBufferInfo buffer{slot.environment_buffer->buffer, 0, sizeof(EnvironmentUniform)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = slot.environment_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}
// Records the sky while the atmosphere is enabled. It lies at the far plane, depth 0, tests depth with
// GREATER_OR_EQUAL and writes none, so recorded after every opaque and masked surface of the pass it shades only the
// pixels that they leave at the far plane, and the frame is the same as if it had drawn first. Binding the environment
// set disturbs the sets bound through resource_pipeline_layout, so it clears resource_bound.
void record_sky() {
    if (!environment.atmosphere.enabled)
        return;
    const auto command = frame().command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, environment_pipeline_layout, 1, 1,
                            &frame().environment_set, 0, nullptr);
    resource_bound = {};
    vkCmdDraw(command, 3, 1, 0, 0);
}
// Whether the view's fog needs the shaders' height fog code: fog whose density falls with height or that scatters
// sunlight. Uniform fog without sunlight, or none, draws through pipelines compiled without it.
[[nodiscard]] bool uses_height_fog() const {
    const auto &e = environment;
    return e.fog_density > 0 && (e.fog_falloff > 0 || e.fog_sun_scattering.x > 0 || e.fog_sun_scattering.y > 0 ||
                                 e.fog_sun_scattering.z > 0);
}
// The fog's height, falloff, sky distance and phase asymmetry, as fog.glsl reads them.
static std::array<float, 4> fog_shape(const Environment &e) {
    return {e.fog_height, e.fog_falloff, e.fog_sky_distance, e.fog_sun_anisotropy};
}
// The sunlight that the fog scatters, before its phase function: fog_sun_scattering times @p ground_sun, the sun at
// the ground, formed in double and capped at the largest float, so that no finite inputs make it infinite.
static std::array<float, 4> fog_sunlight(const Environment &e, Vec3 ground_sun) {
    const auto times = [](float share, float radiance) {
        return float(std::min(double(share) * radiance, double(std::numeric_limits<float>::max())));
    };
    const auto &share = e.fog_sun_scattering;
    return {times(share.x, ground_sun.x), times(share.y, ground_sun.y), times(share.z, ground_sun.z), 0};
}
// The eye's altitude above the atmosphere's ground, kept 1 m inside both the ground and the top; an orthographic view
// sees the sky from 1 m.
double eye_altitude() const {
    const auto &a = environment.atmosphere;
    const double altitude = view_origin[3] != 0 ? double(view_origin[1]) - a.ground_height : 1.0;
    return std::clamp(altitude, 1.0, double(a.thickness) - 1.0);
}
// Writes the environment and the latest fit of the shadow cascades (finish_shadow_cascades()) into the environment
// block of the slot that records the next frame.
void update_environment() {
    const auto vec = [](Vec3 v, float w = 0.F) { return std::array<float, 4>{v.x, v.y, v.z, w}; };
    const auto &e = environment;
    const auto &a = e.atmosphere;
    EnvironmentUniform data{vec(normalized(e.sun.direction)),
                            vec(sunlight),
                            vec(normalized(e.fill.direction)),
                            vec(e.fill.radiance),
                            vec(e.ambient_sky),
                            vec(e.ambient_ground),
                            vec(e.ambient_specular),
                            vec(a.rayleigh_scattering, a.rayleigh_scale_height),
                            vec(a.mie_scattering, a.mie_scale_height),
                            vec(a.mie_absorption, a.mie_anisotropy),
                            vec(e.fog_color, e.fog_density),
                            {e.exposure, e.tone_mapping ? 1.F : 0.F, 0.F, 0.F},
                            view_rays,
                            view_origin,
                            {},
                            {},
                            {},
                            {},
                            {},
                            {},
                            detail_pass.view,
                            {e.detail_shadow.enabled ? 1.F : 0.F, 1.F / static_cast<float>(e.detail_shadow.resolution),
                             e.detail_shadow.constant_bias, e.detail_shadow.slope_bias},
                            fog_shape(e),
                            fog_sunlight(e, sunlight),
                            vec(a.ozone_absorption, a.ozone_altitude),
                            {a.planet_radius, a.thickness, a.ozone_width, a.sun_angular_radius},
                            vec(a.ground_albedo, float(eye_altitude())),
                            vec(e.sun.radiance, a.enabled ? 1.F : 0.F)};
    const auto &settings = e.shadow_cascades;
    // A 16-bit map stores depth in steps of 1/65535, which the bias must also cover.
    const double step = shadow_format == VK_FORMAT_D16_UNORM ? 1. / 65535 : 0;
    for (std::uint32_t i = 0; i < cascade_fits.count; ++i) {
        const auto &fit = cascade_fits.cascades[i];
        const auto texels = fit.texel / cascade_depth[i];
        data.cascade_views[i] = cascade_passes[i].view;
        data.cascade_ends[i] = float(fit.end);
        data.cascade_biases[i] = {float(settings.constant_bias * texels + step), float(settings.slope_bias * texels),
                                  float(fit.texel), 0};
    }
    const auto &origin = cascade_fits.origin, &forward = cascade_fits.forward;
    data.cascade_origin = {float(origin[0]), float(origin[1]), float(origin[2]),
                           cascade_fits.count ? float(cascade_fits.cascades[0].begin) : 0.F};
    data.cascade_axis = {float(forward[0]), float(forward[1]), float(forward[2]), settings.blend};
    data.cascades = {float(cascade_fits.count), 0, 0, 0};
    auto &block = *frame().environment_buffer;
    std::memcpy(block.mapping, &data, sizeof(data));
    flush_resource(block);
}
