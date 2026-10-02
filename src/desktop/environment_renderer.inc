// Included inside Impl. Set 1 is shared by
// world passes; material set 0 and palette set 2 stay independent.
Environment environment;
// As shaders/environment.glsl lays it out, in std140.
struct EnvironmentUniform {
    std::array<float, 4> sun_direction, sun_radiance, fill_direction, fill_radiance;
    std::array<float, 4> ambient_sky, ambient_ground, ambient_specular;
    std::array<float, 4> sky_zenith, sky_horizon, sky_ground, fog, controls;
    Mat4 inverse_view;
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
};
static_assert(offsetof(EnvironmentUniform, inverse_view) == 192);
static_assert(offsetof(EnvironmentUniform, view_origin) == 256);
static_assert(offsetof(EnvironmentUniform, cascade_views) == 272 && offsetof(EnvironmentUniform, cascade_ends) == 528 &&
              offsetof(EnvironmentUniform, cascade_biases) == 544 &&
              offsetof(EnvironmentUniform, cascade_origin) == 608 && offsetof(EnvironmentUniform, cascades) == 640);
static_assert(offsetof(EnvironmentUniform, detail_shadow_view) == 656 &&
              offsetof(EnvironmentUniform, detail_shadow) == 720 && sizeof(EnvironmentUniform) == 736);
VkDescriptorSetLayout environment_layout{};
VkDescriptorPool environment_pool{};
VkDescriptorSet environment_set{};
std::unique_ptr<ResourceBuffer> environment_buffer;
Mat4 inverse_view = identity();
VkShaderModule sky_vertex_shader{}, sky_fragment_shader{};
VkPipeline sky_pipeline{};
void create_environment_layout() {
    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 3;
    layout.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &environment_layout), "Create environment layout");
    environment_buffer = resource_buffer(sizeof(EnvironmentUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &environment_pool), "Create environment pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = environment_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &environment_layout;
    check(vkAllocateDescriptorSets(device, &allocate, &environment_set), "Allocate environment set");
    const VkDescriptorBufferInfo buffer{environment_buffer->buffer, 0, sizeof(EnvironmentUniform)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = environment_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
// Records the sky when the environment has one. It lies at the far plane, depth 0, tests depth with
// GREATER_OR_EQUAL and writes none, so recorded after every opaque and masked surface of the pass it shades only the
// pixels that they leave at the far plane, and the frame is the same as if it had drawn first.
void record_sky() {
    if (!environment.sky)
        return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, environment_pipeline_layout, 1, 1,
                            &environment_set, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
}
// Writes the environment and the latest fit of the shadow cascades (finish_shadow_cascades()) for the next frame.
void update_environment() {
    const auto vec = [](Vec3 v, float w = 0.F) { return std::array<float, 4>{v.x, v.y, v.z, w}; };
    const auto &e = environment;
    EnvironmentUniform data{vec(normalized(e.sun.direction)),
                            vec(e.sun.radiance),
                            vec(normalized(e.fill.direction)),
                            vec(e.fill.radiance),
                            vec(e.ambient_sky),
                            vec(e.ambient_ground),
                            vec(e.ambient_specular),
                            vec(e.sky_zenith),
                            vec(e.sky_horizon),
                            vec(e.sky_ground),
                            vec(e.fog_color, e.fog_density),
                            {e.exposure, e.tone_mapping ? 1.F : 0.F, 0.F, 0.F},
                            inverse_view,
                            view_origin,
                            {},
                            {},
                            {},
                            {},
                            {},
                            {},
                            detail_pass.view,
                            {e.detail_shadow.enabled ? 1.F : 0.F, 1.F / static_cast<float>(e.detail_shadow.resolution),
                             e.detail_shadow.constant_bias, e.detail_shadow.slope_bias}};
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
    std::memcpy(environment_buffer->mapping, &data, sizeof(data));
    flush_resource(*environment_buffer);
}
