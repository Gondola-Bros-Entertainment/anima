// Included inside Impl. Set 1 is shared by
// world passes; material set 0 and palette set 2 stay independent.
Environment environment;
struct EnvironmentUniform {
    std::array<float, 4> sun_direction, sun_radiance, fill_direction, fill_radiance;
    std::array<float, 4> ambient_sky, ambient_ground, ambient_specular;
    std::array<float, 4> sky_zenith, sky_horizon, sky_ground, fog, controls;
    Mat4 inverse_view;
    std::array<float, 4> view_origin;
    Mat4 shadow_view;
    std::array<float, 4> shadow;
    Mat4 detail_shadow_view;
    std::array<float, 4> detail_shadow;
};
static_assert(offsetof(EnvironmentUniform, inverse_view) == 192);
static_assert(offsetof(EnvironmentUniform, view_origin) == 256);
static_assert(offsetof(EnvironmentUniform, shadow_view) == 272);
static_assert(offsetof(EnvironmentUniform, shadow) == 336);
static_assert(offsetof(EnvironmentUniform, detail_shadow_view) == 352 &&
              offsetof(EnvironmentUniform, detail_shadow) == 416 && sizeof(EnvironmentUniform) == 432);
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
void update_environment() {
    const auto vec = [](Vec3 v, float w = 0.F) { return std::array<float, 4>{v.x, v.y, v.z, w}; };
    const auto &e = environment;
    const EnvironmentUniform data{vec(normalized(e.sun.direction)),
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
                                  shadow_view,
                                  {e.shadow.enabled ? 1.F : 0.F, 1.F / static_cast<float>(e.shadow.resolution),
                                   e.shadow.constant_bias, e.shadow.slope_bias},
                                  detail_shadow_view,
                                  {e.detail_shadow.enabled ? 1.F : 0.F,
                                   1.F / static_cast<float>(e.detail_shadow.resolution), e.detail_shadow.constant_bias,
                                   e.detail_shadow.slope_bias}};
    std::memcpy(environment_buffer->mapping, &data, sizeof(data));
    flush_resource(*environment_buffer);
}
